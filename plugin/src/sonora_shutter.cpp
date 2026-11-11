// SONORA Shutter — VST3 glue (SingleComponentEffect + VST3Editor) around
// sonora::dsp::shutter::ShutterEngine (plugin/src/dsp/shutter_engine.h).
//
// Threading: parameter values live in atomics written by setParamNormalized (UI/host thread) and
// read once per block by process(). Host automation arrives as IParamValueQueue points, which are
// merged, sorted by sample offset and handed to the engine as ParamEvents, so automation is
// sample-accurate without splitting blocks. The last point of every queue is written back to the
// atomics so the next block starts from the automated value.
//
// State v2: int32 version (2), int32 parameter count, then one double (normalized) per parameter.
// The count makes the format extensible: Bypass (ID 13) was appended later, older v2 states
// (count 13) load with Bypass off.
// State v1 (v0.1): int32 version (1), 7 doubles; Notenwert is migrated from 9 to 12 list entries.

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

#include "dsp/shutter_engine.h"
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

namespace sh = sonora::dsp::shutter;
using Steinberg::int32;
using Steinberg::tresult;
using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;

constexpr int kNumParams = sh::kNumParams;
constexpr int kNumParamsV1 = 7;
constexpr int32 kStateVersion = 2;
constexpr int kEventCapacity = 8192;

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

template <std::size_t N>
Steinberg::Vst::StringListParameter* makeList(int id, const std::array<const char*, N>& names) {
    Steinberg::Vst::String128 title{};
    toUtf16(sh::kParamInfo[static_cast<std::size_t>(id)].name, title);
    auto* p = new Steinberg::Vst::StringListParameter(title, static_cast<ParamID>(id));
    for (const char* name : names) {
        Steinberg::Vst::String128 s{};
        toUtf16(name, s);
        p->appendString(s);
    }
    const double def = sh::defaultNormalized(id);
    p->getInfo().defaultNormalizedValue = def;
    p->setNormalized(def);
    return p;
}

sonora::ui::ParamFormat formatFor(int id) {
    using sonora::ui::ParamFormat;
    switch (id) {
    case sh::kRate: return ParamFormat::hz(1);
    case sh::kSmoothing: return ParamFormat::ms(1);
    default: return ParamFormat::percent(0); // Tiefe, Mix, Pulsbreite, Swing, Versatz
    }
}

} // namespace

