// SONORA Master — VST3 glue (SingleComponentEffect + VST3Editor) around
// sonora::dsp::MasterEngine (plugin/src/dsp/master_engine.h).
//
// Threading: parameter values live in atomics written by setParamNormalized (UI/host thread) and
// read once per block by process(). Host automation arrives as IParamValueQueue points; they are
// merged and sorted by sample offset, and the block is split at every automation offset so each
// value change lands on its exact sample (the engine smooths internally and is block-size
// invariant). The last point of every queue is written back to the atomics.
//
// "Analyse starten" (ID 17): the editor control is a momentary trigger (sonora::ui::Toggle with
// setMomentary: 1 in one edit gesture, back to 0 ~250 ms later in a second one), so only a rising
// edge (0 -> 1) starts a new 8-second analysis. Edges are detected on the audio thread only
// (block-start value + automation points), so a UI click that reaches the plug-in both through
// setParamNormalized and through the parameter queue triggers exactly once. Loading a state never
// triggers; the value is not persisted (always stored/restored as 0).
//
// State v5: int32 version (5), int32 parameter count, one double (normalized) per parameter, then
// an optional analysis block: int32 count (6), 5 auto-EQ gains (dB) + auto-comp ratio.
// States v1-v4 (v0.1) hold a fixed number of normalized doubles (6/16/17/18, IDs 0..n-1) and are
// migrated (see migrateLegacyState).

#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "base/source/fstreamer.h"
#include "vstgui/plugin-bindings/vst3editor.h"

#include "dsp/master_engine.h"
#include "ui/param_format.h"
#include "ui/sonora_ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define SONORA_HAS_MXCSR 1
#endif

#ifndef SONORA_VERSION_STRING
#define SONORA_VERSION_STRING "0.2.0"
#endif

namespace {

namespace M = sonora::dsp::master;
using Steinberg::int32;
using Steinberg::tresult;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;

constexpr int kNumParams = M::kNumParams;
constexpr int32 kStateVersion = 5;
constexpr int kEventCapacity = 8192;
constexpr int kAnalysisStateCount = M::kBands + 1; // auto-EQ gains + auto-comp ratio
constexpr double kLegacyDefaultAmount = 0.3;      // v0.1 default Stärke (30 %)

constexpr std::array<const char*, 3> kModeNames{{"Vocal", "Beat", "Master"}};

struct ParamEvent {
    int32 offset;
    int id;
    double normalized;
};

// Flush denormals to zero for the duration of process() (x86 only).
class DenormalGuard {
public:
    DenormalGuard() {
#ifdef SONORA_HAS_MXCSR
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u); // FTZ | DAZ
#endif
    }
    ~DenormalGuard() {
#ifdef SONORA_HAS_MXCSR
        _mm_setcsr(saved_);
#endif
    }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
#ifdef SONORA_HAS_MXCSR
    unsigned int saved_ = 0;
#endif
};

double sanitizeNormalized(double v, double fallback) {
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : fallback;
}

void toUtf16(const char* utf8, Steinberg::Vst::String128 out) {
    sonora::ui::FormattedParameter::utf8ToUtf16(utf8, out, 128);
}

// On/off parameter with German value texts ("An"/"Aus").
class ToggleParameter : public Steinberg::Vst::Parameter {
public:
    ToggleParameter(const char* title, ParamID id, bool defaultOn, int32 extraFlags = 0) {
        toUtf16(title, info.title);
        info.id = id;
        info.stepCount = 1;
        info.defaultNormalizedValue = defaultOn ? 1.0 : 0.0;
        info.flags = Steinberg::Vst::ParameterInfo::kCanAutomate | extraFlags;
        info.unitId = Steinberg::Vst::kRootUnitId;
        setNormalized(info.defaultNormalizedValue);
    }
    void toString(ParamValue normalized, Steinberg::Vst::String128 string) const SMTG_OVERRIDE {
        toUtf16(normalized >= 0.5 ? "An" : "Aus", string);
    }
    bool fromString(const Steinberg::Vst::TChar* string, ParamValue& normalized) const SMTG_OVERRIDE {
        if (!string || !string[0]) return false;
        const auto c0 = string[0] | 0x20, c1 = string[1] | 0x20;
        if (c0 == 'a' && c1 == 'n') normalized = 1.0;          // "An"
        else if (c0 == 'a' && c1 == 'u') normalized = 0.0;     // "Aus"
        else if (c0 == 'o' && c1 == 'n') normalized = 1.0;     // "On"
        else if (c0 == 'o' && c1 == 'f') normalized = 0.0;     // "Off"
        else if (c0 == '1') normalized = 1.0;
        else if (c0 == '0') normalized = 0.0;
        else return false;
        return true;
    }
    OBJ_METHODS(ToggleParameter, Parameter)
};

