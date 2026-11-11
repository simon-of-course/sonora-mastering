#pragma once

// SONORA Drowner engine (v0.2) — SDK-independent, header-only, real-time safe.
//
// "Underwater" effect for rap vocals, driven by one depth control (Unterwasser-Anteil).
// Signal flow per channel (all modulation per sample, so output is block-size invariant):
//   x -> wobble chorus (modulated 7 ms delay, L/R detuned LFOs)
//     -> 24 dB/oct low-pass (2 cascaded TPT SVFs, Q 0.54 + resonant 1.31…4.5)
//        cutoff = exponential map of smoothed depth: min(0.48·fs, 21 kHz) -> 350 Hz, ± wobble LFO
//     -> Körper (1st-order low shelf ~180 Hz, gain = Körper dB · depth)
//     -> auto make-up (+6 dB · depth) and resonance level compensation
//   Wasserhall: dark 8-line FDN (Hadamard, damped loop, RT60 ≈ 1.2 s) fed with the filtered
//   chain scaled by Wasserhall · depth; its return is not gated by depth, so the tail decays
//   naturally when the depth returns to 0. Once the tail is below -140 dBFS the reverb idles.
//   The wet chain is crossfaded in by engage(depth) (0 at depth 0, 1 above depth 0.03), Ausgang
//   applies to the wet sound only (scaled by engage). With smoothed depth == 0 and an idle
//   reverb the output is a bit-exact copy of the input.
// Depth smoothing is a one-pole whose time constant is chosen so that 90 % of a depth change
// is reached after Abtauchzeit (rising) or Auftauchzeit (falling).
// Parameter changes are applied sample-accurately via the optional ParamEvent list passed to
// process(); setParameter() between blocks equals an event at offset 0.

#include "dsp_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace sonora::dsp::drowner {

enum ParamId : int {
    kDepth = 0,       // Unterwasser-Anteil 0–100 %
    kReturnTime = 1,  // Auftauchzeit 0.2–6 s (linear, v1 compatible)
    kDiveTime = 2,    // Abtauchzeit 0.01–2 s (logarithmic)
    kResonance = 3,   // Resonanz 0–100 %
    kWobble = 4,      // Wobble 0–100 %
    kReverb = 5,      // Wasserhall 0–100 %
    kBody = 6,        // Körper 0–6 dB
    kMix = 7,         // Mix 0–100 %
    kOutput = 8,      // Ausgang -12…+6 dB
    kNumParams = 9
};

struct ParamInfo {
    const char* name;  // UTF-8, German
    const char* unit;
    double minPlain;
    double maxPlain;
    double defaultPlain;
    bool logarithmic;  // plain = min · (max/min)^normalized
};

constexpr std::array<ParamInfo, kNumParams> kParamInfo{{
    {"Unterwasser-Anteil", "%", 0.0, 100.0, 0.0, false},
    {"Auftauchzeit", "s", 0.2, 6.0, 2.0, false},
    {"Abtauchzeit", "s", 0.01, 2.0, 0.05, true},
    {"Resonanz", "%", 0.0, 100.0, 30.0, false},
    {"Wobble", "%", 0.0, 100.0, 35.0, false},
    {"Wasserhall", "%", 0.0, 100.0, 30.0, false},
    {"Körper", "dB", 0.0, 6.0, 2.5, false},
    {"Mix", "%", 0.0, 100.0, 100.0, false},
    {"Ausgang", "dB", -12.0, 6.0, 0.0, false},
}};

constexpr double kMinCutoffHz = 350.0;
constexpr double kMaxCutoffHz = 21000.0;   // also limited to 0.48 · fs
constexpr double kMaxMakeupDb = 6.0;
constexpr double kEngageDepth = 0.03;

constexpr double clampNormalized(double n) {
    return n != n ? 0.0 : (n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n));
}
constexpr bool isValidParam(int id) { return id >= 0 && id < kNumParams; }

inline double toPlain(int id, double normalized) {
    if (!isValidParam(id)) return 0.0;
    const ParamInfo& p = kParamInfo[static_cast<std::size_t>(id)];
    const double n = clampNormalized(normalized);
    if (p.logarithmic) return p.minPlain * std::pow(p.maxPlain / p.minPlain, n);
    return p.minPlain + n * (p.maxPlain - p.minPlain);
}
inline double toNormalized(int id, double plain) {
    if (!isValidParam(id) || !std::isfinite(plain)) return 0.0;
    const ParamInfo& p = kParamInfo[static_cast<std::size_t>(id)];
    if (p.logarithmic) {
        const double v = std::clamp(plain, p.minPlain, p.maxPlain);
        return clampNormalized(std::log(v / p.minPlain) / std::log(p.maxPlain / p.minPlain));
    }
    return clampNormalized((plain - p.minPlain) / (p.maxPlain - p.minPlain));
}
inline double defaultNormalized(int id) {
    return isValidParam(id) ? toNormalized(id, kParamInfo[static_cast<std::size_t>(id)].defaultPlain) : 0.0;
}
// State v1 stored IDs 0 (depth) and 1 (return time) with the same normalization as v2.
constexpr double migrateParamV1ToV2(int /*id*/, double normalizedV1) { return clampNormalized(normalizedV1); }

