#pragma once

// SONORA meter bridges: lock-free, SDK-independent structs that the audio thread writes and the
// editor (UI thread) reads. Every field is a relaxed std::atomic, so a torn/out-of-date frame is
// possible but harmless for metering. No allocation, no locks: safe to call from process().
//
// Ownership: one bridge per plugin instance (member of the SingleComponentEffect), handed to the
// editor views through sonora::ui::*UiContext (see sonora_ui.h). Views tolerate a null bridge.
//
// Writer side (audio thread, once per process() block):
//   MasterMeterBridge::publish(engine.meters()) copies a sonora::dsp::master::MasterMeters snapshot
//   (field-by-field, duck-typed so this header stays engine-free); alternatively the granular
//   publishLevels / publishLoudness / publishDynamics / publishAutoEq / publishAnalysis* calls.
//   ShutterMeterBridge::publish ; DrownerMeterBridge::publish
// Reader side (UI thread): load() the fields, or PeakCell::take() for peak meters (single consumer).
// UI -> audio requests: MasterMeterBridge::requestLoudnessReset() / consumeLoudnessResetRequest().

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace sonora::ui {

constexpr float kMeterFloorDb = -120.0f;

// Linear peak accumulator. The audio thread pushes block peaks (max |x|); the single UI consumer
// takes the maximum since its last read and resets it. Lost updates only ever lose values that
// are smaller than what the reader already received.
struct PeakCell {
    std::atomic<float> value{0.0f};

    void push(float peak) noexcept {
        if (!(peak >= 0.0f)) { // also rejects NaN
            return;
        }
        if (peak > value.load(std::memory_order_relaxed)) {
            value.store(peak, std::memory_order_relaxed);
        }
    }
    float take() noexcept { return value.exchange(0.0f, std::memory_order_relaxed); }
    float peek() const noexcept { return value.load(std::memory_order_relaxed); }
};

// Same numeric values as v0.1 AnalysisReadout (0..6) and sonora::dsp::master::AnalysisStatus
// (static_assert'ed in sonora_ui.h). 7: Master mode without sidechain, mix-only analysis.
enum AnalysisState : int {
    kAnalysisReady = 0,
    kAnalysisRunning = 1,
    kAnalysisComplete = 2,
    kAnalysisNoVocal = 3,
    kAnalysisMissingSidechain = 4,
    kAnalysisWrongMode = 5,
    kAnalysisTooQuiet = 6,
    kAnalysisCompleteMixOnly = 7
};

inline float sanitizeMeter(float v, float fallback) noexcept {
    return std::isfinite(v) ? v : fallback;
}

struct MasterMeterBridge {
    static constexpr int kBands = 5; // 60 / 250 / 1k / 4k / 12k

    // --- levels (linear amplitude, max since last UI read) ---
    std::array<PeakCell, 2> inputPeak{};
    std::array<PeakCell, 2> outputPeak{};
    // True-peak maximum (dBTP) held since the last loudness reset.
    std::atomic<float> truePeakMaxDb{kMeterFloorDb};
    std::atomic<float> outputTruePeakDb{kMeterFloorDb}; // true peak of the last block (dBTP)

    // --- loudness (LUFS, BS.1770-4; <= -70 means "no reading yet") ---
    std::atomic<float> momentaryLufs{kMeterFloorDb};
    std::atomic<float> shortTermLufs{kMeterFloorDb};
    std::atomic<float> integratedLufs{kMeterFloorDb};

    // --- dynamics ---
    std::atomic<float> glueGainReductionDb{0.0f};    // >= 0, amount of reduction
    std::atomic<float> limiterGainReductionDb{0.0f}; // >= 0, amount of reduction
    std::atomic<float> clipGainReductionDb{0.0f};    // >= 0
    std::atomic<float> clipActivity{0.0f};           // 0..1 (0 = clipper idle)
    std::atomic<float> deEsserGainReductionDb{0.0f}; // >= 0
    std::atomic<float> duckDb{0.0f};                 // >= 0, sidechain ducking depth
    std::atomic<float> correlation{1.0f};            // -1..+1
    std::atomic<float> sampleRate{48000.0f};         // for the EQ display (optional)

    // --- auto-EQ (dB offsets added by the analysis, per band) ---
    std::array<std::atomic<double>, kBands> autoEqDb{};

    // --- analysis readout (same semantics as v0.1 AnalysisReadout) ---
    std::atomic<int> analysisStatus{kAnalysisReady};
    std::atomic<double> analysisProgress{0.0}; // 0..1
    std::array<std::atomic<double>, kBands> beatBandsDb{};
    std::array<std::atomic<double>, kBands> vocalBandsDb{};
    std::atomic<double> beatRmsDb{-120.0};
    std::atomic<double> vocalRmsDb{-120.0};
    std::atomic<double> beatCrestDb{0.0};
    std::atomic<double> autoCompRatio{1.0};
    std::atomic<int> latencySamples{0};

    // --- UI -> audio request ---
    std::atomic<bool> loudnessResetRequest{false};

    MasterMeterBridge() noexcept { reset(); }

    void reset() noexcept {
        for (auto& p : inputPeak) p.value.store(0.0f, std::memory_order_relaxed);
        for (auto& p : outputPeak) p.value.store(0.0f, std::memory_order_relaxed);
        truePeakMaxDb.store(kMeterFloorDb, std::memory_order_relaxed);
        momentaryLufs.store(kMeterFloorDb, std::memory_order_relaxed);
        shortTermLufs.store(kMeterFloorDb, std::memory_order_relaxed);
        integratedLufs.store(kMeterFloorDb, std::memory_order_relaxed);
        glueGainReductionDb.store(0.0f, std::memory_order_relaxed);
        limiterGainReductionDb.store(0.0f, std::memory_order_relaxed);
        outputTruePeakDb.store(kMeterFloorDb, std::memory_order_relaxed);
        clipGainReductionDb.store(0.0f, std::memory_order_relaxed);
        clipActivity.store(0.0f, std::memory_order_relaxed);
        deEsserGainReductionDb.store(0.0f, std::memory_order_relaxed);
        duckDb.store(0.0f, std::memory_order_relaxed);
        correlation.store(1.0f, std::memory_order_relaxed);
        for (auto& g : autoEqDb) g.store(0.0, std::memory_order_relaxed);
        analysisStatus.store(kAnalysisReady, std::memory_order_relaxed);
        analysisProgress.store(0.0, std::memory_order_relaxed);
        for (auto& b : beatBandsDb) b.store(-120.0, std::memory_order_relaxed);
        for (auto& b : vocalBandsDb) b.store(-120.0, std::memory_order_relaxed);
        beatRmsDb.store(-120.0, std::memory_order_relaxed);
        vocalRmsDb.store(-120.0, std::memory_order_relaxed);
        beatCrestDb.store(0.0, std::memory_order_relaxed);
        autoCompRatio.store(1.0, std::memory_order_relaxed);
        latencySamples.store(0, std::memory_order_relaxed);
    }

    // One-call copy of a sonora::dsp::master::MasterMeters snapshot (same field names), e.g.
    //   bridge.publish(engine.meters()); bridge.publishSampleRate(sampleRate);
    // Template so this header does not depend on the engine.
    template <typename Meters>
    void publish(const Meters& m) noexcept {
        publishLevels(m.inputPeak[0], m.inputPeak[1], m.outputPeak[0], m.outputPeak[1]);
        publishLoudness(m.momentaryLufs, m.shortTermLufs, m.integratedLufs, m.truePeakMaxDb);
        outputTruePeakDb.store(sanitizeMeter(m.outputTruePeakDb, kMeterFloorDb), std::memory_order_relaxed);
        publishDynamics(m.glueGrDb, m.limiterGrDb, m.clipActivity, m.correlation);
        clipGainReductionDb.store(std::max(0.0f, sanitizeMeter(m.clipGrDb, 0.0f)), std::memory_order_relaxed);
        deEsserGainReductionDb.store(std::max(0.0f, sanitizeMeter(m.deEsserGrDb, 0.0f)), std::memory_order_relaxed);
        duckDb.store(std::max(0.0f, sanitizeMeter(m.duckDb, 0.0f)), std::memory_order_relaxed);
        publishAutoEq(m.autoEqDb);
        for (int i = 0; i < kBands; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            beatBandsDb[idx].store(m.beatBandsDb[i], std::memory_order_relaxed);
            vocalBandsDb[idx].store(m.vocalBandsDb[i], std::memory_order_relaxed);
        }
        beatRmsDb.store(m.beatRmsDb, std::memory_order_relaxed);
        vocalRmsDb.store(m.vocalRmsDb, std::memory_order_relaxed);
        beatCrestDb.store(m.beatCrestDb, std::memory_order_relaxed);
        autoCompRatio.store(m.autoCompRatio, std::memory_order_relaxed);
        latencySamples.store(m.latencySamples, std::memory_order_relaxed);
        publishAnalysisStatus(static_cast<AnalysisState>(m.analysisStatus), m.analysisProgress);
    }

    // Block peaks, linear |x| maxima (pass 0 for a missing channel; mono: pass L twice).
    void publishLevels(float inL, float inR, float outL, float outR) noexcept {
        inputPeak[0].push(inL);
        inputPeak[1].push(inR);
        outputPeak[0].push(outL);
        outputPeak[1].push(outR);
    }

    // LUFS values and the true-peak max hold in dBTP.
    void publishLoudness(float momentary, float shortTerm, float integrated,
                         float truePeakMax) noexcept {
        momentaryLufs.store(sanitizeMeter(momentary, kMeterFloorDb), std::memory_order_relaxed);
        shortTermLufs.store(sanitizeMeter(shortTerm, kMeterFloorDb), std::memory_order_relaxed);
        integratedLufs.store(sanitizeMeter(integrated, kMeterFloorDb), std::memory_order_relaxed);
        truePeakMaxDb.store(sanitizeMeter(truePeakMax, kMeterFloorDb), std::memory_order_relaxed);
    }

    // Gain reduction as positive dB, clipper activity 0..1, correlation -1..1.
    void publishDynamics(float glueGrDb, float limiterGrDb, float clip, float corr) noexcept {
        glueGainReductionDb.store(std::max(0.0f, sanitizeMeter(glueGrDb, 0.0f)),
                                  std::memory_order_relaxed);
        limiterGainReductionDb.store(std::max(0.0f, sanitizeMeter(limiterGrDb, 0.0f)),
                                     std::memory_order_relaxed);
        clipActivity.store(std::clamp(sanitizeMeter(clip, 0.0f), 0.0f, 1.0f),
                           std::memory_order_relaxed);
        correlation.store(std::clamp(sanitizeMeter(corr, 0.0f), -1.0f, 1.0f),
                          std::memory_order_relaxed);
    }

    void publishAutoEq(const double* gainsDb) noexcept {
        if (!gainsDb) return;
        for (int i = 0; i < kBands; ++i) {
            autoEqDb[static_cast<std::size_t>(i)].store(
                std::isfinite(gainsDb[i]) ? gainsDb[i] : 0.0, std::memory_order_relaxed);
        }
    }

    void publishSampleRate(double sr) noexcept {
        if (sr > 0.0 && std::isfinite(sr)) sampleRate.store(static_cast<float>(sr), std::memory_order_relaxed);
    }

    void publishAnalysisStatus(AnalysisState status, double progress) noexcept {
        analysisProgress.store(std::clamp(progress, 0.0, 1.0), std::memory_order_relaxed);
        analysisStatus.store(status, std::memory_order_release);
    }

    // Final analysis result; bands are per-band RMS in dBFS like v0.1.
    void publishAnalysisResult(AnalysisState status, const double* beatBands,
                               const double* vocalBands, double beatRms, double vocalRms,
                               double beatCrest) noexcept {
        for (int i = 0; i < kBands; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            if (beatBands) beatBandsDb[idx].store(beatBands[i], std::memory_order_relaxed);
            if (vocalBands) vocalBandsDb[idx].store(vocalBands[i], std::memory_order_relaxed);
        }
        beatRmsDb.store(beatRms, std::memory_order_relaxed);
        vocalRmsDb.store(vocalRms, std::memory_order_relaxed);
        beatCrestDb.store(beatCrest, std::memory_order_relaxed);
        publishAnalysisStatus(status, 1.0);
    }

    // UI thread: click on the loudness display. Audio thread: poll once per block and reset
    // integrated loudness + true-peak hold when it returns true.
    void requestLoudnessReset() noexcept { loudnessResetRequest.store(true, std::memory_order_release); }
    bool consumeLoudnessResetRequest() noexcept {
        return loudnessResetRequest.load(std::memory_order_relaxed)
            && loudnessResetRequest.exchange(false, std::memory_order_acq_rel);
    }
};

struct ShutterMeterBridge {
    std::array<std::atomic<float>, 2> gateGain{}; // current gate gain per channel 0..1
    // Position of the playhead inside the 2-bar window drawn by the ShutterScope, 0..1
    // (synced: (ppqPosition mod 8) / 8 ; free rate: (cycles mod 8) / 8).
    std::atomic<float> phase{0.0f};
    std::atomic<bool> active{false};  // gate currently engaged (enabled and transport running)
    std::atomic<bool> playing{false}; // host transport running

    ShutterMeterBridge() noexcept {
        for (auto& g : gateGain) g.store(1.0f, std::memory_order_relaxed);
    }

    void publish(float gainL, float gainR, float phase01, bool isActive, bool isPlaying) noexcept {
        gateGain[0].store(std::clamp(sanitizeMeter(gainL, 1.0f), 0.0f, 1.0f), std::memory_order_relaxed);
        gateGain[1].store(std::clamp(sanitizeMeter(gainR, 1.0f), 0.0f, 1.0f), std::memory_order_relaxed);
        float p = sanitizeMeter(phase01, 0.0f);
        p = p - std::floor(p);
        phase.store(p, std::memory_order_relaxed);
        active.store(isActive, std::memory_order_relaxed);
        playing.store(isPlaying, std::memory_order_relaxed);
    }
};

struct DrownerMeterBridge {
    std::atomic<float> depth{0.0f};       // smoothed (dive/return applied) depth 0..1
    std::atomic<float> cutoffHz{20000.0f}; // current low-pass cutoff

    void publish(float smoothedDepth, float cutoff) noexcept {
        depth.store(std::clamp(sanitizeMeter(smoothedDepth, 0.0f), 0.0f, 1.0f), std::memory_order_relaxed);
        cutoffHz.store(std::max(1.0f, sanitizeMeter(cutoff, 20000.0f)), std::memory_order_relaxed);
    }
};

} // namespace sonora::ui