template <typename Names>
Steinberg::Vst::StringListParameter* makeList(int id, const Names& names, int count) {
    Steinberg::Vst::String128 title{};
    toUtf16(M::kParams[static_cast<std::size_t>(id)].name, title);
    auto* p = new Steinberg::Vst::StringListParameter(title, static_cast<ParamID>(id));
    for (int i = 0; i < count; ++i) {
        Steinberg::Vst::String128 s{};
        toUtf16(names(i), s);
        p->appendString(s);
    }
    const double def = M::defaultNormalized(id);
    p->getInfo().defaultNormalizedValue = def;
    p->setNormalized(def);
    return p;
}

sonora::ui::ParamFormat formatFor(int id) {
    using sonora::ui::ParamFormat;
    switch (id) {
    case M::kBass:
    case M::kPresence:
    case M::kOutput:
    case M::kBeatHighs:
    case M::kEq60:
    case M::kEq250:
    case M::kEq1k:
    case M::kEq4k:
    case M::kEq12k:
    case M::kLoudness:
    case M::kInput: return ParamFormat::db(1, true);
    case M::kCeiling: return ParamFormat::dbtp(1);
    case M::kBassMono: return ParamFormat::hz(0).withZeroText("Aus");
    default: return ParamFormat::percent(0); // Stärke, Nasalität, Resonanzen, Sättigung, Vocal-Fokus, Glue, ...
    }
}

// v0.1 (state v1-v4) -> v0.2 migration, in place on normalized values.
//  * Bass, Stimmen-Präsenz, Beat-Höhen, Nasalität and Resonanzen were scaled by Stärke in v0.1;
//    v0.2 applies them directly, so the stored plain value is multiplied by the old Stärke.
//  * Preset: 6-entry list -> 12-entry list (same order, new rap presets appended).
//  * Bus-Modus keeps its values. NOTE (intended v0.2 change): v0.1 "Vocal" and "Beat" modes were
//    pass-through except for EQ/ducking details; in v0.2 they run the full Vocal/Beat chains.
//  * IDs 18-26 did not exist and keep their v0.2 defaults; "Analyse starten" is not restored.
void migrateLegacyState(std::array<double, kNumParams>& v, int loadedCount) {
    const double oldAmount = loadedCount > M::kAmount ? std::clamp(v[M::kAmount], 0.0, 1.0) : kLegacyDefaultAmount;
    if (loadedCount <= M::kAmount) v[M::kAmount] = kLegacyDefaultAmount;
    for (int id : {M::kBass, M::kPresence, M::kBeatHighs, M::kNasal, M::kResonance}) {
        if (id >= loadedCount) continue;
        auto& n = v[static_cast<std::size_t>(id)];
        n = M::toNormalized(id, M::toPlain(id, n) * oldAmount);
    }
    if (loadedCount > M::kPreset) v[M::kPreset] = M::migrateLegacyPresetNormalized(v[M::kPreset]);
}

} // namespace

