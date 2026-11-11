#pragma once

// SONORA Master v0.2 "Rap Overhaul" — SDK-independent, header-only mastering engine.
//
// Chains (all modes run the same per-sample pipeline; the mode only changes smoothed stage
// targets, so mode/bypass switches are click-free and latency is constant):
//   Vocal : in -> HPF 80 -> EQ/nasal/resonance -> de-esser -> presence -> vocal compressor
//           -> [4x OS: saturation -> safety soft clip] -> output gain
//   Beat  : in -> HPF 20 -> EQ/bass/beat-highs -> vocal-sidechain ducking -> 808-Punch -> glue
//           -> width/bass-mono -> [4x OS: saturation] -> output gain
//   Master: Beat chain + de-esser -> [4x OS: saturation -> Lautheit drive -> soft clipper]
//           -> trim (Ausgang) -> look-ahead true-peak limiter -> ceiling
// Note: width/bass-mono runs before the oversampled saturation so all nonlinear stages share one
// oversampler (lower latency); with bass-mono this also keeps saturated lows mono.
//
// Real-time safety: all memory is allocated in prepare(); process() never allocates or locks.
// Block-size invariance: everything is processed per sample; coefficient updates happen on an
// internal 16-sample control counter that persists across process() calls.

#include "dsp_common.h"
#include "loudness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sonora::dsp {
namespace master {

enum ParamId : int {
    kMode = 0, kAmount = 1, kBass = 2, kPresence = 3, kOutput = 4, kBypass = 5, kPreset = 6,
    kNasal = 7, kResonance = 8, kBeatHighs = 9, kSaturation = 10, kEq60 = 11, kEq250 = 12,
    kEq1k = 13, kEq4k = 14, kEq12k = 15, kVocalFocus = 16, kAnalyze = 17, kLoudness = 18,
    kCeiling = 19, kGlue = 20, kClipper = 21, kPunch808 = 22, kBassMono = 23, kWidth = 24,
    kDeEsser = 25, kInput = 26, kNumParams = 27
};

enum Mode : int { kModeVocal = 0, kModeBeat = 1, kModeMaster = 2 };

// Same numeric values as v0.1 (0..6); 7 is new: Master mode without sidechain (mix-only).
enum AnalysisStatus : int {
    kAnalysisReady = 0, kAnalysisRunning = 1, kAnalysisComplete = 2, kAnalysisNoVocal = 3,
    kAnalysisMissingSidechain = 4, kAnalysisWrongMode = 5, kAnalysisTooQuiet = 6,
    kAnalysisCompleteMixOnly = 7
};

// stepCount: 0 = continuous, 1 = toggle, n = list with n + 1 entries (VST3 convention).
struct ParamInfo {
    const char* name; // UTF-8 (German)
    const char* unit;
    double minValue, maxValue, defaultValue;
    int stepCount;
};

inline constexpr std::array<ParamInfo, kNumParams> kParams{{
    {"Bus-Modus", "", 0.0, 2.0, 2.0, 2},
    {"Stärke", "%", 0.0, 100.0, 50.0, 0},
    {"Bass", "dB", -6.0, 6.0, 0.0, 0},
    {"Stimmen-Präsenz", "dB", -4.0, 4.0, 0.0, 0},
    {"Ausgang", "dB", -6.0, 6.0, 0.0, 0},
    {"Bypass", "", 0.0, 1.0, 0.0, 1},
    {"Preset", "", 0.0, 11.0, 0.0, 11},
    {"Nasalität", "%", 0.0, 100.0, 0.0, 0},
    {"Resonanzen", "%", 0.0, 100.0, 0.0, 0},
    {"Beat-Höhen", "dB", -6.0, 6.0, 0.0, 0},
    {"Sättigung", "%", 0.0, 100.0, 0.0, 0},
    {"EQ 60 Hz", "dB", -8.0, 8.0, 0.0, 0},
    {"EQ 250 Hz", "dB", -8.0, 8.0, 0.0, 0},
    {"EQ 1 kHz", "dB", -8.0, 8.0, 0.0, 0},
    {"EQ 4 kHz", "dB", -8.0, 8.0, 0.0, 0},
    {"EQ 12 kHz", "dB", -8.0, 8.0, 0.0, 0},
    {"Vocal-Fokus", "%", 0.0, 100.0, 50.0, 0},
    {"Analyse starten", "", 0.0, 1.0, 0.0, 1},
    {"Lautheit", "dB", 0.0, 18.0, 0.0, 0},
    {"Ceiling", "dBTP", -3.0, 0.0, -1.0, 0},
    {"Glue", "%", 0.0, 100.0, 0.0, 0},
    {"Clipper", "%", 0.0, 100.0, 0.0, 0},
    {"808-Punch", "%", 0.0, 100.0, 0.0, 0},
    {"Bass-Mono", "Hz", 0.0, 250.0, 0.0, 0},
    {"Stereo-Breite", "%", 0.0, 200.0, 100.0, 0},
    {"De-Esser", "%", 0.0, 100.0, 0.0, 0},
    {"Eingang", "dB", -12.0, 12.0, 0.0, 0},
}};

inline double toPlain(int id, double normalized) {
    if (id < 0 || id >= kNumParams) return 0.0;
    const auto& p = kParams[static_cast<std::size_t>(id)];
    double n = clamp01(normalized);
    if (p.stepCount > 0) n = std::round(n * p.stepCount) / p.stepCount;
    return p.minValue + n * (p.maxValue - p.minValue);
}

inline double toNormalized(int id, double plain) {
    if (id < 0 || id >= kNumParams) return 0.0;
    const auto& p = kParams[static_cast<std::size_t>(id)];
    if (!std::isfinite(plain)) plain = p.defaultValue;
    double n = (std::clamp(plain, p.minValue, p.maxValue) - p.minValue) / (p.maxValue - p.minValue);
    if (p.stepCount > 0) n = std::round(n * p.stepCount) / p.stepCount;
    return n;
}

inline double defaultNormalized(int id) {
    return id >= 0 && id < kNumParams ? toNormalized(id, kParams[static_cast<std::size_t>(id)].defaultValue) : 0.0;
}

constexpr int kNumPresets = 12;
constexpr int kLegacyPresetCount = 6;

// v0.1 stored the preset as a 6-entry list normalization; map it onto the 12-entry list.
inline double migrateLegacyPresetNormalized(double legacyNormalized) {
    const double index = std::round(clamp01(legacyNormalized) * (kLegacyPresetCount - 1));
    return index / (kNumPresets - 1);
}

// Character macros at Stärke = 100 %; scaled by Stärke and added on top of the user knobs.
struct Preset {
    const char* name;
    std::array<double, 5> eqDb; // 60 / 250 / 1k / 4k / 12k
    double presenceDb;          // added to Stimmen-Präsenz
    double nasal, resonance;    // 0..1 added to the knobs
    double glue, clipper, punch, deEsser, saturation; // 0..1 added
    double widthDelta;          // added to Stereo-Breite (1.0 = +100 %)
    double driveDb;             // loudness push into clipper/limiter (Master mode only)
    double bassMonoHz;          // bass-mono frequency at 100 % (max with the knob)
};

inline constexpr std::array<Preset, kNumPresets> kPresets{{
    // v0.1 character presets (same EQ curves as v0.1, mild dynamics, little/no loudness push)
    {"Natural",       {{0.0, 0.0, 0.0, 0.0, 0.0}},   0.0, 0.0, 0.0, 0.15, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {"Warm",          {{0.2, 0.8, 0.0, -0.5, -0.5}}, 0.0, 0.0, 0.0, 0.20, 0.0, 0.05, 0.0, 0.20, 0.0, 0.5, 0.0},
    {"Punch",         {{0.2, 0.0, 0.0, 0.5, 0.0}},   0.0, 0.0, 0.0, 0.30, 0.10, 0.15, 0.0, 0.05, 0.0, 1.5, 0.0},
    {"Air",           {{-0.4, -0.2, 0.0, 0.2, 0.7}}, 0.0, 0.0, 0.0, 0.15, 0.0, 0.0, 0.05, 0.0, 0.05, 0.5, 0.0},
    {"Bass",          {{0.6, 0.4, -0.3, 0.0, -0.2}}, 0.0, 0.0, 0.0, 0.20, 0.0, 0.15, 0.0, 0.05, 0.0, 1.0, 60.0},
    {"Vocal Clarity", {{-0.3, -0.4, 0.2, 0.4, 0.2}}, 0.5, 0.15, 0.15, 0.15, 0.0, 0.0, 0.15, 0.0, 0.0, 0.5, 0.0},
    // v0.2 rap presets
    {"Trap 808",      {{1.5, -1.2, 0.0, 1.6, 1.8}}, 1.0, 0.0, 0.10, 0.45, 0.65, 0.80, 0.30, 0.25, 0.15, 5.6, 120.0},
    {"Drill",         {{1.0, -1.5, -0.2, 1.8, 1.5}}, 1.2, 0.0, 0.15, 0.50, 0.70, 0.65, 0.35, 0.30, 0.10, 5.9, 140.0},
    {"Boom Bap",      {{0.8, 0.6, 0.4, 0.6, -0.6}},  0.3, 0.0, 0.0, 0.65, 0.40, 0.35, 0.20, 0.50, -0.05, 6.5, 100.0},
    {"Cloud Rap",     {{1.0, -0.6, -0.5, 0.4, 2.5}}, 0.5, 0.0, 0.10, 0.35, 0.30, 0.45, 0.30, 0.15, 0.35, 5.5, 100.0},
    {"Vocal Upfront", {{-0.5, -1.5, 0.5, 1.2, 1.0}}, 2.0, 0.30, 0.30, 0.40, 0.35, 0.20, 0.50, 0.15, 0.05, 6.0, 100.0},
    {"Club Loud",     {{1.5, -1.0, 0.2, 1.5, 1.5}},  1.0, 0.0, 0.10, 0.55, 0.75, 0.60, 0.35, 0.30, 0.10, 7.3, 150.0},
}};

constexpr int kBands = 5;
inline constexpr std::array<double, kBands> kBandFrequencies{{60.0, 250.0, 1000.0, 4000.0, 12000.0}};

// Plain POD snapshot written at the end of every process() call. The plugin glue copies it into
// its atomics/meter bridge. Peaks are linear, everything else as named.
struct MasterMeters {
    float inputPeak[2] = {0.0f, 0.0f};  // block max |x| (raw input)
    float outputPeak[2] = {0.0f, 0.0f}; // block max |y|
    float outputTruePeakDb = -120.0f;   // block true peak of the output (4x)
    float truePeakMaxDb = -120.0f;      // held since resetLoudness()
    float momentaryLufs = -120.0f, shortTermLufs = -120.0f, integratedLufs = -120.0f;
    float glueGrDb = 0.0f;     // >= 0 (glue in Beat/Master, vocal compressor in Vocal mode), block max
    float limiterGrDb = 0.0f;  // >= 0, block max
    float clipGrDb = 0.0f;     // >= 0, block max reduction of the clipper
    float clipActivity = 0.0f; // 0..1
    float deEsserGrDb = 0.0f;  // >= 0
    float duckDb = 0.0f;       // >= 0, current sidechain ducking depth (max of both bands)
    float correlation = 1.0f;  // -1..+1 (output, ~300 ms)
    double autoEqDb[kBands] = {0.0, 0.0, 0.0, 0.0, 0.0};
    int analysisStatus = kAnalysisReady;
    double analysisProgress = 0.0;
    double beatBandsDb[kBands] = {-120.0, -120.0, -120.0, -120.0, -120.0};
    double vocalBandsDb[kBands] = {-120.0, -120.0, -120.0, -120.0, -120.0};
    double beatRmsDb = -120.0, vocalRmsDb = -120.0, beatCrestDb = 0.0;
    double autoCompRatio = 1.0;
    int latencySamples = 0;
};

} // namespace master

// ------------------------------------------------------------------------------------------------
// Building blocks

namespace detail {

inline double fastLog2Core(double m) { // m in [0.707, 1.414]
    const double t = (m - 1.0) / (m + 1.0), t2 = t * t;    // |t| <= 0.1716
    return 2.8853900817779268 * t * (1.0 + t2 * (1.0 / 3.0 + t2 * (0.2 + t2 * (1.0 / 7.0 + t2 * (1.0 / 9.0 + t2 * (1.0 / 11.0))))));
}
// Fast, smooth transcendental approximations (relative error < 2e-7) for per-sample paths.
inline double fastExp2(double y) {
    y = std::clamp(y, -1000.0, 1000.0);
    std::int64_t ii = static_cast<std::int64_t>(y);
    if (static_cast<double>(ii) > y) --ii;
    const double fi = static_cast<double>(ii);
    const double g = (y - fi - 0.5) * 0.69314718055994531; // |g| <= 0.347
    const double p = 1.0 + g * (1.0 + g * (0.5 + g * (1.0 / 6.0 + g * (1.0 / 24.0 + g * (1.0 / 120.0 + g * (1.0 / 720.0 + g * (1.0 / 5040.0)))))));
    const std::int64_t e = ii + 1023;
    const std::uint64_t bits = static_cast<std::uint64_t>(e) << 52;
    double scale;
    std::memcpy(&scale, &bits, sizeof(scale));
    return p * 1.4142135623730951 * scale;
}
inline double fastLog2(double x) { // x > 0, normal
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const int e = static_cast<int>((bits >> 52) & 0x7FF) - 1023;
    bits = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull; // mantissa in [1, 2)
    double m;
    std::memcpy(&m, &bits, sizeof(m));
    if (m > 1.4142135623730951) { m *= 0.5; return fastLog2Core(m) + (e + 1); }
    return fastLog2Core(m) + e;
}
inline double fastDbToGain(double db) { return fastExp2(db * 0.16609640474436813); } // log2(10)/20
inline double fastGainToDb(double g, double floorGain = 1e-12) {
    return 6.0205999132796239 * fastLog2(std::max(g, floorGain)); // 20*log10(2)
}
inline double fastTanh(double x) {
    if (x > 19.0) return 1.0;
    if (x < -19.0) return -1.0;
    if (std::abs(x) < 1e-4) return x * (1.0 - x * x * (1.0 / 3.0));
    const double e = fastExp2(2.8853900817779268 * x); // e^(2x)
    return (e - 1.0) / (e + 1.0);
}

// Soft-knee clipper: linear below knee = T * (1 - k); tanh-shaped (C2) approach to T above.
inline double softClip(double x, double threshold, double kneeFraction) {
    const double a = threshold * (1.0 - kneeFraction);
    const double ax = std::abs(x);
    if (ax <= a) return x;
    const double span = threshold - a;
    const double y = a + span * fastTanh((ax - a) / span);
    return x < 0.0 ? -y : y;
}

struct OnePole { // exponential smoother that snaps exactly onto its target
    double value = 0.0, target = 0.0, coefficient = 0.0;
    void setTime(double seconds, double rate) { coefficient = timeCoefficient(seconds, rate); }
    void snap(double v) { value = target = v; }
    double next() {
        if (value != target) {
            value = target + (value - target) * coefficient;
            if (std::abs(value - target) < 1e-9) value = target;
        }
        return value;
    }
};

struct LinearRamp { // linear fade that reaches exactly 0 / 1
    double value = 0.0, target = 0.0, step = 0.001;
    void setTime(double seconds, double rate) { step = 1.0 / std::max(1.0, seconds * rate); }
    void snap(double v) { value = target = v; }
    double next() {
        if (value < target) value = std::min(target, value + step);
        else if (value > target) value = std::max(target, value - step);
        return value;
    }
};

// 2x halfband FIR resampler (Kaiser windowed sinc). C must be even so the even polyphase branch
// is a pure delay; up+down group delay is exactly C samples at the lower rate.
template <int C>
class Halfband2x {
    static_assert(C % 2 == 0, "halfband centre must be even");
public:
    explicit Halfband2x(double beta = 8.0) {
        double sum = 0.0;
        for (int k = 0; k < C; ++k) {
            const int j = 2 * k + 1;
            odd_[static_cast<std::size_t>(k)] = 0.5 * sincPi((j - C) / 2.0) * kaiserWindow(j, 2 * C + 1, beta);
            sum += odd_[static_cast<std::size_t>(k)];
        }
        for (auto& c : odd_) c *= 0.5 / sum; // odd branch sums to 0.5 (unity DC gain per phase)
        reset();
    }
    void reset() {
        upHist_.fill(0.0); upPos_ = 0;
        evenHist_.fill(0.0); evenPos_ = 0;
        oddHist_.fill(0.0); oddPos_ = 0;
    }
    // One stereo input sample at rate R -> two output samples at 2R.
    void up(F2 x, F2& y0, F2& y1) {
        upPos_ = upPos_ == 0 ? C - 1 : upPos_ - 1;
        x.store(&upHist_[static_cast<std::size_t>(2 * upPos_)]);
        x.store(&upHist_[static_cast<std::size_t>(2 * (upPos_ + C))]);
        const double* h = &upHist_[static_cast<std::size_t>(2 * upPos_)]; // pair k = x[n-k]
        y0 = F2::load(h + 2 * (C / 2));
        y1 = F2::splat(2.0) * foldedDot(h);
    }
    // Two stereo input samples at 2R (v[2n], v[2n+1]) -> one output sample at R.
    F2 down(F2 v0, F2 v1) {
        evenPos_ = evenPos_ == 0 ? kEven - 1 : evenPos_ - 1;
        v0.store(&evenHist_[static_cast<std::size_t>(2 * evenPos_)]);
        v0.store(&evenHist_[static_cast<std::size_t>(2 * (evenPos_ + kEven))]);
        const double* o = &oddHist_[static_cast<std::size_t>(2 * oddPos_)]; // pair q = v[2(n-1-q)+1]
        const F2 y = F2::splat(0.5) * F2::load(&evenHist_[static_cast<std::size_t>(2 * (evenPos_ + C / 2))]) + foldedDot(o);
        oddPos_ = oddPos_ == 0 ? C - 1 : oddPos_ - 1;
        v1.store(&oddHist_[static_cast<std::size_t>(2 * oddPos_)]);
        v1.store(&oddHist_[static_cast<std::size_t>(2 * (oddPos_ + C))]);
        return y;
    }

private:
    // sum_k odd_[k] * v[k] using the coefficient symmetry odd_[k] == odd_[C-1-k]; two
    // accumulators break the floating-point add dependency chain.
    F2 foldedDot(const double* v) const {
        F2 a0 = F2::splat(0.0), a1 = F2::splat(0.0);
        int k = 0;
        for (; k + 1 < C / 2; k += 2) {
            a0 = a0 + F2::splat(odd_[static_cast<std::size_t>(k)]) * (F2::load(v + 2 * k) + F2::load(v + 2 * (C - 1 - k)));
            a1 = a1 + F2::splat(odd_[static_cast<std::size_t>(k + 1)]) * (F2::load(v + 2 * (k + 1)) + F2::load(v + 2 * (C - 2 - k)));
        }
        for (; k < C / 2; ++k) a0 = a0 + F2::splat(odd_[static_cast<std::size_t>(k)]) * (F2::load(v + 2 * k) + F2::load(v + 2 * (C - 1 - k)));
        return a0 + a1;
    }
    static constexpr int kEven = C / 2 + 1;
    std::array<double, C> odd_{};
    std::array<double, 4 * C> upHist_{};
    std::array<double, 4 * kEven> evenHist_{};
    std::array<double, 4 * C> oddHist_{};
    int upPos_ = 0, evenPos_ = 0, oddPos_ = 0;
};

// Steep 2:1 decimation lowpass (2M+1 taps, Kaiser-windowed sinc, symmetric). Unlike a halfband,
// whose -6 dB point sits exactly at the output Nyquist frequency, it passes up to ~0.43 fs and
// rejects everything from 0.5 fs (output rate) by > 75 dB, so nonlinear products above the
// output Nyquist cannot fold back into the 20-24 kHz region (where they would also fool a 4x
// true-peak detector).
template <int M>
class Decimator2x {
public:
    static constexpr int kTaps = 2 * M + 1;
    Decimator2x() {
        const double fc = 0.2325, beta = 7.9; // cutoff in cycles/sample at the input (2x) rate
        double sum = 0.0;
        for (int k = 0; k < kTaps; ++k) {
            const double h = 2.0 * fc * sincPi(2.0 * fc * (k - M)) * kaiserWindow(k, kTaps, beta);
            if (k <= M) h_[static_cast<std::size_t>(k)] = h;
            sum += h;
        }
        for (auto& c : h_) c /= sum;
        reset();
    }
    void reset() { hist_.fill(0.0); pos_ = 0; }
    // Two stereo samples at 2R -> one at R.
    F2 down(F2 v0, F2 v1) {
        push(v0);
        push(v1);
        const double* x = &hist_[static_cast<std::size_t>(2 * pos_)]; // pair k = v[n-k]
        F2 a0 = F2::splat(h_[static_cast<std::size_t>(M)]) * F2::load(x + 2 * M), a1 = F2::splat(0.0);
        int k = 0;
        for (; k + 1 < M; k += 2) {
            a0 = a0 + F2::splat(h_[static_cast<std::size_t>(k)]) * (F2::load(x + 2 * k) + F2::load(x + 2 * (kTaps - 1 - k)));
            a1 = a1 + F2::splat(h_[static_cast<std::size_t>(k + 1)]) * (F2::load(x + 2 * (k + 1)) + F2::load(x + 2 * (kTaps - 2 - k)));
        }
        for (; k < M; ++k) a0 = a0 + F2::splat(h_[static_cast<std::size_t>(k)]) * (F2::load(x + 2 * k) + F2::load(x + 2 * (kTaps - 1 - k)));
        return a0 + a1;
    }

private:
    void push(F2 v) {
        pos_ = pos_ == 0 ? kTaps - 1 : pos_ - 1;
        v.store(&hist_[static_cast<std::size_t>(2 * pos_)]);
        v.store(&hist_[static_cast<std::size_t>(2 * (pos_ + kTaps))]);
    }
    std::array<double, M + 1> h_{};
    std::array<double, 4 * kTaps> hist_{};
    int pos_ = 0;
};

// 4x oversampler. Up: 48k->96k (61-tap halfband) and 96k->192k (21-tap halfband). Down:
// 192k->96k (21-tap halfband, its fold-over region lies above 40 kHz) and 96k->48k with the
// steep 143-tap decimator.
class Oversampler4x {
public:
    // Delays at the 2x rate: up halfband 30, 4x stage round trip 10, decimator 70 (its centre tap
    // M = 71 counted from the newest odd sample) -> 110 / 2 = 55 base-rate samples.
    static constexpr int kLatency = 55;
    Oversampler4x() : a_(8.0), b_(7.0) {}
    void reset() { a_.reset(); b_.reset(); d_.reset(); }
    void up(F2 x, F2* y4) {
        F2 m0, m1;
        a_.up(x, m0, m1);
        b_.up(m0, y4[0], y4[1]);
        b_.up(m1, y4[2], y4[3]);
    }
    F2 down(const F2* y4) {
        const F2 m0 = b_.down(y4[0], y4[1]);
        const F2 m1 = b_.down(y4[2], y4[3]);
        return d_.down(m0, m1);
    }
private:
    Halfband2x<30> a_;
    Halfband2x<10> b_;
    Decimator2x<71> d_;
};

// Stereo-linked feed-forward compressor with sidechain HPF, RMS/peak hybrid detection, soft
// knee and program-dependent (dual) release.
class Compressor {
public:
    struct Config {
        double attackMs, releaseFastMs, releaseSlowMs, rmsMs, peakReleaseMs, hpfHz, rmsWeight;
    };
    void prepare(double sr, const Config& c) {
        sr_ = sr;
        cfg_ = c;
        attack_ = timeCoefficient(c.attackMs * 1e-3, sr);
        relFast_ = timeCoefficient(c.releaseFastMs * 1e-3, sr);
        relSlow_ = timeCoefficient(c.releaseSlowMs * 1e-3, sr);
        rms_ = timeCoefficient(c.rmsMs * 1e-3, sr);
        peakRel_ = timeCoefficient(c.peakReleaseMs * 1e-3, sr);
        avg_ = timeCoefficient(0.5, sr);
        const auto h = Biquad::highPass(sr, c.hpfHz, 0.7071);
        hpf_.setCoefficients(h);
        reset();
    }
    void reset() {
        hpf_.reset();
        rmsState_ = 0.0; peakState_ = 0.0; grEnv_ = 0.0; grAvg_ = 0.0;
    }
    static double staticGainReductionDb(double levelDb, double thresholdDb, double ratio, double kneeDb) {
        const double slope = 1.0 - 1.0 / std::max(1.0, ratio);
        const double over = levelDb - thresholdDb;
        if (2.0 * over <= -kneeDb) return 0.0;
        if (2.0 * std::abs(over) < kneeDb) {
            const double t = over + kneeDb * 0.5;
            return slope * t * t / (2.0 * kneeDb);
        }
        return slope * over;
    }
    // Feed one frame; returns the linear gain to apply (without makeup).
    double process(const double* x, int channels, double thresholdDb, double ratio, double kneeDb) {
        double sq = 0.0, pk = 0.0;
        const F2 hp = hpf_.process(F2::make(x[0], channels >= 2 ? x[1] : 0.0));
        double hs[2];
        hp.store(hs);
        for (int c = 0; c < channels; ++c) {
            const double s = hs[c];
            sq += s * s;
            pk = std::max(pk, std::abs(s));
        }
        sq /= channels;
        rmsState_ = sq + (rmsState_ - sq) * rms_;
        peakState_ = pk > peakState_ ? pk : pk + (peakState_ - pk) * peakRel_;
        if (ratio <= 1.0 + 1e-9 && grEnv_ == 0.0) return 1.0;
        const double rmsDb = 0.5 * fastGainToDb(2.0 * rmsState_ + 1e-20, 1e-20); // sine-peak equivalent
        const double peakDb = fastGainToDb(peakState_ + 1e-10);
        const double level = cfg_.rmsWeight * rmsDb + (1.0 - cfg_.rmsWeight) * peakDb;
        const double target = staticGainReductionDb(level, thresholdDb, ratio, kneeDb);
        if (target > grEnv_) {
            grEnv_ = target + (grEnv_ - target) * attack_;
        } else {
            const double coef = grEnv_ > grAvg_ + 2.0 ? relFast_ : relSlow_;
            grEnv_ = target + (grEnv_ - target) * coef;
        }
        grAvg_ = grEnv_ + (grAvg_ - grEnv_) * avg_;
        if (grEnv_ < 1e-7 && target == 0.0) { grEnv_ = 0.0; grAvg_ = sanitize(grAvg_); return 1.0; }
        return fastDbToGain(-grEnv_);
    }
    double gainReductionDb() const { return grEnv_; }
    void flush() {
        hpf_.flush();
        rmsState_ = sanitize(rmsState_);
        peakState_ = sanitize(peakState_);
        grEnv_ = sanitize(grEnv_);
        grAvg_ = sanitize(grAvg_);
    }

private:
    double sr_ = 48000.0;
    Config cfg_{};
    double attack_ = 0, relFast_ = 0, relSlow_ = 0, rms_ = 0, peakRel_ = 0, avg_ = 0;
    FastBiquad2 hpf_{};
    double rmsState_ = 0, peakState_ = 0, grEnv_ = 0, grAvg_ = 0;
};

// Look-ahead brickwall limiter with 4x true-peak detection, stereo link, peak-hold + two cascaded
// moving averages (smooth S-shaped attack over the look-ahead window, guaranteed to reach the
// required reduction before the peak), and program-dependent release in the dB domain.
// 8x inter-sample peak detector for the limiter (stereo pair, 32-tap Kaiser sinc per phase, beta 6).
// Flat (< 0.03 dB) to 21 kHz at 48 kHz with a worst-case 8x grid error of ~0.13 dB, so the ceiling
// holds against a high-precision reference and not only against a BS.1770 4x meter (whose short
// kernel under-reads dense 18-22 kHz content by up to several dB). It also evaluates the
// BS.1770-4 4x kernel (TruePeakInterpolator4x) on the same, aligned history and returns the max of
// both, because that short kernel can over-read dense transients by ~0.5 dB and the output must stay
// under the ceiling on standard meters too. Peak points lie between x[n-16] and x[n-15] -> kLatency = 16.
class TruePeakDetector8x {
public:
    static constexpr int kTaps = 32;
    static constexpr int kHalf = kTaps / 2;
    static constexpr int kLatency = kHalf;
    TruePeakDetector8x() {
        std::array<std::array<double, kTaps>, 4> c{}; // phases 1..4 (of 8)
        for (int p = 1; p <= 4; ++p) {
            auto& row = c[static_cast<std::size_t>(p - 1)];
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k) {
                const double t = k - kHalf + p / 8.0;
                const double r = t / (kHalf + 0.5);
                const double w = besselI0(6.0 * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(6.0);
                row[static_cast<std::size_t>(k)] = sincPi(t) * w;
                sum += row[static_cast<std::size_t>(k)];
            }
            for (auto& v : row) v /= sum;
        }
        // Phase 8-p mirrors phase p (c_{8-p}[k] = c_p[kTaps-1-k]); phase 4 is symmetric.
        for (std::size_t p = 0; p < 3; ++p) {
            for (std::size_t k = 0; k < kHalf; ++k) {
                sum_[p][k] = 0.5 * (c[p][k] + c[p][kTaps - 1 - k]);
                diff_[p][k] = 0.5 * (c[p][k] - c[p][kTaps - 1 - k]);
            }
        }
        for (std::size_t k = 0; k < kHalf; ++k) mid_[k] = c[3][k];
        // BS.1770 4x kernel (identical construction to TruePeakInterpolator4x), 12 taps per phase.
        std::array<double, 49> h{};
        for (int j = 0; j < 49; ++j) h[static_cast<std::size_t>(j)] = sincPi((j - 24) / 4.0) * kaiserWindow(j, 49, 7.5);
        std::array<std::array<double, 12>, 2> c4{};
        for (int p = 1; p <= 2; ++p) {
            double sum = 0.0;
            for (int k = 0; k < 12; ++k) sum += h[static_cast<std::size_t>(4 * k + p)];
            for (int k = 0; k < 12; ++k) c4[static_cast<std::size_t>(p - 1)][static_cast<std::size_t>(k)] = h[static_cast<std::size_t>(4 * k + p)] / sum;
        }
        for (std::size_t k = 0; k < 6; ++k) {
            sum4_[k] = 0.5 * (c4[0][k] + c4[0][11 - k]);
            diff4_[k] = 0.5 * (c4[0][k] - c4[0][11 - k]);
            mid4_[k] = c4[1][k];
        }
        reset();
    }
    void reset() { hist_.fill(0.0); pos_ = 0; }
    double pushAndPeak(double l, double r) {
        pos_ = pos_ == 0 ? kTaps - 1 : pos_ - 1;
        const F2 in = F2::make(l, r);
        in.store(&hist_[static_cast<std::size_t>(2 * pos_)]);
        in.store(&hist_[static_cast<std::size_t>(2 * (pos_ + kTaps))]);
        const double* h = &hist_[static_cast<std::size_t>(2 * pos_)];
        F2 sv[kHalf], dv[kHalf];
        for (int k = 0; k < kHalf; ++k) {
            const F2 a = F2::load(h + 2 * k), b = F2::load(h + 2 * (kTaps - 1 - k));
            sv[k] = a + b;
            dv[k] = a - b;
        }
        F2 peak = F2::absv(F2::load(h + 2 * kLatency));
        for (std::size_t p = 0; p < 3; ++p) {
            F2 s0 = F2::splat(0.0), s1 = F2::splat(0.0), d0 = F2::splat(0.0), d1 = F2::splat(0.0);
            for (std::size_t k = 0; k < kHalf; k += 2) {
                s0 = s0 + F2::splat(sum_[p][k]) * sv[k];
                s1 = s1 + F2::splat(sum_[p][k + 1]) * sv[k + 1];
                d0 = d0 + F2::splat(diff_[p][k]) * dv[k];
                d1 = d1 + F2::splat(diff_[p][k + 1]) * dv[k + 1];
            }
            const F2 ss = s0 + s1, dd = d0 + d1;
            peak = F2::maxv(peak, F2::maxv(F2::absv(ss + dd), F2::absv(ss - dd)));
        }
        F2 m0 = F2::splat(0.0), m1 = F2::splat(0.0);
        for (std::size_t k = 0; k < kHalf; k += 2) {
            m0 = m0 + F2::splat(mid_[k]) * sv[k];
            m1 = m1 + F2::splat(mid_[k + 1]) * sv[k + 1];
        }
        peak = F2::maxv(peak, F2::absv(m0 + m1));
        // 4x kernel centred on the same point: taps x[n-10] .. x[n-21].
        const double* h4 = h + 2 * (kHalf - 6);
        F2 s4 = F2::splat(0.0), d4 = F2::splat(0.0), y2 = F2::splat(0.0);
        for (std::size_t k = 0; k < 6; ++k) {
            const F2 a = F2::load(h4 + 2 * k), b = F2::load(h4 + 2 * (11 - k));
            const F2 sm = a + b;
            s4 = s4 + F2::splat(sum4_[k]) * sm;
            d4 = d4 + F2::splat(diff4_[k]) * (a - b);
            y2 = y2 + F2::splat(mid4_[k]) * sm;
        }
        peak = F2::maxv(peak, F2::maxv(F2::absv(s4 + d4), F2::absv(s4 - d4)));
        peak = F2::maxv(peak, F2::absv(y2));
        return peak.hmax();
    }

private:
    std::array<std::array<double, kHalf>, 3> sum_{}, diff_{};
    std::array<double, kHalf> mid_{};
    std::array<double, 6> sum4_{}, diff4_{}, mid4_{};
    std::array<double, 4 * kTaps> hist_{};
    int pos_ = 0;
};

class TruePeakLimiter {
public:
    void prepare(double sr) {
        window_ = std::max(4, static_cast<int>(std::lround(0.00135 * sr)));
        hold_ = window_ + 1;
        box1Len_ = window_ / 2;
        box2Len_ = window_ - box1Len_ + 1;
        invBox1_ = 1.0 / box1Len_;
        invBox2_ = 1.0 / box2Len_;
        delay_ = window_ - 1 + TruePeakDetector8x::kLatency;
        for (auto& d : audio_) d.prepare(delay_ + 1);
        dqIndex_.assign(static_cast<std::size_t>(hold_ + 2), 0);
        dqValue_.assign(static_cast<std::size_t>(hold_ + 2), 0.0);
        box1_.assign(static_cast<std::size_t>(box1Len_), 0.0);
        box2_.assign(static_cast<std::size_t>(box2Len_), 0.0);
        relFast_ = timeCoefficient(0.045, sr);
        relSlow_ = timeCoefficient(0.350, sr);
        avg_ = timeCoefficient(0.300, sr);
        reset();
    }
    void reset() {
        for (auto& d : audio_) d.reset();
        tp_.reset();
        dqHead_ = dqCount_ = 0;
        counter_ = 0;
        std::fill(box1_.begin(), box1_.end(), 0.0);
        std::fill(box2_.begin(), box2_.end(), 0.0);
        pos1_ = pos2_ = 0;
        sum1_ = sum2_ = 0.0;
        rel_ = 0.0; relAvg_ = 0.0; grDb_ = 0.0;
    }
    int latency() const { return delay_; }