// Low-pass cutoff (without wobble) for a smoothed depth 0…1.
inline double cutoffForDepth(double depth, double sampleRate) {
    const double top = std::max(kMinCutoffHz, std::min(kMaxCutoffHz, 0.48 * sampleRate));
    return top * std::pow(kMinCutoffHz / top, std::clamp(depth, 0.0, 1.0));
}

struct ParamEvent {
    int sampleOffset;   // 0 … numSamples-1, sorted ascending
    int id;
    double normalized;
};

// Snapshot written at the end of every process() call (copy into the UI meter bridge).
struct DrownerMeters {
    float depth = 0.0f;        // smoothed depth 0 … 1
    float cutoffHz = 0.0f;     // current low-pass cutoff incl. wobble (left channel)
    float engage = 0.0f;       // wet crossfade 0 … 1
    float lfo = 0.0f;          // wobble LFO -1 … 1 (for the water animation)
    float reverbLevel = 0.0f;  // |reverb return| peak of the last block
};

class DrownerEngine {
public:
    static constexpr int kMaxChannels = 2;

    DrownerEngine() {
        for (int id = 0; id < kNumParams; ++id) normalized_[static_cast<std::size_t>(id)] = defaultNormalized(id);
        updateTargets();
        prepare(44100.0, 512);
    }

    void prepare(double sampleRate, int maxBlock) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock_ = std::max(1, maxBlock);
        topCutoff_ = std::max(kMinCutoffHz, std::min(kMaxCutoffHz, 0.48 * sampleRate_));
        logCutoffRatio_ = std::log(kMinCutoffHz / topCutoff_);
        bodyG_ = std::tan(kPi * 180.0 / sampleRate_);
        bodyG_ = bodyG_ / (1.0 + bodyG_);
        reverbHpG_ = std::tan(kPi * 150.0 / sampleRate_);
        reverbHpG_ = reverbHpG_ / (1.0 + reverbHpG_);
        dampG_ = std::tan(kPi * 2400.0 / sampleRate_);
        dampG_ = dampG_ / (1.0 + dampG_);
        for (auto* s : {&resSmooth_, &wobbleSmooth_, &reverbSmooth_, &bodySmooth_, &mixSmooth_, &outSmooth_})
            s->setTime(0.02, sampleRate_);

        chorusSize_ = static_cast<int>(std::ceil(0.012 * sampleRate_)) + 4;
        for (auto& ch : channels_) ch.chorus.assign(static_cast<std::size_t>(chorusSize_), 0.0);