class SonoraMaster final : public Steinberg::Vst::SingleComponentEffect,
                           public VSTGUI::VST3EditorDelegate {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new SonoraMaster());
    }

    SonoraMaster() {
        for (int id = 0; id < kNumParams; ++id) {
            values_[static_cast<std::size_t>(id)].store(M::defaultNormalized(id));
            applied_[static_cast<std::size_t>(id)] = -1.0;
        }
        for (int i = 0; i < kAnalysisStateCount; ++i) analysisState_[static_cast<std::size_t>(i)].store(i < M::kBands ? 0.0 : 1.0);
        events_.resize(kEventCapacity);
        sorted_.resize(kEventCapacity);
        order_.resize(kEventCapacity);
        uiContext_.meters = &meters_;
        uiContext_.getParam = [this](std::uint32_t id) { return getParamNormalized(id); };
    }

    // --- IComponent / IAudioProcessor ------------------------------------------------------------

    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != Steinberg::kResultOk) return result;
        addAudioInput(STR16("Stereo In"), Steinberg::Vst::SpeakerArr::kStereo);
        // Optional vocal stem for ducking/analysis; inactive by default (as in v0.1).
        addAudioInput(STR16("Vocal Sidechain"), Steinberg::Vst::SpeakerArr::kMono, Steinberg::Vst::kAux, 0);
        addAudioOutput(STR16("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);

        for (int id = 0; id < kNumParams; ++id) {
            const auto& info = M::kParams[static_cast<std::size_t>(id)];
            switch (id) {
            case M::kBypass:
                parameters.addParameter(new ToggleParameter(info.name, static_cast<ParamID>(id), false,
                                                            Steinberg::Vst::ParameterInfo::kIsBypass));
                break;
            case M::kAnalyze:
                parameters.addParameter(new ToggleParameter(info.name, static_cast<ParamID>(id), false));
                break;
            case M::kMode:
                parameters.addParameter(makeList(id, [](int i) { return kModeNames[static_cast<std::size_t>(i)]; },
                                                 static_cast<int>(kModeNames.size())));
                break;
            case M::kPreset:
                parameters.addParameter(makeList(id, [](int i) { return M::kPresets[static_cast<std::size_t>(i)].name; },
                                                 M::kNumPresets));
                break;
            default: {
                Steinberg::Vst::String128 title{};
                toUtf16(info.name, title);
                parameters.addParameter(new sonora::ui::FormattedParameter(
                    title, static_cast<ParamID>(id), info.minValue, info.maxValue, info.defaultValue, formatFor(id)));
                break;
            }
            }
        }
        return Steinberg::kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == Steinberg::Vst::kSample32 || size == Steinberg::Vst::kSample64 ? Steinberg::kResultTrue
                                                                                   : Steinberg::kResultFalse;
    }

    Steinberg::uint32 PLUGIN_API getProcessContextRequirements() override {
        return Steinberg::Vst::IProcessContextRequirements::kNeedTransportState;
    }

    // Constant in every mode and in bypass (the engine delays its dry path).
    Steinberg::uint32 PLUGIN_API getLatencySamples() override {
        return static_cast<Steinberg::uint32>(std::max(0, engine_.latencySamples()));
    }
    // Latency plus a short filter/limiter-release decay.
    Steinberg::uint32 PLUGIN_API getTailSamples() override {
        return getLatencySamples() + static_cast<Steinberg::uint32>(std::ceil(0.05 * sampleRate_));
    }

    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs, int32 numOuts) override {
        using namespace Steinberg::Vst;
        if (!inputs || !outputs || numOuts != 1 || (numIns != 1 && numIns != 2)) return Steinberg::kResultFalse;
        if (inputs[0] != outputs[0] || (inputs[0] != SpeakerArr::kMono && inputs[0] != SpeakerArr::kStereo))
            return Steinberg::kResultFalse;
        if (numIns == 2 && inputs[1] != SpeakerArr::kMono && inputs[1] != SpeakerArr::kStereo)
            return Steinberg::kResultFalse;
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API activateBus(Steinberg::Vst::MediaType type, Steinberg::Vst::BusDirection dir, int32 index,
                                   Steinberg::TBool state) override {
        const auto result = SingleComponentEffect::activateBus(type, dir, index, state);
        if (result == Steinberg::kResultTrue && type == Steinberg::Vst::kAudio && dir == Steinberg::Vst::kInput && index == 1)
            sidechainActive_.store(state != 0, std::memory_order_relaxed);
        return result;
    }

    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate > 0.0 && std::isfinite(setup.sampleRate) ? setup.sampleRate : 44100.0;
        maxBlock_ = std::max<int32>(1, setup.maxSamplesPerBlock);
        syncEngineFromAtomics(false);
        engine_.prepare(sampleRate_, maxBlock_);
        meters_.publishSampleRate(sampleRate_);
        meters_.publish(engine_.meters());
        return SingleComponentEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(Steinberg::TBool state) override {
        if (state) {
            syncEngineFromAtomics(false);
            engine_.reset();
            wasPlaying_ = false;
        }
        return SingleComponentEffect::setActive(state);
    }

    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
        DenormalGuard denormals;
        restorePendingAnalysis();
        syncEngineFromAtomics(true);
        const int numEvents = collectEvents(data);

        if (meters_.consumeLoudnessResetRequest()) engine_.resetLoudness();
        if (const auto* ctx = data.processContext) {
            const bool playing = (ctx->state & Steinberg::Vst::ProcessContext::kPlaying) != 0;
            if (playing && !wasPlaying_) engine_.resetLoudness(); // integrated loudness per playback run
            wasPlaying_ = playing;
        }

        const bool haveAudio = data.numSamples > 0 && data.numInputs > 0 && data.numOutputs > 0 && data.inputs
                               && data.outputs;
        if (!haveAudio) {
            for (int i = 0; i < numEvents; ++i) applyEvent(events_[static_cast<std::size_t>(i)]);
            return Steinberg::kResultOk;
        }

        auto& inBus = data.inputs[0];
        auto& outBus = data.outputs[0];
        const int channels = std::clamp<int>(std::min(inBus.numChannels, outBus.numChannels), 0, 2);
        const Steinberg::Vst::AudioBusBuffers* sc = nullptr;
        if (data.numInputs > 1 && data.inputs[1].numChannels > 0 && sidechainActive_.load(std::memory_order_relaxed))
            sc = &data.inputs[1];
        if (data.symbolicSampleSize == Steinberg::Vst::kSample64) {
            processBuffers(inBus.channelBuffers64, outBus.channelBuffers64, outBus.numChannels, channels,
                           sc ? sc->channelBuffers64 : nullptr, sc ? sc->numChannels : 0, data.numSamples, numEvents);
        } else {
            processBuffers(inBus.channelBuffers32, outBus.channelBuffers32, outBus.numChannels, channels,
                           sc ? sc->channelBuffers32 : nullptr, sc ? sc->numChannels : 0, data.numSamples, numEvents);
        }
        outBus.silenceFlags = 0;
        storeAnalysisResult();
        return Steinberg::kResultOk;
    }

    // --- IEditController -------------------------------------------------------------------------

    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) override {
        const auto result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result == Steinberg::kResultOk && tag < static_cast<ParamID>(kNumParams))
            values_[tag].store(sanitizeNormalized(value, M::defaultNormalized(static_cast<int>(tag))),
                               std::memory_order_relaxed);
        return result;
    }

    tresult PLUGIN_API setState(Steinberg::IBStream* state) override { return readState(state); }
    tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override { return readState(state); }

    tresult PLUGIN_API getState(Steinberg::IBStream* state) override {
        if (!state) return Steinberg::kInvalidArgument;
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        if (!streamer.writeInt32(kStateVersion) || !streamer.writeInt32(kNumParams)) return Steinberg::kResultFalse;
        for (int id = 0; id < kNumParams; ++id) {
            const double v = id == M::kAnalyze ? 0.0 : values_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
            if (!streamer.writeDouble(v)) return Steinberg::kResultFalse;
        }
        if (!streamer.writeInt32(kAnalysisStateCount)) return Steinberg::kResultFalse;
        for (const auto& v : analysisState_)
            if (!streamer.writeDouble(v.load(std::memory_order_relaxed))) return Steinberg::kResultFalse;
        return Steinberg::kResultOk;
    }

    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override {
        if (name && Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor)) {
            auto* editor = new VSTGUI::VST3Editor(this, "view", "sonora.uidesc");
            editor->setDelegate(this);
            return editor;
        }
        return nullptr;
    }

    // --- VST3EditorDelegate ----------------------------------------------------------------------

    VSTGUI::CView* createCustomView(VSTGUI::UTF8StringPtr name, const VSTGUI::UIAttributes& attributes,
                                    const VSTGUI::IUIDescription* description, VSTGUI::VST3Editor* editor) override {
        return sonora::ui::createMasterView(name, attributes, description, editor, uiContext_);
    }

    // A momentary "Analyse starten" click whose 250 ms release timer has not fired yet would leave
    // the parameter at 1 once the editor's parameter listeners are gone; reset it while they exist.
    void willClose(VSTGUI::VST3Editor*) override {
        const auto id = static_cast<ParamID>(M::kAnalyze);
        if (getParamNormalized(id) < 0.5) return;
        beginEdit(id);
        setParamNormalized(id, 0.0);
        performEdit(id, 0.0);
        endEdit(id);
    }