    // In-place on x[0..channels-1]. mix 0 = pure delay, 1 = limited. Returns applied GR dB.
    double process(double* x, int channels, double ceilingLin, double mix) {
        const double p = tp_.pushAndPeak(x[0], channels >= 2 ? x[1] : 0.0);
        for (int c = 0; c < channels; ++c) audio_[static_cast<std::size_t>(c)].push(x[c]);
        const double required = p > ceilingLin ? fastGainToDb(p / ceilingLin) : 0.0;
        const double held = pushMax(required);
        if (held >= rel_) {
            rel_ = held;
        } else {
            const double coef = rel_ > relAvg_ + 1.0 ? relFast_ : relSlow_;
            rel_ = held + (rel_ - held) * coef;
            if (rel_ < 1e-9) rel_ = 0.0;
        }
        relAvg_ = sanitize(rel_ + (relAvg_ - rel_) * avg_);
        sum1_ += rel_ - box1_[static_cast<std::size_t>(pos1_)];
        box1_[static_cast<std::size_t>(pos1_)] = rel_;
        if (++pos1_ >= box1Len_) pos1_ = 0;
        if (sum1_ < 1e-9) sum1_ = 0.0;
        const double a1 = sum1_ * invBox1_;
        sum2_ += a1 - box2_[static_cast<std::size_t>(pos2_)];
        box2_[static_cast<std::size_t>(pos2_)] = a1;
        if (++pos2_ >= box2Len_) pos2_ = 0;
        if (sum2_ < 1e-9) sum2_ = 0.0;
        grDb_ = sum2_ * invBox2_;
        const double g = grDb_ > 0.0 ? fastDbToGain(-grDb_) : 1.0;
        const double applied = 1.0 + mix * (g - 1.0);
        for (int c = 0; c < channels; ++c) {
            x[c] = audio_[static_cast<std::size_t>(c)].read(delay_) * applied;
        }
        return grDb_ * mix;
    }

private:
    double pushMax(double v) {
        const int cap = hold_ + 2;
        // pop smaller values from the back
        while (dqCount_ > 0) {
            int back = dqHead_ + dqCount_ - 1;
            if (back >= cap) back -= cap;
            if (dqValue_[static_cast<std::size_t>(back)] <= v) --dqCount_;
            else break;
        }
        int slot = dqHead_ + dqCount_;
        if (slot >= cap) slot -= cap;
        dqIndex_[static_cast<std::size_t>(slot)] = counter_;
        dqValue_[static_cast<std::size_t>(slot)] = v;
        ++dqCount_;
        while (dqIndex_[static_cast<std::size_t>(dqHead_)] <= counter_ - hold_) {
            if (++dqHead_ >= cap) dqHead_ = 0;
            --dqCount_;
        }
        ++counter_;
        if (counter_ > (1LL << 60)) { // practically never; keep indices bounded
            for (int i = 0; i < dqCount_; ++i) dqIndex_[static_cast<std::size_t>((dqHead_ + i) % cap)] -= (1LL << 59);
            counter_ -= (1LL << 59);
        }
        return dqValue_[static_cast<std::size_t>(dqHead_)];
    }

