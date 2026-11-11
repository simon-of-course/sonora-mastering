#pragma once

// SONORA Shutter engine (v0.2) — SDK-independent, header-only, real-time safe.
//
// Tempo-synced volume gate for rap vocals. The gate pattern is derived per sample from the
// host's musical position (quarter notes), so it is phase-locked to the grid regardless of
// host block size. All parameter changes are applied sample-accurately through the optional
// ParamEvent list passed to process() (events are applied at their sample offset inside the
// block; the glue does NOT have to split blocks). Calling setParameter() between process()
// calls is equivalent to an event at offset 0 of the next block.
//
// Gate model (all positions in "steps", one step = one note value):
//   base  = ppq / stepQuarters            (synced)   or free-running accumulator
//   P     = base - Versatz                (Versatz shifts the grid later by 0..1 step)
//   Steps are grouped in pairs. Swing delays the start of every 2nd (odd) step by
//   swing * 2/3 of a step (Swing 50 % = triplet shuffle, 75 % = hardest MPC-style swing).
//   Inside each step (local phase u in [0,1) relative to the step's actual length):
//     Hart : opens at u = 0, closes at u = Pulsbreite, raised-cosine edges of length
//            max(Glättung, 0.6 ms) (clamped so open/closed portions stay distinct).
//     Weich: same, edges as long as possible (half of the shorter portion).
//     Pumpe: drops from 1 to (1 - Tiefe) at u = 0 (edge length = Glättung), then swells back
//            to 1 with a smoothstep curve that completes at u = Pulsbreite (trap pump).
//   Ping-Pong (stereo buses only): L gates on even steps, R on odd steps; the inactive
//   channel stays closed (Hart/Weich) or un-ducked (Pumpe).
//   Gate-Klang: the closed portion is crossfaded (by Tiefe) to silence (Stumm), a driven
//   telephone band (400 Hz–3 kHz, Telefon) or a dull low-pass (700 Hz, Dumpf).
// A 0.25 ms slew limiter on the gate value catches discontinuities from parameter jumps or
// transport relocation. Enable/disable and transport start/stop use an 8 ms linear fade;
// fully disabled (or stopped) output is a bit-exact copy of the input.

#include "dsp_common.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sonora::dsp::shutter {

enum ParamId : int {
    kEnable = 0,     // Shutter aktiv
    kSync = 1,       // Tempo Sync
    kDivision = 2,   // Notenwert (12 entries)
    kRate = 3,       // Rate 0.5–20 Hz
    kDepth = 4,      // Tiefe 0–100 %
    kMix = 5,        // Mix 0–100 %
    kSmoothing = 6,  // Glättung 0–12 ms
    kDuty = 7,       // Pulsbreite 10–90 %
    kSwing = 8,      // Swing 0–75 %
    kShape = 9,      // Form Hart/Weich/Pumpe
    kStereo = 10,    // Stereo Mono/Ping-Pong
    kGateTone = 11,  // Gate-Klang Stumm/Telefon/Dumpf
    kOffset = 12,    // Versatz 0–100 %
    kNumParams = 13
};

enum Shape : int { kShapeHard = 0, kShapeSoft = 1, kShapePump = 2 };
enum StereoMode : int { kStereoMono = 0, kStereoPingPong = 1 };
enum GateTone : int { kToneMute = 0, kToneTelephone = 1, kToneDull = 2 };

constexpr int kNumDivisions = 12;
constexpr int kNumDivisionsV1 = 9;
// Length of one gate step in quarter notes, in list order.
constexpr std::array<double, kNumDivisions> kDivisionQuarters{{
    4.0, 2.0, 1.0, 0.5, 0.25, 0.125,          // 1/1 … 1/32
    1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0,         // 1/8 T, 1/16 T, 1/32 T
    1.5, 0.75, 0.375                          // 1/4 P, 1/8 P, 1/16 P (dotted)
}};
constexpr std::array<const char*, kNumDivisions> kDivisionNames{{
    "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T", "1/32 T",
    "1/4 P", "1/8 P", "1/16 P"}};
constexpr std::array<const char*, 3> kShapeNames{{"Hart", "Weich", "Pumpe"}};
constexpr std::array<const char*, 2> kStereoNames{{"Mono", "Ping-Pong"}};
constexpr std::array<const char*, 3> kGateToneNames{{"Stumm", "Telefon", "Dumpf"}};