        predelay_.init(static_cast<int>(std::lround(0.010 * sampleRate_)));
        constexpr std::array<double, 3> diffMs{{4.7, 7.3, 11.1}};
        for (std::size_t i = 0; i < diffusers_.size(); ++i)
            diffusers_[i].init(static_cast<int>(std::lround(diffMs[i] * 0.001 * sampleRate_)));
        constexpr std::array<double, kLines> lineMs{{23.3, 28.9, 33.7, 37.1, 41.9, 45.7, 52.3, 57.1}};
        for (std::size_t i = 0; i < kLines; ++i) {
            const int len = static_cast<int>(std::lround(lineMs[i] * 0.001 * sampleRate_));
            lines_[i].init(len);
            lineGain_[i] = std::pow(10.0, -3.0 * len / (kReverbRt60 * sampleRate_));
        }
        idleLimit_ = static_cast<int>(0.05 * sampleRate_);
        updateTimeCoefficients();
        reset();
    }

    void reset() {
        updateTargets();
        depth_ = 0.0;
        resSmooth_.reset(resonance_);
        wobbleSmooth_.reset(wobble_);
        reverbSmooth_.reset(reverb_);
        bodySmooth_.reset(bodyDb_);
        mixSmooth_.reset(mix_);
        outSmooth_.reset(outputDb_);
        for (auto& ch : channels_) {
            std::fill(ch.chorus.begin(), ch.chorus.end(), 0.0);
            ch.svf1.reset();
            ch.svf2.reset();
            ch.bodyZ = 0.0;
        }
        chorusWrite_ = 0;
        lfoPhase_ = {0.0, 0.31};
        predelay_.clear();
        for (auto& d : diffusers_) d.clear();
        for (auto& l : lines_) l.clear();
        damp_.fill(0.0);
        reverbHpZ_ = 0.0;
        reverbIdle_ = true;
        idleCount_ = 0;
        meters_ = {};
        meters_.cutoffHz = static_cast<float>(topCutoff_);
    }

    int latencySamples() const { return 0; }
    double sampleRate() const { return sampleRate_; }

    void setParameter(int id, double normalized) {
        if (!isValidParam(id)) return;
        normalized_[static_cast<std::size_t>(id)] = clampNormalized(normalized);
        updateTargets();
        if (id == kDiveTime || id == kReturnTime) updateTimeCoefficients();
    }
    double getParameter(int id) const {
        return isValidParam(id) ? normalized_[static_cast<std::size_t>(id)] : 0.0;
    }

    const DrownerMeters& meters() const { return meters_; }
    double smoothedDepth() const { return depth_; }

    // in/out may alias (in-place). Channels beyond 2 are passed through unchanged.
    template <typename T>
    void process(const T* const* in, T* const* out, int numChannels, int numSamples,
                 const ParamEvent* events = nullptr, int numEvents = 0) {
        if (numChannels <= 0 || numSamples <= 0 || !in || !out) return;
        const int channels = std::min(numChannels, kMaxChannels);
        for (int ch = channels; ch < numChannels; ++ch)
            if (in[ch] != out[ch]) std::copy(in[ch], in[ch] + numSamples, out[ch]);

        constexpr double kLn10Over20 = 2.302585092994046 / 20.0;
        constexpr double kLn2 = 0.6931471805599453;
        double reverbPeak = 0.0;
        double cutoffL = meters_.cutoffHz;
        int nextEvent = 0;
        for (int n = 0; n < numSamples; ++n) {
            while (events && nextEvent < numEvents && events[nextEvent].sampleOffset <= n) {
                setParameter(events[nextEvent].id, events[nextEvent].normalized);
                ++nextEvent;
            }

            // Depth envelope: dive (rising) / return (falling), 90 % reached after the set time.
            const double coef = depthTarget_ > depth_ ? diveCoef_ : returnCoef_;
            depth_ = depthTarget_ + (depth_ - depthTarget_) * coef;
            if (depthTarget_ <= 0.0 && depth_ < 1e-5) depth_ = 0.0;
            if (std::abs(depth_ - depthTarget_) < 1e-12) depth_ = depthTarget_;
            const double s = depth_;
            const double e0 = std::min(1.0, s / kEngageDepth);
            const double engage = e0 * e0 * (3.0 - 2.0 * e0);

            const double res = resSmooth_.next(resonance_);
            const double wobble = wobbleSmooth_.next(wobble_);
            const double reverbAmount = reverbSmooth_.next(reverb_);
            const double bodyDb = bodySmooth_.next(bodyDb_);
            const double mix = mixSmooth_.next(mix_);
            const double outDb = outSmooth_.next(outputDb_);

            // Wobble LFOs (L/R slightly detuned).
            const double lfoRate = 0.4 + 0.8 * wobble;
            std::array<double, 2> lfo{};
            for (int ch = 0; ch < 2; ++ch) {
                auto& ph = lfoPhase_[static_cast<std::size_t>(ch)];
                lfo[static_cast<std::size_t>(ch)] = std::sin(2.0 * kPi * ph);
                ph += lfoRate * (ch == 0 ? 1.0 : 1.13) / sampleRate_;
                if (ph >= 1.0) ph -= 1.0;
            }

            const double chorusMix = 0.5 * wobble * s;
            const double q2 = 1.3066 + res * std::min(1.0, 3.0 * s) * (4.5 - 1.3066);
            const double k1 = 1.0 / 0.5412, k2 = 1.0 / q2;
            const double resComp = std::pow(q2 / 1.3066, -0.3);
            const double bodyGain = std::exp(bodyDb * s * kLn10Over20);
            const double makeup = std::exp(kMaxMakeupDb * s * kLn10Over20) * resComp;
            const double outGain = std::exp(outDb * engage * kLn10Over20);

            std::array<double, 2> chain{};
            for (int ch = 0; ch < channels; ++ch) {
                auto& st = channels_[static_cast<std::size_t>(ch)];
                const double x = static_cast<double>(in[ch][n]);
                // Chorus: modulated delay, 7 ms ± 2 ms · wobble.
                st.chorus[static_cast<std::size_t>(chorusWrite_)] = x;
                const double delaySamples =
                    (0.007 + 0.002 * wobble * lfo[static_cast<std::size_t>(ch)]) * sampleRate_;
                double readPos = chorusWrite_ - delaySamples;
                if (readPos < 0.0) readPos += chorusSize_;
                const int i0 = static_cast<int>(readPos);
                const double frac = readPos - i0;
                const int i1 = i0 + 1 >= chorusSize_ ? 0 : i0 + 1;
                const double delayed = st.chorus[static_cast<std::size_t>(i0)] * (1.0 - frac)
                    + st.chorus[static_cast<std::size_t>(i1)] * frac;
                const double xc = x + chorusMix * (delayed - x);

                // 24 dB/oct low-pass.
                const double octaves = 0.6 * wobble * s * lfo[static_cast<std::size_t>(ch)];
                const double cutoff = std::clamp(topCutoff_ * std::exp(s * logCutoffRatio_ + octaves * kLn2),
                                                 20.0, 0.49 * sampleRate_);
                if (ch == 0) cutoffL = cutoff;
                const double g = std::tan(kPi * cutoff / sampleRate_);
                setSvf(st.svf1, g, k1);
                setSvf(st.svf2, g, k2);
                const double low = st.svf2.process(st.svf1.process(xc).low).low;

                // Körper: first-order low shelf (x + (G-1)·LP).
                const double v = (low - st.bodyZ) * bodyG_;
                const double lp = v + st.bodyZ;
                st.bodyZ = sanitize(lp + v);
                chain[static_cast<std::size_t>(ch)] = (low + (bodyGain - 1.0) * lp) * makeup;
            }
            if (chorusWrite_ + 1 >= chorusSize_) chorusWrite_ = 0; else ++chorusWrite_;

            // Wasserhall.
            std::array<double, 2> wet{};
            const double send = reverbAmount * s;
            if (send > 0.0 || !reverbIdle_) {
                const double mono = channels == 2 ? 0.5 * (chain[0] + chain[1]) : chain[0];
                processReverb(mono * send, wet[0], wet[1]);
                const double peak = std::max(std::abs(wet[0]), std::abs(wet[1]));
                reverbPeak = std::max(reverbPeak, peak);
                if (send > 0.0 || peak > 1e-7) {
                    reverbIdle_ = false;
                    idleCount_ = 0;
                } else if (++idleCount_ > idleLimit_) {
                    reverbIdle_ = true;
                }
            }
            if (channels == 1) wet[0] = 0.5 * (wet[0] + wet[1]);

            for (int ch = 0; ch < channels; ++ch) {
                const double x = static_cast<double>(in[ch][n]);
                if (engage <= 0.0 && reverbIdle_) {
                    out[ch][n] = in[ch][n];
                    continue;
                }
                const double c = chain[static_cast<std::size_t>(ch)];
                const double wetSignal = engage * (c * outGain - x) + kReverbLevel * wet[static_cast<std::size_t>(ch)] * outGain;
                out[ch][n] = static_cast<T>(x + mix * wetSignal);
            }
            if (n == numSamples - 1) meters_.lfo = static_cast<float>(lfo[0]);
            meters_.engage = static_cast<float>(engage);
        }
        while (events && nextEvent < numEvents) {
            setParameter(events[nextEvent].id, events[nextEvent].normalized);
            ++nextEvent;
        }
        meters_.depth = static_cast<float>(depth_);
        meters_.cutoffHz = static_cast<float>(cutoffL);
        meters_.reverbLevel = static_cast<float>(reverbPeak);
    }