    int window_ = 72, hold_ = 73, box1Len_ = 36, box2Len_ = 37, delay_ = 77;
    std::array<DelayLine, 2> audio_{};
    TruePeakDetector8x tp_{};
    std::vector<long long> dqIndex_;
    std::vector<double> dqValue_;
    int dqHead_ = 0, dqCount_ = 0;
    long long counter_ = 0;
    std::vector<double> box1_, box2_;
    int pos1_ = 0, pos2_ = 0;
    double sum1_ = 0, sum2_ = 0, invBox1_ = 1.0, invBox2_ = 1.0;
    double rel_ = 0, relAvg_ = 0, grDb_ = 0;
    double relFast_ = 0, relSlow_ = 0, avg_ = 0;
};

} // namespace detail

// ------------------------------------------------------------------------------------------------

class MasterEngine {
public:
    using Meters = master::MasterMeters;
    static constexpr int kControlInterval = 16;

    MasterEngine() {
        for (int id = 0; id < master::kNumParams; ++id) {
            plain_[static_cast<std::size_t>(id)] = master::kParams[static_cast<std::size_t>(id)].defaultValue;
        }
    }

    // Not real-time safe (allocates). Call before processing and whenever sr/maxBlock change.
    void prepare(double sampleRate, int maxBlock) {
        sr_ = sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlock);
        limiter_.prepare(sr_);
        latency_ = detail::Oversampler4x::kLatency + limiter_.latency();
        for (auto& d : dry_) d.prepare(latency_ + 1);
        loudness_.prepare(sr_);
        glue_.prepare(sr_, {15.0, 120.0, 650.0, 12.0, 40.0, 90.0, 0.65});
        vocalComp_.prepare(sr_, {3.0, 60.0, 260.0, 5.0, 20.0, 60.0, 0.35});
        prepared_ = true;
        reset();
    }