// stepCount follows the VST3 convention: 0 = continuous, 1 = toggle/2-entry list,
// N = list with N + 1 entries (plain value = list index).
struct ParamInfo {
    const char* name;  // UTF-8, German
    const char* unit;
    double minPlain;
    double maxPlain;
    double defaultPlain;
    int stepCount;
};

constexpr std::array<ParamInfo, kNumParams> kParamInfo{{
    {"Shutter aktiv", "", 0.0, 1.0, 0.0, 1},
    {"Tempo Sync", "", 0.0, 1.0, 1.0, 1},
    {"Notenwert", "", 0.0, 11.0, 3.0, 11},
    {"Rate", "Hz", 0.5, 20.0, 4.0, 0},
    {"Tiefe", "%", 0.0, 100.0, 85.0, 0},
    {"Mix", "%", 0.0, 100.0, 100.0, 0},
    {"Glättung", "ms", 0.0, 12.0, 2.0, 0},
    {"Pulsbreite", "%", 10.0, 90.0, 50.0, 0},
    {"Swing", "%", 0.0, 75.0, 0.0, 0},
    {"Form", "", 0.0, 2.0, 0.0, 2},
    {"Stereo", "", 0.0, 1.0, 0.0, 1},
    {"Gate-Klang", "", 0.0, 2.0, 0.0, 2},
    {"Versatz", "%", 0.0, 100.0, 0.0, 0},
}};

constexpr double clampNormalized(double n) {
    return n != n ? 0.0 : (n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n));
}
constexpr int roundToInt(double v) {
    return v >= 0.0 ? static_cast<int>(v + 0.5) : -static_cast<int>(-v + 0.5);
}
constexpr bool isValidParam(int id) { return id >= 0 && id < kNumParams; }

// Normalized [0,1] -> plain value (list/toggle parameters return the index).
constexpr double toPlain(int id, double normalized) {
    if (!isValidParam(id)) return 0.0;
    const ParamInfo& p = kParamInfo[static_cast<std::size_t>(id)];
    const double n = clampNormalized(normalized);
    if (p.stepCount > 0) return p.minPlain + roundToInt(n * p.stepCount);
    return p.minPlain + n * (p.maxPlain - p.minPlain);
}
// Plain value -> normalized [0,1] (list/toggle values are snapped to the nearest entry).
constexpr double toNormalized(int id, double plain) {
    if (!isValidParam(id)) return 0.0;
    const ParamInfo& p = kParamInfo[static_cast<std::size_t>(id)];
    const double n = clampNormalized((plain - p.minPlain) / (p.maxPlain - p.minPlain));
    if (p.stepCount > 0) return static_cast<double>(roundToInt(n * p.stepCount)) / p.stepCount;
    return n;
}
constexpr double defaultNormalized(int id) {
    return isValidParam(id) ? toNormalized(id, kParamInfo[static_cast<std::size_t>(id)].defaultPlain)
                            : 0.0;
}
// List index for list/toggle parameters.
constexpr int listIndex(int id, double normalized) { return roundToInt(toPlain(id, normalized)); }

// State v1 stored "Notenwert" as a 9-entry StringList normalization (index / 8). The first
// nine entries keep their order in v2, so only the normalization has to be rescaled.
constexpr double migrateDivisionV1ToV2(double normalizedV1) {
    const int index = roundToInt(clampNormalized(normalizedV1) * (kNumDivisionsV1 - 1));
    return static_cast<double>(index) / (kNumDivisions - 1);
}
// v1 parameter IDs 0–6 keep their meaning and (apart from Notenwert) their normalization.
constexpr double migrateParamV1ToV2(int id, double normalizedV1) {
    return id == kDivision ? migrateDivisionV1ToV2(normalizedV1) : clampNormalized(normalizedV1);
}

struct TransportInfo {
    double tempo = 120.0;        // BPM
    double ppqPosition = 0.0;    // musical position (quarter notes) of the first sample of the block
    bool hasTempo = true;
    bool hasPosition = false;
    bool isPlaying = true;       // glue: true if the host gives no process context
};

struct ParamEvent {
    int sampleOffset;   // 0 … numSamples-1, sorted ascending
    int id;
    double normalized;
};