class SonoraShutter final : public Steinberg::Vst::SingleComponentEffect,
                            public VSTGUI::VST3EditorDelegate {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new SonoraShutter());
    }

    SonoraShutter() {
        for (int id = 0; id < kNumParams; ++id) values_[static_cast<std::size_t>(id)].store(sh::defaultNormalized(id));
        events_.resize(kEventCapacity);
        sorted_.resize(kEventCapacity);
        order_.resize(kEventCapacity);
        uiContext_.meters = &meters_;
        uiContext_.getParam = [this](std::uint32_t id) { return getParamNormalized(id); };
        uiContext_.gateAt = [this](double pos, int channel) {
            std::array<double, kNumParams> n{};
            for (int id = 0; id < kNumParams; ++id) n[static_cast<std::size_t>(id)] = getParamNormalized(static_cast<ParamID>(id));
            return sh::patternGain(n.data(), pos, channel, lastTempo_.load(std::memory_order_relaxed),
                                   busChannels_.load(std::memory_order_relaxed) >= 2);
        };
    }

    // --- IComponent / IAudioProcessor ------------------------------------------------------------

    tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != Steinberg::kResultOk) return result;
        addAudioInput(STR16("Vocal In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioOutput(STR16("Vocal Out"), Steinberg::Vst::SpeakerArr::kStereo);

        for (int id = 0; id < kNumParams; ++id) {
            const auto& info = sh::kParamInfo[static_cast<std::size_t>(id)];
            switch (id) {
            case sh::kEnable:
            case sh::kSync:
                parameters.addParameter(new ToggleParameter(info.name, static_cast<ParamID>(id), info.defaultPlain >= 0.5));
                break;
            case sh::kBypass:
                parameters.addParameter(new ToggleParameter(info.name, static_cast<ParamID>(id), false,
                                                            Steinberg::Vst::ParameterInfo::kIsBypass));
                break;
            case sh::kDivision: parameters.addParameter(makeList(id, sh::kDivisionNames)); break;
            case sh::kShape: parameters.addParameter(makeList(id, sh::kShapeNames)); break;
            case sh::kStereo: parameters.addParameter(makeList(id, sh::kStereoNames)); break;
            case sh::kGateTone: parameters.addParameter(makeList(id, sh::kGateToneNames)); break;
            default: {
                Steinberg::Vst::String128 title{};
                toUtf16(info.name, title);
                parameters.addParameter(new sonora::ui::FormattedParameter(
                    title, static_cast<ParamID>(id), info.minPlain, info.maxPlain, info.defaultPlain, formatFor(id)));
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
        using R = Steinberg::Vst::IProcessContextRequirements;
        return R::kNeedTempo | R::kNeedProjectTimeMusic | R::kNeedTransportState;
    }

    Steinberg::uint32 PLUGIN_API getLatencySamples() override { return 0; }
    Steinberg::uint32 PLUGIN_API getTailSamples() override { return 0; }

    tresult PLUGIN_API setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, int32 numIns,
                                          Steinberg::Vst::SpeakerArrangement* outputs, int32 numOuts) override {
        using namespace Steinberg::Vst;
        if (numIns != 1 || numOuts != 1 || !inputs || !outputs || inputs[0] != outputs[0]) return Steinberg::kResultFalse;
        if (inputs[0] != SpeakerArr::kMono && inputs[0] != SpeakerArr::kStereo) return Steinberg::kResultFalse;
        return SingleComponentEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate > 0.0 && std::isfinite(setup.sampleRate) ? setup.sampleRate : 44100.0;
        maxBlock_ = std::max<int32>(1, setup.maxSamplesPerBlock);
        syncEngineFromAtomics();
        engine_.prepare(sampleRate_, maxBlock_);
        return SingleComponentEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(Steinberg::TBool state) override {
        if (state) {
            syncEngineFromAtomics();
            engine_.reset();
        }
        meters_.publish(1.0f, 1.0f, 0.0f, false, false);
        return SingleComponentEffect::setActive(state);
    }

    tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
        DenormalGuard denormals;
        syncEngineFromAtomics();
        const int numEvents = collectEvents(data);

        const bool haveAudio = data.numSamples > 0 && data.numInputs > 0 && data.numOutputs > 0 && data.inputs
                               && data.outputs;
        if (!haveAudio) {
            for (int i = 0; i < numEvents; ++i) engine_.setParameter(events_[static_cast<std::size_t>(i)].id, events_[static_cast<std::size_t>(i)].normalized);
            return Steinberg::kResultOk;
        }

        sh::TransportInfo transport;
        if (const auto* ctx = data.processContext) {
            using PC = Steinberg::Vst::ProcessContext;
            transport.hasTempo = (ctx->state & PC::kTempoValid) != 0;
            transport.tempo = ctx->tempo;
            transport.hasPosition = (ctx->state & PC::kProjectTimeMusicValid) != 0;
            transport.ppqPosition = ctx->projectTimeMusic;
            transport.isPlaying = (ctx->state & PC::kPlaying) != 0;
        } else {
            transport.hasTempo = false;
            transport.hasPosition = false;
            transport.isPlaying = true;
        }

        auto& inBus = data.inputs[0];
        auto& outBus = data.outputs[0];
        const int channels = std::clamp<int>(std::min(inBus.numChannels, outBus.numChannels), 0, 2);
        const int n = data.numSamples;
        if (data.symbolicSampleSize == Steinberg::Vst::kSample64) {
            processBuffers(inBus.channelBuffers64, outBus.channelBuffers64, inBus.numChannels, outBus.numChannels,
                           channels, n, transport, numEvents);
        } else {
            processBuffers(inBus.channelBuffers32, outBus.channelBuffers32, inBus.numChannels, outBus.numChannels,
                           channels, n, transport, numEvents);
        }
        outBus.silenceFlags = 0;

        if (transport.hasTempo && std::isfinite(transport.tempo) && transport.tempo > 0.0)
            lastTempo_.store(transport.tempo, std::memory_order_relaxed);
        if (channels > 0) busChannels_.store(channels, std::memory_order_relaxed);
        const auto& m = engine_.meters();
        meters_.publish(m.gainL, channels >= 2 ? m.gainR : m.gainL, m.scopePhase, m.active > 0.0f,
                        transport.isPlaying);
        return Steinberg::kResultOk;
    }

    // --- IEditController -------------------------------------------------------------------------

    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) override {
        const auto result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result == Steinberg::kResultOk && tag < static_cast<ParamID>(kNumParams))
            values_[tag].store(sanitizeNormalized(value, sh::defaultNormalized(static_cast<int>(tag))),
                               std::memory_order_relaxed);
        return result;
    }

    tresult PLUGIN_API setState(Steinberg::IBStream* state) override { return readState(state); }
    tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override { return readState(state); }

    tresult PLUGIN_API getState(Steinberg::IBStream* state) override {
        if (!state) return Steinberg::kInvalidArgument;
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        if (!streamer.writeInt32(kStateVersion) || !streamer.writeInt32(kNumParams)) return Steinberg::kResultFalse;
        for (const auto& v : values_)
            if (!streamer.writeDouble(v.load(std::memory_order_relaxed))) return Steinberg::kResultFalse;
        return Steinberg::kResultOk;
    }

    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override {
        if (name && Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor)) {
            auto* editor = new VSTGUI::VST3Editor(this, "view", "shutter.uidesc");
            editor->setDelegate(this);
            return editor;
        }
        return nullptr;
    }

    // --- VST3EditorDelegate ----------------------------------------------------------------------

    VSTGUI::CView* createCustomView(VSTGUI::UTF8StringPtr name, const VSTGUI::UIAttributes& attributes,
                                    const VSTGUI::IUIDescription* description, VSTGUI::VST3Editor* editor) override {
        return sonora::ui::createShutterView(name, attributes, description, editor, uiContext_);
    }