    // Clears all state and snaps every smoother to the current parameter targets.
    void reset() {
        computeTargets();
        for (auto& f : eqFilters_) f.reset();
        hpf_.reset();
        for (auto& f : duckFilters_) f.reset();
        for (auto& f : scBands_) f.reset();
        duckEnv_.fill(0.0);
        deessHp_.reset(); deessLp_.reset(); deessBand_.reset();
        sibEnv_ = 0.0; deessGrDb_ = 0.0;
        punchSplit_.reset();
        for (auto& f : punchHarmHp_) f.reset();
        punchHarmLp_.reset();
        punchEnv_ = punchFast_ = punchSlow_ = 0.0;
        glue_.reset();
        vocalComp_.reset();
        widthSplit_.reset(); bassMonoSplit_.reset(); widthAllpass_.reset(); bassMonoAllpass_.reset();
        os_.reset();
        dcState_.fill(0.0);
        dcPrev_.fill(0.0);
        limiter_.reset();
        for (auto& d : dry_) d.reset();
        loudness_.reset();
        outTp_.reset();
        corrLR_ = corrLL_ = corrRR_ = 0.0;
        controlCounter_ = 0;
        snapSmoothers();
        updateCoefficients(true);
        analysisResetRuntime();
        meters_ = Meters{};
        meters_.latencySamples = latency_;
        publishAnalysisMeters();
    }

    int latencySamples() const { return latency_; }
    double sampleRate() const { return sr_; }

    // ---- parameters ------------------------------------------------------------------------
    void setParameter(int id, double normalized) { setParameterPlain(id, master::toPlain(id, normalized)); }
    void setParameterPlain(int id, double plain) {
        if (id < 0 || id >= master::kNumParams) return;
        const auto& info = master::kParams[static_cast<std::size_t>(id)];
        if (!std::isfinite(plain)) plain = info.defaultValue;
        plain = std::clamp(plain, info.minValue, info.maxValue);
        if (info.stepCount > 0) {
            const double step = (info.maxValue - info.minValue) / info.stepCount;
            plain = info.minValue + std::round((plain - info.minValue) / step) * step;
        }
        if (id == master::kAnalyze) {
            if (plain >= 0.5 && plain_[static_cast<std::size_t>(id)] < 0.5) requestAnalysis();
        }
        plain_[static_cast<std::size_t>(id)] = plain;
        targetsDirty_ = true;
    }
    double getParameterPlain(int id) const {
        return id >= 0 && id < master::kNumParams ? plain_[static_cast<std::size_t>(id)] : 0.0;
    }
    double getParameterNormalized(int id) const { return master::toNormalized(id, getParameterPlain(id)); }