// Snapshot written at the end of every process() call (copy into the UI meter bridge).
struct ShutterMeters {
    float gainL = 1.0f;       // current effective gain (1 = open/dry)
    float gainR = 1.0f;
    float gateL = 1.0f;       // raw gate pattern value 0 (closed) … 1 (open)
    float gateR = 1.0f;
    float phase = 0.0f;       // position inside the current step, 0 … 1
    float pairPhase = 0.0f;   // position inside the current step pair, 0 … 1
    int stepParity = 0;       // 0 = even step, 1 = odd (swung / R in Ping-Pong)
    float active = 0.0f;      // enable/transport fade 0 … 1
};

// Pattern value of one step (0 = closed, 1 = open). `edge` is the edge length in step units.
inline double gateShape(double u, double duty, double edge, int shape) {
    const auto rc = [](double t) { return 0.5 - 0.5 * std::cos(kPi * t); };
    if (shape == kShapePump) {
        if (edge > 0.0 && u < edge) return 1.0 - rc(u / edge);
        const double recoverEnd = std::max(duty, edge + 1e-6);
        if (u >= recoverEnd) return 1.0;
        const double t = std::clamp((u - edge) / (recoverEnd - edge), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    if (edge > 0.0 && u < edge) return rc(u / edge);
    if (u < duty) return 1.0;
    if (edge > 0.0 && u < duty + edge) return 1.0 - rc((u - duty) / edge);
    return 0.0;
}

// Edge length in step units for the given shape. stepSeconds = actual length of the step.
inline double gateEdge(int shape, double duty, double smoothingMs, double stepSeconds) {
    const double minimum = std::max(smoothingMs, 0.6) * 0.001 / std::max(stepSeconds, 1e-9);
    if (shape == kShapePump) return std::min(minimum, 0.5 * duty);
    const double maxEdge = 0.5 * std::min(duty, 1.0 - duty);
    return shape == kShapeSoft ? maxEdge : std::min(minimum, maxEdge);
}

struct StepPosition {
    int parity;      // 0 even, 1 odd
    double u;        // 0 … 1 inside the step
    double length;   // actual step length in nominal steps (1 ± swing delay)
};

// Maps a continuous step position (offset already applied) to the swung step layout.
inline StepPosition locateStep(double position, double swing) {
    const double pair = position - 2.0 * std::floor(position * 0.5);
    const double delay = std::clamp(swing, 0.0, 0.75) * (2.0 / 3.0);
    if (pair < 1.0 + delay) {
        const double len = 1.0 + delay;
        return {0, std::clamp(pair / len, 0.0, 1.0), len};
    }
    const double len = 1.0 - delay;
    return {1, std::clamp((pair - 1.0 - delay) / len, 0.0, 1.0), len};
}

class ShutterEngine {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr double kFadeSeconds = 0.008;
    static constexpr double kSlewSeconds = 0.00025;
    static constexpr double kTelephoneDrive = 2.0;

    ShutterEngine() {
        for (int id = 0; id < kNumParams; ++id) normalized_[static_cast<std::size_t>(id)] = defaultNormalized(id);
        updateTargets();
        prepare(44100.0, 512);
    }

    void prepare(double sampleRate, int maxBlock) {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock_ = std::max(1, maxBlock);
        depthSmooth_.setTime(0.005, sampleRate_);
        mixSmooth_.setTime(0.005, sampleRate_);
        smoothingSmooth_.setTime(0.02, sampleRate_);
        dutySmooth_.setTime(0.015, sampleRate_);
        swingSmooth_.setTime(0.015, sampleRate_);
        offsetSmooth_.setTime(0.02, sampleRate_);
        for (auto& w : toneWeights_) w.setTime(0.006, sampleRate_);
        const Biquad hp = Biquad::highPass(sampleRate_, 400.0, 0.8);
        const Biquad lp = Biquad::lowPass(sampleRate_, 3000.0, 0.8);
        const Biquad dull = Biquad::lowPass(sampleRate_, 700.0, 0.7071);
        for (auto& ch : channels_) {
            ch.teleHp.setCoefficients(hp);
            ch.teleLp.setCoefficients(lp);
            ch.dullLp.setCoefficients(dull);
        }
        fadeStep_ = 1.0 / (kFadeSeconds * sampleRate_);
        slewStep_ = 1.0 / (kSlewSeconds * sampleRate_);
        reset();
    }

    void reset() {
        updateTargets();
        depthSmooth_.reset(depth_);
        mixSmooth_.reset(mix_);
        smoothingSmooth_.reset(smoothingMs_);
        dutySmooth_.reset(duty_);
        swingSmooth_.reset(swing_);
        offsetSmooth_.reset(offset_);
        for (int i = 0; i < 3; ++i) toneWeights_[static_cast<std::size_t>(i)].reset(i == tone_ ? 1.0 : 0.0);
        for (auto& ch : channels_) {
            ch.teleHp.reset();
            ch.teleLp.reset();
            ch.dullLp.reset();
            ch.gate = 1.0;
        }
        active_ = 0.0;
        freePosition_ = 0.0;
        meters_ = {};
    }

    int latencySamples() const { return 0; }
    double sampleRate() const { return sampleRate_; }

    void setParameter(int id, double normalized) {
        if (!isValidParam(id)) return;
        normalized_[static_cast<std::size_t>(id)] = clampNormalized(normalized);
        updateTargets();
    }
    double getParameter(int id) const {
        return isValidParam(id) ? normalized_[static_cast<std::size_t>(id)] : 0.0;
    }

    const ShutterMeters& meters() const { return meters_; }

    // in/out may alias (in-place). Channels beyond 2 are passed through unchanged.
    template <typename T>
    void process(const T* const* in, T* const* out, int numChannels, int numSamples,
                 const TransportInfo& transport, const ParamEvent* events = nullptr,
                 int numEvents = 0) {
        if (numChannels <= 0 || numSamples <= 0 || !in || !out) return;
        const int channels = std::min(numChannels, kMaxChannels);
        for (int ch = channels; ch < numChannels; ++ch)
            if (in[ch] != out[ch]) std::copy(in[ch], in[ch] + numSamples, out[ch]);

        const bool tempoValid = transport.hasTempo && std::isfinite(transport.tempo) && transport.tempo > 0.0;
        const double tempo = tempoValid ? std::clamp(transport.tempo, 1.0, 999.0) : 120.0;
        const bool positionValid = transport.hasPosition && std::isfinite(transport.ppqPosition);
        const double quartersPerSample = tempo / (60.0 * sampleRate_);
        const bool playing = transport.isPlaying;

        int nextEvent = 0;
        for (int n = 0; n < numSamples; ++n) {
            while (events && nextEvent < numEvents && events[nextEvent].sampleOffset <= n) {
                setParameter(events[nextEvent].id, events[nextEvent].normalized);
                ++nextEvent;
            }

            const double depth = depthSmooth_.next(depth_);
            const double mix = mixSmooth_.next(mix_);
            const double smoothingMs = smoothingSmooth_.next(smoothingMs_);
            const double duty = dutySmooth_.next(duty_);
            const double swing = swingSmooth_.next(swing_);
            const double offset = offsetSmooth_.next(offset_);
            std::array<double, 3> tone{};
            for (int i = 0; i < 3; ++i)
                tone[static_cast<std::size_t>(i)] = toneWeights_[static_cast<std::size_t>(i)].next(i == tone_ ? 1.0 : 0.0);

            const double stepQuarters = kDivisionQuarters[static_cast<std::size_t>(division_)];
            const bool useSync = sync_ && positionValid;
            const double stepsPerSecond = sync_ ? (tempo / 60.0) / stepQuarters : rateHz_;
            double base;
            if (useSync) {
                base = (transport.ppqPosition + static_cast<double>(n) * quartersPerSample) / stepQuarters;
                freePosition_ = base - 2.0 * std::floor(base * 0.5);
            } else {
                base = freePosition_;
                if (playing) {
                    freePosition_ += stepsPerSecond / sampleRate_;
                    if (freePosition_ >= 2.0) freePosition_ -= 2.0 * std::floor(freePosition_ * 0.5);
                }
            }

            const StepPosition step = locateStep(base - offset, swing);
            const double stepSeconds = step.length / std::max(stepsPerSecond, 1e-6);
            const double edge = gateEdge(shape_, duty, smoothingMs, stepSeconds);
            const double pattern = gateShape(step.u, duty, edge, shape_);
            const double idle = shape_ == kShapePump ? 1.0 : 0.0;
            const bool pingPong = stereoMode_ == kStereoPingPong && channels == 2;

            const double activeTarget = enabled_ && playing ? 1.0 : 0.0;
            active_ = activeTarget > active_ ? std::min(activeTarget, active_ + fadeStep_)
                                             : std::max(activeTarget, active_ - fadeStep_);

            for (int ch = 0; ch < channels; ++ch) {
                auto& state = channels_[static_cast<std::size_t>(ch)];
                const double target = pingPong && step.parity != ch ? idle : pattern;
                state.gate += std::clamp(target - state.gate, -slewStep_, slewStep_);

                const double x = static_cast<double>(in[ch][n]);
                // Driven telephone band (small-signal gain -2 dB) and dull low-pass (-3 dB).
                const double band = state.teleLp.process(state.teleHp.process(x));
                const double tele = std::tanh(kTelephoneDrive * band) * (0.8 / kTelephoneDrive);
                const double dull = state.dullLp.process(x) * 0.7;
                const double closed = tone[1] * tele + tone[2] * dull;
                const double amount = depth * mix * active_ * (1.0 - state.gate);
                state.gain = 1.0 - amount;
                if (active_ <= 0.0) {
                    out[ch][n] = in[ch][n];
                } else {
                    out[ch][n] = static_cast<T>(x * (1.0 - amount) + closed * amount);
                }
            }
            if (n == numSamples - 1) {
                meters_.phase = static_cast<float>(step.u);
                const double pair = (base - offset) - 2.0 * std::floor((base - offset) * 0.5);
                meters_.pairPhase = static_cast<float>(pair * 0.5);
                meters_.stepParity = step.parity;
            }
        }
        while (events && nextEvent < numEvents) {
            setParameter(events[nextEvent].id, events[nextEvent].normalized);
            ++nextEvent;
        }
        meters_.gainL = static_cast<float>(channels_[0].gain);
        meters_.gainR = static_cast<float>(channels > 1 ? channels_[1].gain : channels_[0].gain);
        meters_.gateL = static_cast<float>(channels_[0].gate);
        meters_.gateR = static_cast<float>(channels > 1 ? channels_[1].gate : channels_[0].gate);
        meters_.active = static_cast<float>(active_);
    }

private:
    struct Channel {
        Biquad teleHp, teleLp, dullLp;
        double gate = 1.0;
        double gain = 1.0;
    };

    void updateTargets() {
        const auto n = [this](int id) { return normalized_[static_cast<std::size_t>(id)]; };
        enabled_ = n(kEnable) >= 0.5;
        sync_ = n(kSync) >= 0.5;
        division_ = std::clamp(listIndex(kDivision, n(kDivision)), 0, kNumDivisions - 1);
        rateHz_ = toPlain(kRate, n(kRate));
        depth_ = toPlain(kDepth, n(kDepth)) * 0.01;
        mix_ = toPlain(kMix, n(kMix)) * 0.01;
        smoothingMs_ = toPlain(kSmoothing, n(kSmoothing));
        duty_ = toPlain(kDuty, n(kDuty)) * 0.01;
        swing_ = toPlain(kSwing, n(kSwing)) * 0.01;
        shape_ = std::clamp(listIndex(kShape, n(kShape)), 0, 2);
        stereoMode_ = std::clamp(listIndex(kStereo, n(kStereo)), 0, 1);
        tone_ = std::clamp(listIndex(kGateTone, n(kGateTone)), 0, 2);
        offset_ = toPlain(kOffset, n(kOffset)) * 0.01;
    }

    std::array<double, kNumParams> normalized_{};
    bool enabled_ = false, sync_ = true;
    int division_ = 3, shape_ = 0, stereoMode_ = 0, tone_ = 0;
    double rateHz_ = 4.0, depth_ = 0.85, mix_ = 1.0, smoothingMs_ = 2.0, duty_ = 0.5, swing_ = 0.0,
           offset_ = 0.0;

    double sampleRate_ = 44100.0;
    int maxBlock_ = 512;
    double fadeStep_ = 1.0 / (kFadeSeconds * 44100.0);
    double slewStep_ = 1.0 / (kSlewSeconds * 44100.0);
    Smoother depthSmooth_, mixSmooth_, smoothingSmooth_, dutySmooth_, swingSmooth_, offsetSmooth_;
    std::array<Smoother, 3> toneWeights_{};
    std::array<Channel, kMaxChannels> channels_{};
    double active_ = 0.0;
    double freePosition_ = 0.0;
    ShutterMeters meters_{};
};

} // namespace sonora::dsp::shutter