private:
    // Analyse starten: rising edge is the trigger (see header comment).
    void analyzeValue(double normalized) {
        const bool on = normalized >= 0.5;
        if (on && !analyzeOn_) engine_.requestAnalysis();
        analyzeOn_ = on;
    }

    void applyValue(int id, double normalized) {
        const auto idx = static_cast<std::size_t>(id);
        if (id == M::kAnalyze) {
            analyzeValue(normalized);
        } else if (normalized != applied_[idx]) {
            engine_.setParameter(id, normalized);
        }
        applied_[idx] = normalized;
    }
    void applyEvent(const ParamEvent& e) { applyValue(e.id, e.normalized); }

    // allowTrigger = false outside process(): adopt the analyze value without starting an analysis.
    void syncEngineFromAtomics(bool allowTrigger) {
        if (analyzeBaseline_.exchange(false, std::memory_order_acq_rel)) allowTrigger = false;
        for (int id = 0; id < kNumParams; ++id) {
            const double v = values_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
            if (id == M::kAnalyze && !allowTrigger) {
                analyzeOn_ = v >= 0.5;
                applied_[static_cast<std::size_t>(id)] = v;
                continue;
            }
            applyValue(id, v);
        }
    }

    // Merges all queue points into events_ (sorted by offset, stable per parameter) and writes the
    // last value of every queue back to the atomics. Returns the number of events.
    int collectEvents(Steinberg::Vst::ProcessData& data) {
        auto* changes = data.inputParameterChanges;
        if (!changes) return 0;
        const int32 queues = changes->getParameterCount();
        if (queues <= 0) return 0;
        const int32 lastOffset = std::max<int32>(0, data.numSamples - 1);
        const int budget = std::max(1, kEventCapacity / static_cast<int>(queues));
        int count = 0;
        for (int32 q = 0; q < queues; ++q) {
            auto* queue = changes->getParameterData(q);
            if (!queue) continue;
            const ParamID id = queue->getParameterId();
            const int32 points = queue->getPointCount();
            if (id >= static_cast<ParamID>(kNumParams) || points <= 0) continue;
            // Keep every point if possible; otherwise thin out evenly but always keep the last one.
            const int32 take = std::min<int32>(points, budget);
            double last = -1.0;
            for (int32 k = 0; k < take; ++k) {
                const int32 index = take == points ? k : static_cast<int32>((static_cast<long long>(k + 1) * points) / take) - 1;
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint(index, offset, value) != Steinberg::kResultTrue || !std::isfinite(value)) continue;
                value = std::clamp(value, 0.0, 1.0);
                if (count < kEventCapacity) {
                    events_[static_cast<std::size_t>(count)] = {std::clamp(offset, 0, lastOffset), static_cast<int>(id), value};
                    order_[static_cast<std::size_t>(count)] = count;
                    ++count;
                }
                last = value;
            }
            if (last >= 0.0) values_[id].store(last, std::memory_order_relaxed);
        }
        sortEvents(count);
        return count;
    }

    void sortEvents(int count) {
        if (count < 2) return;
        auto* ev = events_.data();
        // Insertion order (order_) breaks ties so points of one queue stay in sequence.
        std::sort(order_.begin(), order_.begin() + count, [ev](int a, int b) {
            const auto oa = ev[a].offset, ob = ev[b].offset;
            return oa != ob ? oa < ob : a < b;
        });
        for (int i = 0; i < count; ++i) sorted_[static_cast<std::size_t>(i)] = ev[order_[static_cast<std::size_t>(i)]];
        std::copy(sorted_.begin(), sorted_.begin() + count, events_.begin());
    }

    template <typename Sample>
    void processBuffers(Sample** in, Sample** out, int32 numOut, int channels, Sample** sc, int32 scChannels,
                        int numSamples, int numEvents) {
        if (!out) {
            for (int i = 0; i < numEvents; ++i) applyEvent(events_[static_cast<std::size_t>(i)]);
            return;
        }
        bool ok = channels > 0 && in;
        for (int ch = 0; ok && ch < channels; ++ch) ok = in[ch] && out[ch];
        if (!ok) channels = 0;

        int scCh = sc ? std::min<int32>(scChannels, 2) : 0;
        for (int ch = 0; ch < scCh; ++ch)
            if (!sc[ch]) scCh = 0;

        if (channels == 0) {
            for (int i = 0; i < numEvents; ++i) applyEvent(events_[static_cast<std::size_t>(i)]);
        } else {
            // Split the block at automation offsets so every value change lands on its sample.
            int pos = 0, e = 0;
            while (pos < numSamples) {
                while (e < numEvents && events_[static_cast<std::size_t>(e)].offset <= pos)
                    applyEvent(events_[static_cast<std::size_t>(e++)]);
                const int end = e < numEvents ? std::min<int>(numSamples, events_[static_cast<std::size_t>(e)].offset) : numSamples;
                const int len = end - pos;
                std::array<const Sample*, 2> ip{};
                std::array<Sample*, 2> op{};
                std::array<const Sample*, 2> sp{};
                for (int ch = 0; ch < channels; ++ch) {
                    ip[static_cast<std::size_t>(ch)] = in[ch] + pos;
                    op[static_cast<std::size_t>(ch)] = out[ch] + pos;
                }
                for (int ch = 0; ch < scCh; ++ch) sp[static_cast<std::size_t>(ch)] = sc[ch] + pos;
                engine_.process<Sample>(ip.data(), op.data(), channels, len, scCh > 0 ? sp.data() : nullptr, scCh);
                meters_.publish(engine_.meters());
                pos = end;
            }
            while (e < numEvents) applyEvent(events_[static_cast<std::size_t>(e++)]);
        }
        for (int32 ch = channels; ch < numOut; ++ch)
            if (out[ch]) std::fill(out[ch], out[ch] + numSamples, Sample(0));
    }

    // Auto-EQ result persistence: readState() hands values to the audio thread via a flag; new
    // analysis results are copied back into analysisState_ whenever the engine's values change.
    void restorePendingAnalysis() {
        if (!analysisPending_.exchange(false, std::memory_order_acq_rel)) return;
        std::array<double, M::kBands> gains{};
        for (int b = 0; b < M::kBands; ++b) gains[static_cast<std::size_t>(b)] = analysisState_[static_cast<std::size_t>(b)].load(std::memory_order_relaxed);
        engine_.setAutoEq(gains.data(), analysisState_[M::kBands].load(std::memory_order_relaxed));
        rememberEngineAnalysis();
    }
    void rememberEngineAnalysis() {
        const auto& m = engine_.meters();
        for (int b = 0; b < M::kBands; ++b) lastEngineAnalysis_[static_cast<std::size_t>(b)] = m.autoEqDb[b];
        lastEngineAnalysis_[M::kBands] = m.autoCompRatio;
    }
    void storeAnalysisResult() {
        if (analysisPending_.load(std::memory_order_acquire)) return;
        const auto& m = engine_.meters();
        bool changed = m.autoCompRatio != lastEngineAnalysis_[M::kBands];
        for (int b = 0; b < M::kBands; ++b) changed = changed || m.autoEqDb[b] != lastEngineAnalysis_[static_cast<std::size_t>(b)];
        if (!changed) return;
        rememberEngineAnalysis();
        for (int i = 0; i < kAnalysisStateCount; ++i)
            analysisState_[static_cast<std::size_t>(i)].store(lastEngineAnalysis_[static_cast<std::size_t>(i)], std::memory_order_relaxed);
    }

    tresult readState(Steinberg::IBStream* state) {
        if (!state) return Steinberg::kInvalidArgument;
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        int32 version = 0;
        if (!streamer.readInt32(version) || version < 1) return Steinberg::kResultFalse;
        std::array<double, kNumParams> loaded{};
        for (int id = 0; id < kNumParams; ++id) loaded[static_cast<std::size_t>(id)] = M::defaultNormalized(id);
        std::array<double, kAnalysisStateCount> analysis{};
        analysis[M::kBands] = 1.0;

        if (version <= 4) {
            // v0.1: fixed parameter count per version, no count field.
            static constexpr int kLegacyCounts[5] = {0, 6, 16, 17, 18};
            const int count = kLegacyCounts[version];
            int read = 0;
            for (; read < count; ++read) {
                double v = 0.0;
                if (!streamer.readDouble(v)) break; // truncated: keep defaults for the rest
                if (std::isfinite(v)) loaded[static_cast<std::size_t>(read)] = std::clamp(v, 0.0, 1.0);
            }
            migrateLegacyState(loaded, read);
        } else {
            int32 count = 0;
            if (streamer.readInt32(count)) {
                int32 i = 0;
                for (; i < count; ++i) {
                    double v = 0.0;
                    if (!streamer.readDouble(v)) break;
                    if (i < kNumParams && std::isfinite(v)) loaded[static_cast<std::size_t>(i)] = std::clamp(v, 0.0, 1.0);
                }
                int32 extra = 0;
                if (i == count && streamer.readInt32(extra) && extra >= kAnalysisStateCount) {
                    std::array<double, kAnalysisStateCount> a{};
                    bool complete = true;
                    for (auto& v : a) complete = complete && streamer.readDouble(v) && std::isfinite(v);
                    if (complete) analysis = a;
                }
            }
        }
        loaded[M::kAnalyze] = 0.0;

        analyzeBaseline_.store(true, std::memory_order_release);
        for (int id = 0; id < kNumParams; ++id)
            setParamNormalized(static_cast<ParamID>(id), loaded[static_cast<std::size_t>(id)]);
        for (int i = 0; i < kAnalysisStateCount; ++i)
            analysisState_[static_cast<std::size_t>(i)].store(analysis[static_cast<std::size_t>(i)], std::memory_order_relaxed);
        analysisPending_.store(true, std::memory_order_release);
        return Steinberg::kResultOk;
    }

    std::array<std::atomic<double>, kNumParams> values_{};
    std::array<std::atomic<double>, kAnalysisStateCount> analysisState_{};
    std::atomic<bool> analysisPending_{false};
    std::atomic<bool> analyzeBaseline_{false};
    std::atomic<bool> sidechainActive_{false};

    // Audio thread only.
    sonora::dsp::MasterEngine engine_;
    std::array<double, kNumParams> applied_{};
    std::array<double, kAnalysisStateCount> lastEngineAnalysis_{{0.0, 0.0, 0.0, 0.0, 0.0, 1.0}};
    bool analyzeOn_ = false;
    bool wasPlaying_ = false;
    std::vector<ParamEvent> events_;
    std::vector<ParamEvent> sorted_;
    std::vector<int> order_;
    double sampleRate_ = 44100.0;
    int32 maxBlock_ = 1024;

    sonora::ui::MasterMeterBridge meters_;
    sonora::ui::MasterUiContext uiContext_;
};

BEGIN_FACTORY_DEF("SONORA Audio", "https://github.com/simon-of-course/sonora-mastering", "")

DEF_CLASS2(INLINE_UID(0x7B628C23, 0xBB614E25, 0x98B04D14, 0x23AC1F91),
           Steinberg::PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           "SONORA Master",
           0,
           "Fx|Mastering",
           SONORA_VERSION_STRING,
           kVstVersionString,
           SonoraMaster::createInstance)

END_FACTORY