    // ---- analysis / meters -----------------------------------------------------------------
    // Request the 8-second analysis (consumed at the start of the next process() call).
    void requestAnalysis() { analysisRequested_ = true; }
    void clearAnalysis() {
        autoEqTarget_.fill(0.0);
        autoRatio_ = 1.0;
        analysisStatus_ = master::kAnalysisReady;
        analysisProgress_ = 0.0;
        targetsDirty_ = true;
        publishAnalysisMeters();
    }
    // Restore auto-EQ gains (e.g. from saved state); values are clamped to [-1.5, 0] dB.
    void setAutoEq(const double* gainsDb, double compRatio = 1.0) {
        for (int b = 0; b < master::kBands; ++b) autoEqTarget_[static_cast<std::size_t>(b)] = std::clamp(gainsDb[b], -1.5, 0.0);
        autoRatio_ = std::clamp(compRatio, 1.0, 1.15);
        targetsDirty_ = true;
        publishAnalysisMeters();
    }
    void resetLoudness() { loudness_.resetIntegrated(); outTp_.resetHold(); }
    const Meters& meters() const { return meters_; }

    // Magnitude response (dB) of the static EQ section for the given plain parameter values and
    // auto-EQ gains — for the UI's EQ graph. Uses the same filter designs as the engine.
    static double eqResponseDb(const std::array<double, master::kNumParams>& plain, const double* autoEqDb,
                               double sampleRate, double freqHz) {
        std::array<double, 9> db{};
        double hpf = 0.0;
        computeEqTargets(plain, autoEqDb, db, hpf);
        double mag = 0.0;
        for (int i = 0; i < 8; ++i) mag += biquadMagnitudeDb(makeEqFilter(i, db[static_cast<std::size_t>(i)], sampleRate), freqHz, sampleRate);
        mag += biquadMagnitudeDb(Biquad::highPass(sampleRate, hpf, 0.7071), freqHz, sampleRate);
        return mag;
    }

    // ---- processing ------------------------------------------------------------------------
    // in/out may alias (in-place). Channels beyond 2 are cleared. sidechain: vocal stem (optional).
    template <typename T>
    void process(const T* const* in, T* const* out, int numChannels, int numSamples,
                 const T* const* sidechain = nullptr, int sidechainChannels = 0) {
        if (!prepared_ || numSamples <= 0 || numChannels <= 0 || !in || !out) return;
        ScopedFlushDenormals noDenormals;
        const int ch = std::min(numChannels, 2);
        const bool hasSidechain = sidechain != nullptr && sidechainChannels > 0;
        const int scCh = hasSidechain ? std::min(sidechainChannels, 2) : 0;
        if (targetsDirty_) computeTargets();
        if (analysisRequested_) { analysisRequested_ = false; analysisStart(hasSidechain, ch, scCh); }

        double inPk[2] = {0.0, 0.0}, outPk[2] = {0.0, 0.0};
        double glueGrMax = 0.0, limGrMax = 0.0, clipRatioMax = 1.0, deessMax = 0.0, duckMax = 0.0, outTpBlock = 0.0;

        for (int i = 0; i < numSamples; ++i) {
            if (controlCounter_ == 0) { updateCoefficients(false); flushFilters(); }
            if (++controlCounter_ >= kControlInterval) controlCounter_ = 0;

            double x[2] = {0.0, 0.0}, raw[2] = {0.0, 0.0};
            for (int c = 0; c < ch; ++c) {
                double v = static_cast<double>(in[c][i]);
                if (!std::isfinite(v)) v = 0.0;
                raw[c] = v;
                inPk[c] = std::max(inPk[c], std::abs(v));
            }
            double scSample = 0.0;
            if (hasSidechain) {
                for (int c = 0; c < scCh; ++c) {
                    const double v = static_cast<double>(sidechain[c][i]);
                    scSample += std::isfinite(v) ? v : 0.0;
                }
                scSample /= scCh;
            }
            if (analysisStatus_ == master::kAnalysisRunning) analysisCapture(raw, ch, hasSidechain, sidechain, scCh, i);

            // --- smoothers (per sample) ---
            const double gIn = sInGain_.next();
            const double gPreOs = sPreOsGain_.next();
            const double gPostOs = sPostOsGain_.next();
            const double duckDepth = sDuck_.next();
            const double deess = sDeess_.next();
            const double punch = sPunch_.next();
            const double glueAmt = sGlue_.next();
            const double vcAmt = sVocalComp_.next();
            const double width = sWidth_.next();
            const double bassMonoMix = sBassMonoMix_.next();
            const double sat = sSat_.next();
            const double drive = sDrive_.next();
            const double clip = sClip_.next();
            const double ceilingDb = sCeiling_.next();
            const double limMix = rLimiter_.next();
            const double safetyMix = rSafety_.next();
            const double bypassMix = rBypass_.next();

            {
                F2 v = F2::make(raw[0] * gIn, ch == 2 ? raw[1] * gIn : 0.0);
                v = hpf_.process(v);
                for (int f = 0; f < 7; ++f) if (eqActive_[static_cast<std::size_t>(f)]) v = eqFilters_[static_cast<std::size_t>(f)].process(v);
                x[0] = v.l();
                x[1] = ch == 2 ? v.r() : 0.0;
            }

            // --- vocal sidechain ducking (dynamic cuts at 900 Hz / 3.2 kHz) ---
            {
                double duckNow = 0.0;
                for (int b = 0; b < 2; ++b) {
                    const double level = std::abs(scBands_[static_cast<std::size_t>(b)].process(scSample));
                    double& env = duckEnv_[static_cast<std::size_t>(b)];
                    env = level + (env - level) * (level > env ? duckAttack_ : duckRelease_);
                    const double activity = std::clamp((env - 0.006) / (0.05 - 0.006), 0.0, 1.0);
                    const double cutDb = duckDepth * activity;
                    duckNow = std::max(duckNow, cutDb);
                    if (cutDb > 1e-6 || duckRunning_) {
                        const double k = 1.0 - detail::fastDbToGain(-cutDb);
                        const F2 band = duckFilters_[static_cast<std::size_t>(b)].process(F2::make(x[0], x[1]));
                        x[0] -= k * band.l();
                        x[1] -= k * band.r();
                    }
                }
                const bool running = duckNow > 1e-6;
                if (!running && duckRunning_) {
                    for (auto& f : duckFilters_) f.reset();
                }
                duckRunning_ = running;
                duckMax = std::max(duckMax, duckNow);
            }

            // --- de-esser (split-band dynamic, 5-10 kHz) ---
            if (deess > 0.0) {
                const F2 xs = F2::make(x[0], x[1]);
                const double sib = F2::absv(deessLp_.process(deessHp_.process(xs))).hmax();
                sibEnv_ = sib + (sibEnv_ - sib) * (sib > sibEnv_ ? deessAttack_ : deessRelease_);
                const double sibDb = detail::fastGainToDb(sibEnv_ + 1e-10);
                const double thr = -20.0 - 16.0 * deess;
                const double target = std::min(12.0 * deess, std::max(0.0, sibDb - thr) * (0.6 + 0.3 * deess));
                deessGrDb_ = target + (deessGrDb_ - target) * (target > deessGrDb_ ? deessGrAttack_ : deessGrRelease_);
                const double k = 1.0 - detail::fastDbToGain(-deessGrDb_);
                const F2 band = deessBand_.process(xs);
                x[0] -= k * band.l();
                x[1] -= k * band.r();
                deessMax = std::max(deessMax, deessGrDb_);
            } else if (deessGrDb_ != 0.0 || sibEnv_ != 0.0) {
                deessGrDb_ = 0.0; sibEnv_ = 0.0;
                deessHp_.reset(); deessLp_.reset(); deessBand_.reset();
            }

            // --- presence peak ---
            if (eqActive_[7]) {
                const F2 v = eqFilters_[7].process(F2::make(x[0], x[1]));
                x[0] = v.l();
                x[1] = ch == 2 ? v.r() : 0.0;
            }

            // --- 808-Punch (mid only: mono-safe) ---
            if (punch > 0.0) {
                const double mid = ch == 2 ? 0.5 * (x[0] + x[1]) : x[0];
                double lo, hi;
                punchSplit_.process(mid, lo, hi);
                const double alo = std::abs(lo);
                punchEnv_ = alo + (punchEnv_ - alo) * (alo > punchEnv_ ? punchEnvAttack_ : punchEnvRelease_);
                punchFast_ = alo + (punchFast_ - alo) * (alo > punchFast_ ? punchFastAttack_ : punchFastRelease_);
                punchSlow_ = alo + (punchSlow_ - alo) * (alo > punchSlow_ ? punchSlowAttack_ : punchSlowRelease_);
                const double u = std::clamp(lo / (punchEnv_ + 1e-6), -1.0, 1.0);
                const double t2 = 2.0 * u * u - 1.0;
                const double t3 = u * (4.0 * u * u - 3.0);
                double harm = punchEnv_ * (0.6 * t2 + 0.4 * t3);
                for (auto& f : punchHarmHp_) harm = f.process(harm);
                harm = punchHarmLp_.process(harm);
                const double transientDb = std::clamp(detail::fastGainToDb((punchFast_ + 1e-9) / (punchSlow_ + 1e-9)), 0.0, 12.0);
                const double boost = detail::fastDbToGain(std::min(4.5, transientDb * 0.75) * punch) - 1.0;
                const double add = punch * 1.1 * harm + boost * lo;
                for (int c = 0; c < ch; ++c) x[c] += add;
                punchActive_ = true;
            } else if (punchActive_) {
                punchActive_ = false;
                punchSplit_.reset();
                for (auto& f : punchHarmHp_) f.reset();
                punchHarmLp_.reset();
                punchEnv_ = punchFast_ = punchSlow_ = 0.0;
            }

            // --- compressors (glue: Beat/Master, vocal: Vocal) ---
            {
                double grNow = 0.0;
                if (glueAmt > 0.0 || glue_.gainReductionDb() > 0.0) {
                    const double ratio = (1.0 + 3.0 * glueAmt) * (1.0 + (autoRatioSmoothed_ - 1.0) * std::min(1.0, glueAmt * 4.0));
                    const double thr = -6.0 - 16.0 * glueAmt;
                    const double g = glue_.process(x, ch, thr, ratio, 8.0);
                    const double makeup = detail::fastDbToGain(glueMakeupDb(glueAmt));
                    for (int c = 0; c < ch; ++c) x[c] *= g * makeup;
                    grNow = std::max(grNow, glue_.gainReductionDb());
                }
                if (vcAmt > 0.0 || vocalComp_.gainReductionDb() > 0.0) {
                    const double ratio = 1.0 + 4.0 * vcAmt;
                    const double thr = -8.0 - 18.0 * vcAmt;
                    const double g = vocalComp_.process(x, ch, thr, ratio, 6.0);
                    const double makeup = detail::fastDbToGain(vocalMakeupDb(vcAmt));
                    for (int c = 0; c < ch; ++c) x[c] *= g * makeup;
                    grNow = std::max(grNow, vocalComp_.gainReductionDb());
                }
                glueGrMax = std::max(glueGrMax, grNow);
            }

            // --- width / bass-mono (M/S) ---
            if (ch == 2) {
                double mid = 0.5 * (x[0] + x[1]);
                double side = 0.5 * (x[0] - x[1]);
                // Width acts only above ~250 Hz: LR4 split of the side signal, matching allpass on
                // the mid so M and S stay phase-aligned (width 100 % = allpass on L/R, flat).
                double sLo, sHi;
                widthSplit_.process(side, sLo, sHi);
                side = sLo + width * sHi;
                mid = widthAllpass_.process(mid);
                if (bassMonoMix > 0.0) {
                    double bLo, bHi;
                    bassMonoSplit_.process(side, bLo, bHi);
                    const double midAp = bassMonoAllpass_.process(mid);
                    side += bassMonoMix * (bHi - side);
                    mid += bassMonoMix * (midAp - mid);
                    bassMonoActive_ = true;
                } else if (bassMonoActive_) {
                    bassMonoActive_ = false;
                    bassMonoSplit_.reset();
                    bassMonoAllpass_.reset();
                }
                x[0] = mid + side;
                x[1] = mid - side;
            }

            // --- oversampled nonlinear section ---
            {
                if (sat != nlSat_ || drive != nlDrive_ || ceilingDb != nlCeiling_ || clip != nlClip_) updateNonlinearConstants(sat, drive, ceilingDb, clip);
                const double satDrive = nlSatDrive_, satMix = nlSatMix_, satBias = nlSatBias_, satGain = nlSatGain_, satT0 = nlSatT0_;
                const double driveLin = nlDriveLin_, ceilingLin = nlCeilingLin_, clipThr = nlClipThr_, clipKnee = nlClipKnee_, clipMix = nlClipMix_;
                const double safetyThr = kSafetyThr;
                F2 up4[4];
                os_.up(F2::make(x[0] * gPreOs, ch == 2 ? x[1] * gPreOs : 0.0), up4);
                double up[4][2];
                for (int j = 0; j < 4; ++j) up4[j].store(up[j]);
                for (int j = 0; j < 4; ++j) {
                    for (int c = 0; c < ch; ++c) {
                        double& s = up[j][c];
                        if (satMix > 0.0) {
                            const double shaped = (detail::fastTanh(satDrive * s + satBias) - satT0) * satGain;
                            s += satMix * (shaped - s);
                        }
                        s *= driveLin;
                        if (clipMix > 0.0) {
                            const double cl = detail::softClip(s, clipThr, clipKnee);
                            const double y = s + clipMix * (cl - s);
                            if (y != s) clipRatioMax = std::max(clipRatioMax, std::abs(s) / std::max(1e-12, std::abs(y)));
                            s = y;
                        }
                        if (safetyMix > 0.0) s += safetyMix * (detail::softClip(s, safetyThr, 0.25) - s);
                    }
                }
                for (int j = 0; j < 4; ++j) up4[j] = F2::load(up[j]);
                const F2 down = os_.down(up4);
                for (int c = 0; c < ch; ++c) {
                    const double y = c == 0 ? down.l() : down.r();
                    // DC blocker (~5 Hz) for asymmetric saturation
                    const double dc = y - dcPrev_[static_cast<std::size_t>(c)] + dcCoef_ * dcState_[static_cast<std::size_t>(c)];
                    dcPrev_[static_cast<std::size_t>(c)] = y;
                    dcState_[static_cast<std::size_t>(c)] = dc;
                    x[c] = dc * gPostOs;
                }
                // --- limiter (always runs for constant latency; mix 0 outside Master mode) ---
                const double grDb = limiter_.process(x, ch, ceilingLin * limiterMargin_, limMix);
                limGrMax = std::max(limGrMax, grDb);
                if (limMix >= 1.0) {
                    for (int c = 0; c < ch; ++c) x[c] = std::clamp(x[c], -ceilingLin, ceilingLin);
                }
            }

            // --- bypass crossfade with the latency-compensated dry signal ---
            for (int c = 0; c < ch; ++c) {
                auto& d = dry_[static_cast<std::size_t>(c)];
                d.push(raw[c]);
                const double dry = d.read(latency_);
                double y;
                if (bypassMix >= 1.0) y = dry;
                else if (bypassMix <= 0.0) y = x[c];
                else y = x[c] + bypassMix * (dry - x[c]);
                x[c] = y;
                out[c][i] = static_cast<T>(y);
                outPk[c] = std::max(outPk[c], std::abs(y));
            }

            // --- meters ---
            loudness_.processFrame(x, ch);
            outTpBlock = std::max(outTpBlock, outTp_.processFrame(x, ch));
            {
                const double l = x[0], r = ch == 2 ? x[1] : x[0];
                corrLR_ = l * r + (corrLR_ - l * r) * corrCoef_;
                corrLL_ = l * l + (corrLL_ - l * l) * corrCoef_;
                corrRR_ = r * r + (corrRR_ - r * r) * corrCoef_;
            }
        }
        for (int c = ch; c < numChannels; ++c) {
            if (out[c]) for (int i = 0; i < numSamples; ++i) out[c][i] = T(0);
        }

        meters_.inputPeak[0] = static_cast<float>(inPk[0]);
        meters_.inputPeak[1] = static_cast<float>(ch == 2 ? inPk[1] : inPk[0]);
        meters_.outputPeak[0] = static_cast<float>(outPk[0]);
        meters_.outputPeak[1] = static_cast<float>(ch == 2 ? outPk[1] : outPk[0]);
        meters_.outputTruePeakDb = static_cast<float>(gainToDb(outTpBlock));
        meters_.truePeakMaxDb = static_cast<float>(outTp_.maxTruePeakDb());
        meters_.momentaryLufs = static_cast<float>(loudness_.momentaryLufs());
        meters_.shortTermLufs = static_cast<float>(loudness_.shortTermLufs());
        meters_.integratedLufs = static_cast<float>(loudness_.integratedLufs());
        const double clipGrMax = gainToDb(clipRatioMax);
        meters_.glueGrDb = static_cast<float>(glueGrMax);
        meters_.limiterGrDb = static_cast<float>(limGrMax);
        meters_.clipGrDb = static_cast<float>(clipGrMax);
        meters_.clipActivity = static_cast<float>(std::clamp(clipGrMax / 6.0, 0.0, 1.0));
        meters_.deEsserGrDb = static_cast<float>(deessMax);
        meters_.duckDb = static_cast<float>(duckMax);
        const double denom = std::sqrt(corrLL_ * corrRR_);
        meters_.correlation = static_cast<float>(denom > 1e-12 ? std::clamp(corrLR_ / denom, -1.0, 1.0) : 0.0);
        meters_.latencySamples = latency_;
        publishAnalysisMeters();
    }

private:
    // ---- target computation ------------------------------------------------------------------
    struct Targets {
        int mode = master::kModeMaster;
        double inGainDb = 0, preOsGainDb = 0, postOsGainDb = 0, hpfHz = 20;
        std::array<double, 9> eqDb{}; // LS60, P250, P1k, P4k, HS12k, nasal, resonance, presence
        double duckDb = 0, deess = 0, punch = 0, glue = 0, vocalComp = 0, width = 1;
        double bassMonoHz = 0, bassMonoOn = 0, sat = 0, driveDb = 0, clip = 0, ceilingDb = -1;
        double limiter = 1, safety = 0, bypass = 0;
    };