private:
    void syncEngineFromAtomics() {
        for (int id = 0; id < kNumParams; ++id) {
            const double v = values_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
            if (v != engine_.getParameter(id)) engine_.setParameter(id, v);
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
            const auto oa = ev[a].sampleOffset, ob = ev[b].sampleOffset;
            return oa != ob ? oa < ob : a < b;
        });
        for (int i = 0; i < count; ++i) sorted_[static_cast<std::size_t>(i)] = ev[order_[static_cast<std::size_t>(i)]];
        std::copy(sorted_.begin(), sorted_.begin() + count, events_.begin());
    }

    template <typename Sample>
    void processBuffers(Sample** in, Sample** out, int32 numIn, int32 numOut, int channels, int numSamples,
                        const sh::TransportInfo& transport, int numEvents) {
        if (!out) return;
        if (channels > 0 && in) {
            bool ok = true;
            for (int ch = 0; ch < channels; ++ch) ok = ok && in[ch] && out[ch];
            if (ok) {
                engine_.process<Sample>(in, out, channels, numSamples, transport, events_.data(), numEvents);
            } else {
                channels = 0;
            }
        }
        if (channels == 0)
            for (int i = 0; i < numEvents; ++i) engine_.setParameter(events_[static_cast<std::size_t>(i)].id, events_[static_cast<std::size_t>(i)].normalized);
        (void)numIn;
        for (int32 ch = channels; ch < numOut; ++ch)
            if (out[ch]) std::fill(out[ch], out[ch] + numSamples, Sample(0));
    }

    tresult readState(Steinberg::IBStream* state) {
        if (!state) return Steinberg::kInvalidArgument;
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        int32 version = 0;
        if (!streamer.readInt32(version) || version < 1) return Steinberg::kResultFalse;
        std::array<double, kNumParams> loaded{};
        for (int id = 0; id < kNumParams; ++id) loaded[static_cast<std::size_t>(id)] = sh::defaultNormalized(id);
        if (version == 1) {
            for (int id = 0; id < kNumParamsV1; ++id) {
                double v = 0.0;
                if (!streamer.readDouble(v)) break; // truncated: keep defaults for the rest
                if (std::isfinite(v)) loaded[static_cast<std::size_t>(id)] = sh::migrateParamV1ToV2(id, v);
            }
        } else {
            int32 count = 0;
            if (streamer.readInt32(count)) {
                for (int32 i = 0; i < count; ++i) {
                    double v = 0.0;
                    if (!streamer.readDouble(v)) break;
                    if (i < kNumParams && std::isfinite(v)) loaded[static_cast<std::size_t>(i)] = std::clamp(v, 0.0, 1.0);
                }
            }
        }
        for (int id = 0; id < kNumParams; ++id)
            setParamNormalized(static_cast<ParamID>(id), loaded[static_cast<std::size_t>(id)]);
        return Steinberg::kResultOk;
    }

    std::array<std::atomic<double>, kNumParams> values_{};
    sh::ShutterEngine engine_;
    std::vector<sh::ParamEvent> events_;
    std::vector<sh::ParamEvent> sorted_;
    std::vector<int> order_;
    double sampleRate_ = 44100.0;
    int32 maxBlock_ = 1024;

    sonora::ui::ShutterMeterBridge meters_;
    sonora::ui::ShutterUiContext uiContext_;
    std::atomic<double> lastTempo_{120.0};
    std::atomic<int> busChannels_{2};
};

BEGIN_FACTORY_DEF("SONORA Audio", "https://github.com/simon-of-course/sonora-mastering", "")

DEF_CLASS2(INLINE_UID(0x9C41C85E, 0xD91F4D72, 0xA6B83F27, 0x68120A55),
           Steinberg::PClassInfo::kManyInstances,
           kVstAudioEffectClass,
           "SONORA Shutter",
           0,
           "Fx|Effect",
           SONORA_VERSION_STRING,
           kVstVersionString,
           SonoraShutter::createInstance)

END_FACTORY