private:
    static constexpr std::size_t kLines = 8;
    static constexpr double kReverbRt60 = 1.2;
    static constexpr double kReverbLevel = 1.4;

    struct Delay {
        std::vector<double> buf;
        int pos = 0;
        void init(int length) {
            buf.assign(static_cast<std::size_t>(std::max(1, length)), 0.0);
            pos = 0;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0);
            pos = 0;
        }
        double read() const { return buf[static_cast<std::size_t>(pos)]; }
        void write(double v) {
            buf[static_cast<std::size_t>(pos)] = v;
            if (++pos >= static_cast<int>(buf.size())) pos = 0;
        }
        // Schroeder allpass with the delay as its memory.
        double allpass(double x, double g) {
            const double d = read();
            const double w = sanitize(x + g * d);
            write(w);
            return d - g * w;
        }
    };

    struct Channel {
        std::vector<double> chorus;
        SvfTpt svf1, svf2;
        double bodyZ = 0.0;
    };

    static void setSvf(SvfTpt& f, double g, double k) {
        f.g = g;
        f.k = k;
        f.a1 = 1.0 / (1.0 + g * (g + k));
        f.a2 = g * f.a1;
        f.a3 = g * f.a2;
    }

    void processReverb(double input, double& outL, double& outR) {
        // DC/rumble high-pass (one-pole TPT) so the tail does not get muddy.
        const double v = (input - reverbHpZ_) * reverbHpG_;
        const double lp = v + reverbHpZ_;
        reverbHpZ_ = sanitize(lp + v);
        double x = input - lp;

        const double pre = predelay_.read();
        predelay_.write(x);
        x = pre;
        for (auto& d : diffusers_) x = d.allpass(x, 0.6);

        std::array<double, kLines> o{};
        for (std::size_t i = 0; i < kLines; ++i) {
            // Damped line output (one-pole low-pass in the loop -> dark tail).
            const double raw = lines_[i].read() * lineGain_[i];
            const double dv = (raw - damp_[i]) * dampG_;
            const double dl = dv + damp_[i];
            damp_[i] = sanitize(dl + dv);
            o[i] = dl;
        }
        outL = 0.5 * (o[0] - o[1] + o[2] - o[3] + o[4] - o[5] + o[6] - o[7]) / 2.0;
        outR = 0.5 * (o[0] + o[1] - o[2] - o[3] + o[4] + o[5] - o[6] - o[7]) / 2.0;
        // In-place fast Walsh–Hadamard transform, normalized (orthogonal mixing matrix).
        for (std::size_t h = 1; h < kLines; h <<= 1)
            for (std::size_t i = 0; i < kLines; i += h << 1)
                for (std::size_t j = i; j < i + h; ++j) {
                    const double a = o[j], b = o[j + h];
                    o[j] = a + b;
                    o[j + h] = a - b;
                }
        constexpr double kNorm = 0.35355339059327373;  // 1/sqrt(8)
        for (std::size_t i = 0; i < kLines; ++i) {
            const double inject = (i & 1u) ? -x : x;
            lines_[i].write(sanitize(o[i] * kNorm + inject * 0.5));
        }
    }

    void updateTargets() {
        const auto p = [this](int id) { return toPlain(id, normalized_[static_cast<std::size_t>(id)]); };
        depthTarget_ = p(kDepth) * 0.01;
        returnSeconds_ = p(kReturnTime);
        diveSeconds_ = p(kDiveTime);
        resonance_ = p(kResonance) * 0.01;
        wobble_ = p(kWobble) * 0.01;
        reverb_ = p(kReverb) * 0.01;
        bodyDb_ = p(kBody);
        mix_ = p(kMix) * 0.01;
        outputDb_ = p(kOutput);
    }
    void updateTimeCoefficients() {
        // One-pole reaching 90 % after T: coef = exp(-ln(10) / (T · fs)).
        diveCoef_ = std::exp(-2.302585092994046 / (std::max(diveSeconds_, 1e-4) * sampleRate_));
        returnCoef_ = std::exp(-2.302585092994046 / (std::max(returnSeconds_, 1e-4) * sampleRate_));
    }

    std::array<double, kNumParams> normalized_{};
    double depthTarget_ = 0.0, returnSeconds_ = 2.0, diveSeconds_ = 0.05, resonance_ = 0.3, wobble_ = 0.35,
           reverb_ = 0.3, bodyDb_ = 2.5, mix_ = 1.0, outputDb_ = 0.0;

    double sampleRate_ = 44100.0;
    int maxBlock_ = 512;
    double topCutoff_ = 21000.0, logCutoffRatio_ = 0.0;
    double bodyG_ = 0.0, reverbHpG_ = 0.0, dampG_ = 0.0;
    double diveCoef_ = 0.0, returnCoef_ = 0.0;
    double depth_ = 0.0;
    Smoother resSmooth_, wobbleSmooth_, reverbSmooth_, bodySmooth_, mixSmooth_, outSmooth_;
    std::array<Channel, kMaxChannels> channels_{};
    int chorusSize_ = 1, chorusWrite_ = 0;
    std::array<double, 2> lfoPhase_{{0.0, 0.31}};

    Delay predelay_;
    std::array<Delay, 3> diffusers_{};
    std::array<Delay, kLines> lines_{};
    std::array<double, kLines> lineGain_{};
    std::array<double, kLines> damp_{};
    double reverbHpZ_ = 0.0;
    bool reverbIdle_ = true;
    int idleCount_ = 0, idleLimit_ = 2205;
    DrownerMeters meters_{};
};

} // namespace sonora::dsp::drowner