    static const master::Preset& presetFor(const std::array<double, master::kNumParams>& p) {
        const int idx = std::clamp(static_cast<int>(std::lround(p[master::kPreset])), 0, master::kNumPresets - 1);
        return master::kPresets[static_cast<std::size_t>(idx)];
    }

    static void computeEqTargets(const std::array<double, master::kNumParams>& p, const double* autoEq,
                                 std::array<double, 9>& db, double& hpfHz) {
        const int mode = std::clamp(static_cast<int>(std::lround(p[master::kMode])), 0, 2);
        const double a = p[master::kAmount] / 100.0;
        const auto& pr = presetFor(p);
        const std::array<int, 5> ids{{master::kEq60, master::kEq250, master::kEq1k, master::kEq4k, master::kEq12k}};
        for (int b = 0; b < 5; ++b) {
            db[static_cast<std::size_t>(b)] = p[static_cast<std::size_t>(ids[static_cast<std::size_t>(b)])]
                + pr.eqDb[static_cast<std::size_t>(b)] * a + (autoEq ? autoEq[b] : 0.0);
        }
        db[0] += p[master::kBass];
        db[4] += p[master::kBeatHighs];
        db[5] = -4.0 * clamp01(p[master::kNasal] / 100.0 + pr.nasal * a);
        db[6] = -4.5 * clamp01(p[master::kResonance] / 100.0 + pr.resonance * a);
        db[7] = p[master::kPresence] + pr.presenceDb * a;
        db[8] = 0.0;
        hpfHz = mode == master::kModeVocal ? 80.0 : 20.0;
    }

    void computeTargets() {
        targetsDirty_ = false;
        const auto& p = plain_;
        Targets t;
        t.mode = std::clamp(static_cast<int>(std::lround(p[master::kMode])), 0, 2);
        const bool vocal = t.mode == master::kModeVocal, beat = t.mode == master::kModeBeat, mst = t.mode == master::kModeMaster;
        const double a = p[master::kAmount] / 100.0;
        const auto& pr = presetFor(p);
        computeEqTargets(p, autoEqTarget_.data(), t.eqDb, t.hpfHz);
        t.inGainDb = p[master::kInput];
        t.preOsGainDb = vocal ? p[master::kOutput] : 0.0;
        t.postOsGainDb = vocal ? 0.0 : p[master::kOutput];
        t.duckDb = vocal ? 0.0 : 4.0 * p[master::kVocalFocus] / 100.0;
        t.deess = beat ? 0.0 : clamp01(p[master::kDeEsser] / 100.0 + pr.deEsser * a);
        t.punch = vocal ? 0.0 : clamp01(p[master::kPunch808] / 100.0 + pr.punch * a);
        const double glue = clamp01(p[master::kGlue] / 100.0 + pr.glue * a);
        t.glue = vocal ? 0.0 : glue;
        t.vocalComp = vocal ? glue : 0.0;
        t.width = vocal ? 1.0 : std::clamp(p[master::kWidth] / 100.0 + pr.widthDelta * a, 0.0, 2.0);
        const double bm = vocal ? 0.0 : std::max(p[master::kBassMono], pr.bassMonoHz * a);
        t.bassMonoOn = bm >= 1.0 ? 1.0 : 0.0;
        t.bassMonoHz = std::max(20.0, bm);
        t.sat = clamp01(p[master::kSaturation] / 100.0 + pr.saturation * a);
        t.driveDb = mst ? p[master::kLoudness] + pr.driveDb * a : 0.0;
        t.clip = mst ? clamp01(p[master::kClipper] / 100.0 + pr.clipper * a) : 0.0;
        t.ceilingDb = p[master::kCeiling];
        t.limiter = mst ? 1.0 : 0.0;
        t.safety = vocal ? 1.0 : 0.0;
        t.bypass = p[master::kBypass] >= 0.5 ? 1.0 : 0.0;
        t_ = t;

        sInGain_.target = dbToGain(t.inGainDb);
        sPreOsGain_.target = dbToGain(t.preOsGainDb);
        sPostOsGain_.target = dbToGain(t.postOsGainDb);
        sDuck_.target = t.duckDb;
        sDeess_.target = t.deess;
        sPunch_.target = t.punch;
        sGlue_.target = t.glue;
        sVocalComp_.target = t.vocalComp;
        sWidth_.target = t.width;
        sBassMonoMix_.target = t.bassMonoOn;
        sSat_.target = t.sat;
        sDrive_.target = t.driveDb;
        sClip_.target = t.clip;
        sCeiling_.target = t.ceilingDb;
        rLimiter_.target = t.limiter;
        rSafety_.target = t.safety;
        rBypass_.target = t.bypass;
    }

    void snapSmoothers() {
        const double rate = sr_;
        for (auto* s : {&sInGain_, &sPreOsGain_, &sPostOsGain_, &sDuck_, &sDeess_, &sPunch_, &sGlue_, &sVocalComp_,
                        &sWidth_, &sBassMonoMix_, &sSat_, &sDrive_, &sClip_, &sCeiling_}) {
            s->setTime(0.02, rate);
            s->snap(s->target);
        }
        sBassMonoMix_.setTime(0.03, rate);
        for (auto* r : {&rLimiter_, &rSafety_, &rBypass_}) {
            r->setTime(0.02, rate);
            r->snap(r->target);
        }
        rBypass_.setTime(0.01, rate);
        eqDbCur_ = t_.eqDb;
        hpfCur_ = t_.hpfHz;
        bassMonoHzCur_ = t_.bassMonoHz;
        autoRatioSmoothed_ = autoRatio_;
        eqDbApplied_.fill(1e9);
        hpfApplied_ = bassMonoApplied_ = -1.0;

        // fixed coefficients
        duckAttack_ = timeCoefficient(0.008, rate);
        duckRelease_ = timeCoefficient(0.14, rate);
        const std::array<double, 2> duckFreq{{900.0, 3200.0}};
        for (int b = 0; b < 2; ++b) {
            const auto bp = Biquad::bandPass(rate, duckFreq[static_cast<std::size_t>(b)], 0.8);
            scBands_[static_cast<std::size_t>(b)].setCoefficients(bp);
            duckFilters_[static_cast<std::size_t>(b)].setCoefficients(bp);
        }
        deessHp_.setCoefficients(Biquad::highPass(rate, 4000.0, 0.7071));
        deessLp_.setCoefficients(Biquad::lowPass(rate, 10000.0, 0.7071));
        deessBand_.setCoefficients(Biquad::bandPass(rate, 7000.0, 0.75));
        deessAttack_ = timeCoefficient(0.0005, rate);
        deessRelease_ = timeCoefficient(0.03, rate);
        deessGrAttack_ = timeCoefficient(0.001, rate);
        deessGrRelease_ = timeCoefficient(0.06, rate);
        punchSplit_.set(rate, 110.0);
        for (auto& f : punchHarmHp_) f.setCoefficients(Biquad::highPass(rate, 75.0, 0.7071));
        punchHarmLp_.setCoefficients(Biquad::lowPass(rate, 320.0, 0.7071));
        punchEnvAttack_ = timeCoefficient(0.002, rate);
        punchEnvRelease_ = timeCoefficient(0.060, rate);
        punchFastAttack_ = timeCoefficient(0.001, rate);
        punchFastRelease_ = timeCoefficient(0.030, rate);
        punchSlowAttack_ = timeCoefficient(0.030, rate);
        punchSlowRelease_ = timeCoefficient(0.150, rate);
        widthSplit_.set(rate, 250.0);
        widthAllpass_.setCoefficients(allPassForCrossover(rate, 250.0));
        dcCoef_ = std::exp(-2.0 * kPi * 5.0 / rate);
        corrCoef_ = timeCoefficient(0.3, rate);
        controlCoef_ = timeCoefficient(0.03, rate / kControlInterval);
        limiterMargin_ = dbToGain(-0.12);
    }

    static Biquad makeEqFilter(int index, double db, double sr) {
        switch (index) {
            case 0: return Biquad::lowShelf(sr, 60.0, db);
            case 1: return Biquad::peak(sr, 250.0, 0.65, db);
            case 2: return Biquad::peak(sr, 1000.0, 0.65, db);
            case 3: return Biquad::peak(sr, 4000.0, 0.65, db);
            case 4: return Biquad::highShelf(sr, 12000.0, db);
            case 5: return Biquad::peak(sr, 900.0, 1.0, db);
            case 6: return Biquad::peak(sr, 3400.0, 3.2, db);
            case 7: return Biquad::peak(sr, 3500.0, 0.9, db);
            default: return Biquad::identity();
        }
    }

    static double biquadMagnitudeDb(const Biquad& q, double f, double sr) {
        const double w = 2.0 * kPi * f / sr;
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
        const double dr = 1.0 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
        return 10.0 * std::log10((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
    }

    // Control-rate update (every kControlInterval samples, counter persists across blocks).
    void updateCoefficients(bool force) {
        const double k = force ? 0.0 : controlCoef_;
        auto step = [k](double cur, double target) {
            const double v = target + (cur - target) * k;
            return std::abs(v - target) < 1e-6 ? target : v;
        };
        for (int f = 0; f < 8; ++f) {
            auto& cur = eqDbCur_[static_cast<std::size_t>(f)];
            cur = step(cur, t_.eqDb[static_cast<std::size_t>(f)]);
            if (cur != eqDbApplied_[static_cast<std::size_t>(f)]) {
                eqDbApplied_[static_cast<std::size_t>(f)] = cur;
                const auto q = makeEqFilter(f, cur, sr_);
                eqFilters_[static_cast<std::size_t>(f)].setCoefficients(q);
                const bool active = std::abs(cur) >= 1e-4;
                if (!active && eqActive_[static_cast<std::size_t>(f)]) {
                    eqFilters_[static_cast<std::size_t>(f)].reset();
                }
                eqActive_[static_cast<std::size_t>(f)] = active;
            }
        }
        hpfCur_ = std::exp(step(std::log(hpfCur_), std::log(t_.hpfHz)));
        if (std::abs(hpfCur_ - t_.hpfHz) < 1e-6) hpfCur_ = t_.hpfHz;
        if (hpfCur_ != hpfApplied_) {
            hpfApplied_ = hpfCur_;
            const auto q = Biquad::highPass(sr_, hpfCur_, 0.7071);
            hpf_.setCoefficients(q);
        }
        bassMonoHzCur_ = std::exp(step(std::log(bassMonoHzCur_), std::log(t_.bassMonoHz)));
        if (std::abs(bassMonoHzCur_ - t_.bassMonoHz) < 1e-6) bassMonoHzCur_ = t_.bassMonoHz;
        if (bassMonoHzCur_ != bassMonoApplied_) {
            bassMonoApplied_ = bassMonoHzCur_;
            bassMonoSplit_.set(sr_, bassMonoHzCur_);
            bassMonoAllpass_.setCoefficients(allPassForCrossover(sr_, bassMonoHzCur_));
        }
        autoRatioSmoothed_ = step(autoRatioSmoothed_, autoRatio_);
    }

    // Clears denormal/non-finite filter state (the per-sample filters skip sanitize()).
    void flushFilters() {
        hpf_.flush();
        for (auto& f : eqFilters_) f.flush();
        for (auto& f : duckFilters_) f.flush();
        for (auto& f : scBands_) f.flush();
        deessHp_.flush(); deessLp_.flush(); deessBand_.flush();
        punchSplit_.flush();
        for (auto& f : punchHarmHp_) f.flush();
        punchHarmLp_.flush();
        widthSplit_.flush(); bassMonoSplit_.flush(); widthAllpass_.flush(); bassMonoAllpass_.flush();
        glue_.flush();
        vocalComp_.flush();
        for (auto& src : anFilters_) for (auto& chf : src) for (auto& f : chf) f.flush();
        for (auto& v : duckEnv_) v = sanitize(v);
        for (auto& v : dcState_) v = sanitize(v);
        for (auto& v : dcPrev_) v = sanitize(v);
        sibEnv_ = sanitize(sibEnv_);
        punchEnv_ = sanitize(punchEnv_); punchFast_ = sanitize(punchFast_); punchSlow_ = sanitize(punchSlow_);
        corrLR_ = sanitize(corrLR_); corrLL_ = sanitize(corrLL_); corrRR_ = sanitize(corrRR_);
    }

    static double fastTanhSq(double x) { const double t = detail::fastTanh(x); return t * t; }
    void updateNonlinearConstants(double sat, double drive, double ceilingDb, double clip) {
        nlSat_ = sat; nlDrive_ = drive; nlCeiling_ = ceilingDb; nlClip_ = clip;
        nlSatDrive_ = dbToGain(sat * 12.0); // 0..+12 dB into the tanh
        nlSatMix_ = std::min(1.0, sat * 5.0);
        nlSatBias_ = 0.12 * sat;
        nlSatT0_ = detail::fastTanh(nlSatBias_);
        nlSatGain_ = dbToGain(satMakeupDb(sat)) / (nlSatDrive_ * (1.0 - fastTanhSq(nlSatBias_)));
        nlDriveLin_ = dbToGain(drive);
        nlCeilingLin_ = dbToGain(ceilingDb);
        nlClipThr_ = nlCeilingLin_ * dbToGain(2.5 * (1.0 - clip) - 0.6 * clip);
        nlClipKnee_ = 0.12 + 0.28 * clip;
        nlClipMix_ = std::min(1.0, clip * 4.0);
    }
    static constexpr double kSafetyThr = 0.96605087898981337; // -0.3 dBFS
    double nlSat_ = -1.0, nlDrive_ = -1.0, nlCeiling_ = 1.0, nlClip_ = -1.0;
    double nlSatDrive_ = 1.0, nlSatMix_ = 0.0, nlSatBias_ = 0.0, nlSatT0_ = 0.0, nlSatGain_ = 1.0;
    double nlDriveLin_ = 1.0, nlCeilingLin_ = 1.0, nlClipThr_ = 1.0, nlClipKnee_ = 0.12, nlClipMix_ = 0.0;

    // Static make-up gains (dB) tuned on the -14 LUFS rap test mix so the knobs are roughly
    // loudness-neutral at moderate settings.
    static double glueMakeupDb(double a) { return a * (2.2 + 1.6 * a); }
    static double vocalMakeupDb(double a) { return a * (3.0 + 2.5 * a); }
    static double satMakeupDb(double s) { return s * (0.6 + 0.8 * s); }

    // ---- analysis (8 s, port of v0.1, now in Beat and Master modes) -------------------------
    void analysisResetRuntime() {
        if (analysisStatus_ == master::kAnalysisRunning) {
            analysisStatus_ = master::kAnalysisReady;
            analysisProgress_ = 0.0;
        }
    }

    void analysisStart(bool hasSidechain, int ch, int scCh) {
        analysisProgress_ = 0.0;
        if (t_.mode == master::kModeVocal) { analysisStatus_ = master::kAnalysisWrongMode; return; }
        if (t_.mode == master::kModeBeat && !hasSidechain) { analysisStatus_ = master::kAnalysisMissingSidechain; return; }
        analysisWithVocal_ = hasSidechain;
        analysisTarget_ = static_cast<std::uint64_t>(std::max(1.0, sr_ * 8.0));
        analysisCaptured_ = 0;
        analysisCh_ = ch;
        analysisScCh_ = std::max(1, scCh);
        anPower_.fill(0.0);
        anPeak_.fill(0.0);
        for (auto& s : anBandPower_) s.fill(0.0);
        for (int b = 0; b < master::kBands; ++b) {
            const auto bp = Biquad::bandPass(sr_, master::kBandFrequencies[static_cast<std::size_t>(b)], 0.8);
            for (auto& src : anFilters_) for (auto& chf : src) {
                chf[static_cast<std::size_t>(b)].setCoefficients(bp);
                chf[static_cast<std::size_t>(b)].reset();
            }
        }
        analysisStatus_ = master::kAnalysisRunning;
    }

    template <typename T>
    void analysisCapture(const double* raw, int ch, bool hasSidechain, const T* const* sc, int scCh, int i) {
        if (t_.mode == master::kModeVocal) { analysisStatus_ = master::kAnalysisWrongMode; return; }
        if (analysisWithVocal_ && !hasSidechain) { analysisStatus_ = master::kAnalysisMissingSidechain; return; }
        for (int c = 0; c < std::min(ch, analysisCh_); ++c) analysisAccumulate(0, c, raw[c]);
        if (analysisWithVocal_) {
            for (int c = 0; c < std::min(scCh, analysisScCh_); ++c) {
                const double v = static_cast<double>(sc[c][i]);
                analysisAccumulate(1, c, std::isfinite(v) ? v : 0.0);
            }
        }
        ++analysisCaptured_;
        analysisProgress_ = static_cast<double>(analysisCaptured_) / static_cast<double>(analysisTarget_);
        if (analysisCaptured_ >= analysisTarget_) analysisFinish();
    }

    void analysisAccumulate(int source, int c, double v) {
        const auto s = static_cast<std::size_t>(source);
        anPower_[s] += v * v;
        anPeak_[s] = std::max(anPeak_[s], std::abs(v));
        for (int b = 0; b < master::kBands; ++b) {
            const double f = anFilters_[s][static_cast<std::size_t>(c)][static_cast<std::size_t>(b)].process(v);
            anBandPower_[s][static_cast<std::size_t>(b)] += f * f;
        }
    }

    void analysisFinish() {
        const std::array<int, 2> counts{{analysisCh_, analysisScCh_}};
        std::array<double, 2> rmsDb{{-120.0, -120.0}};
        for (int s = 0; s < (analysisWithVocal_ ? 2 : 1); ++s) {
            const double n = static_cast<double>(analysisCaptured_) * counts[static_cast<std::size_t>(s)];
            const double mean = n > 0 ? anPower_[static_cast<std::size_t>(s)] / n : 0.0;
            rmsDb[static_cast<std::size_t>(s)] = mean > 1e-12 ? 10.0 * std::log10(mean) : -120.0;
            for (int b = 0; b < master::kBands; ++b) {
                const double bp = n > 0 ? anBandPower_[static_cast<std::size_t>(s)][static_cast<std::size_t>(b)] / n : 0.0;
                (s == 0 ? beatBandsDb_ : vocalBandsDb_)[static_cast<std::size_t>(b)] = bp > 1e-12 ? 10.0 * std::log10(bp) : -120.0;
            }
        }
        if (!analysisWithVocal_) vocalBandsDb_.fill(-120.0);
        beatRmsDb_ = rmsDb[0];
        vocalRmsDb_ = rmsDb[1];
        const double beatRms = std::pow(10.0, rmsDb[0] / 20.0);
        beatCrestDb_ = beatRms > 1e-9 && anPeak_[0] > 0.0 ? 20.0 * std::log10(anPeak_[0] / beatRms) : 0.0;
        analysisProgress_ = 1.0;
        if (rmsDb[0] < -65.0) { analysisStatus_ = master::kAnalysisTooQuiet; return; }

        // Rap reference tilt: expected band level relative to the band mean of a balanced rap mix
        // (strong lows are intended and must not be "corrected").
        static constexpr std::array<double, master::kBands> kTilt{{6.0, 1.5, -1.0, -2.5, -4.0}};
        double mean = 0.0;
        for (int b = 0; b < master::kBands; ++b) mean += beatBandsDb_[static_cast<std::size_t>(b)] - kTilt[static_cast<std::size_t>(b)];
        mean /= master::kBands;
        const bool vocalPresent = analysisWithVocal_ && rmsDb[1] > -60.0;
        for (int b = 0; b < master::kBands; ++b) {
            const auto bi = static_cast<std::size_t>(b);
            const double spectralCut = std::clamp((beatBandsDb_[bi] - kTilt[bi] - mean - 3.0) * 0.12, 0.0, 0.8);
            double overlapCut = 0.0;
            if (vocalPresent && (b == 2 || b == 3)) {
                overlapCut = std::clamp((beatBandsDb_[bi] - vocalBandsDb_[bi] - 1.0) * 0.12, 0.0, 1.2);
            }
            autoEqTarget_[bi] = -std::max(spectralCut, overlapCut);
        }
        autoRatio_ = std::clamp(1.0 + std::max(0.0, beatCrestDb_ - 10.0) * 0.015, 1.0, 1.12);
        targetsDirty_ = true;
        if (!analysisWithVocal_) analysisStatus_ = master::kAnalysisCompleteMixOnly;
        else analysisStatus_ = vocalPresent ? master::kAnalysisComplete : master::kAnalysisNoVocal;
    }

    void publishAnalysisMeters() {
        for (int b = 0; b < master::kBands; ++b) {
            const auto bi = static_cast<std::size_t>(b);
            meters_.autoEqDb[b] = autoEqTarget_[bi];
            meters_.beatBandsDb[b] = beatBandsDb_[bi];
            meters_.vocalBandsDb[b] = vocalBandsDb_[bi];
        }
        meters_.analysisStatus = analysisStatus_;
        meters_.analysisProgress = std::clamp(analysisProgress_, 0.0, 1.0);
        meters_.beatRmsDb = beatRmsDb_;
        meters_.vocalRmsDb = vocalRmsDb_;
        meters_.beatCrestDb = beatCrestDb_;
        meters_.autoCompRatio = autoRatio_;
    }

    // ---- state ---------------------------------------------------------------------------------
    double sr_ = 48000.0;
    int maxBlock_ = 512;
    bool prepared_ = false;
    int latency_ = 0;
    std::array<double, master::kNumParams> plain_{};
    bool targetsDirty_ = true;
    Targets t_{};
    int controlCounter_ = 0;
    double controlCoef_ = 0.0;

    detail::OnePole sInGain_, sPreOsGain_, sPostOsGain_, sDuck_, sDeess_, sPunch_, sGlue_, sVocalComp_, sWidth_,
        sBassMonoMix_, sSat_, sDrive_, sClip_, sCeiling_;
    detail::LinearRamp rLimiter_, rSafety_, rBypass_;

    FastBiquad2 hpf_{};
    double hpfCur_ = 20.0, hpfApplied_ = -1.0;
    std::array<FastBiquad2, 8> eqFilters_{};
    std::array<bool, 8> eqActive_{};
    std::array<double, 9> eqDbCur_{}, eqDbApplied_{};

    std::array<FastBiquad2, 2> duckFilters_{};
    std::array<FastBiquad, 2> scBands_{};
    std::array<double, 2> duckEnv_{};
    double duckAttack_ = 0, duckRelease_ = 0;
    bool duckRunning_ = false;

    FastBiquad2 deessHp_{}, deessLp_{}, deessBand_{};
    double sibEnv_ = 0, deessGrDb_ = 0, deessAttack_ = 0, deessRelease_ = 0, deessGrAttack_ = 0, deessGrRelease_ = 0;

    FastLinkwitzRiley punchSplit_{};
    std::array<FastBiquad, 2> punchHarmHp_{};
    FastBiquad punchHarmLp_{};
    double punchEnv_ = 0, punchFast_ = 0, punchSlow_ = 0;
    double punchEnvAttack_ = 0, punchEnvRelease_ = 0, punchFastAttack_ = 0, punchFastRelease_ = 0,
           punchSlowAttack_ = 0, punchSlowRelease_ = 0;
    bool punchActive_ = false;

    detail::Compressor glue_, vocalComp_;
    double autoRatioSmoothed_ = 1.0;

    FastLinkwitzRiley widthSplit_{}, bassMonoSplit_{};
    FastBiquad widthAllpass_{}, bassMonoAllpass_{};
    double bassMonoHzCur_ = 20.0, bassMonoApplied_ = -1.0;
    bool bassMonoActive_ = false;

    detail::Oversampler4x os_{};
    std::array<double, 2> dcState_{}, dcPrev_{};
    double dcCoef_ = 0.9995;

    detail::TruePeakLimiter limiter_;
    double limiterMargin_ = 1.0;
    std::array<DelayLine, 2> dry_{};

    LoudnessMeter loudness_;
    TruePeakMeter outTp_;
    double corrLR_ = 0, corrLL_ = 0, corrRR_ = 0, corrCoef_ = 0;

    // analysis
    bool analysisRequested_ = false;
    int analysisStatus_ = master::kAnalysisReady;
    double analysisProgress_ = 0.0;
    bool analysisWithVocal_ = false;
    std::uint64_t analysisTarget_ = 0, analysisCaptured_ = 0;
    int analysisCh_ = 2, analysisScCh_ = 1;
    std::array<double, 2> anPower_{}, anPeak_{};
    std::array<std::array<double, master::kBands>, 2> anBandPower_{};
    std::array<std::array<std::array<FastBiquad, master::kBands>, 2>, 2> anFilters_{};
    std::array<double, master::kBands> autoEqTarget_{}, beatBandsDb_{}, vocalBandsDb_{};
    double beatRmsDb_ = -120.0, vocalRmsDb_ = -120.0, beatCrestDb_ = 0.0, autoRatio_ = 1.0;

    Meters meters_{};
};

} // namespace sonora::dsp
