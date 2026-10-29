#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstguieditor.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "base/source/fstreamer.h"
#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/uidescription/uiattributes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

enum ParameterId : Steinberg::Vst::ParamID {
    kEnableId,
    kSyncId,
    kDivisionId,
    kRateId,
    kDepthId,
    kMixId,
    kSmoothingId,
    kParameterCount
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultTempo = 120.0;
constexpr std::array<double, 9> kQuarterNotesPerCycle{{
    4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0
}};

double clampNormalized(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

class ShutterCard final : public VSTGUI::CView {
public:
    explicit ShutterCard(const VSTGUI::CRect& size) : CView(size) {
        setMouseEnabled(false);
    }

    void draw(VSTGUI::CDrawContext* context) override {
        context->setDrawMode(VSTGUI::kAntiAliasing);
        context->setFillColor(VSTGUI::CColor{255, 255, 255, 255});
        context->setFrameColor(VSTGUI::CColor{232, 229, 238, 255});
        auto* path = context->createRoundRectGraphicsPath(getViewSize(), 10.0);
        if (path) {
            context->drawGraphicsPath(path, VSTGUI::CDrawContext::kPathFilled);
            context->drawGraphicsPath(path, VSTGUI::CDrawContext::kPathStroked);
            path->forget();
        }
        context->setDrawMode(VSTGUI::kAliasing);
        setDirty(false);
    }

    CLASS_METHODS(ShutterCard, CView)
};

} // namespace

class SonoraShutter final : public Steinberg::Vst::SingleComponentEffect,
                            public VSTGUI::VST3EditorDelegate,
                            public VSTGUI::IControlListener {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new SonoraShutter());
    }

    SonoraShutter() {
        parameterValues_[kEnableId].store(0.0);
        parameterValues_[kSyncId].store(1.0);
        parameterValues_[kDivisionId].store(4.0 / 8.0);
        parameterValues_[kRateId].store(0.35);
        parameterValues_[kDepthId].store(0.85);
        parameterValues_[kMixId].store(1.0);
        parameterValues_[kSmoothingId].store(0.2);
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != Steinberg::kResultOk) {
            return result;
        }
        addAudioInput(STR16("Vocal In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioOutput(STR16("Vocal Out"), Steinberg::Vst::SpeakerArr::kStereo);

        parameters.addParameter(
            STR16("Shutter aktiv"), nullptr, Steinberg::Vst::kStepCountToggle,
            0.0, Steinberg::Vst::ParameterInfo::kCanAutomate, kEnableId);
        parameters.addParameter(
            STR16("Tempo Sync"), nullptr, Steinberg::Vst::kStepCountToggle,
            1.0, Steinberg::Vst::ParameterInfo::kCanAutomate, kSyncId);

        auto* division = new Steinberg::Vst::StringListParameter(
            STR16("Notenwert"), kDivisionId);
        division->appendString(STR16("1/1"));
        division->appendString(STR16("1/2"));
        division->appendString(STR16("1/4"));
        division->appendString(STR16("1/8"));
        division->appendString(STR16("1/16"));
        division->appendString(STR16("1/32"));
        division->appendString(STR16("1/8 T"));
        division->appendString(STR16("1/16 T"));
        division->appendString(STR16("1/32 T"));
        division->getInfo().defaultNormalizedValue = 0.5;
        division->setNormalized(0.5);
        parameters.addParameter(division);

        addRangeParameter("Rate", "Hz", kRateId, 0.5, 20.0, 4.0);
        addRangeParameter("Tiefe", "%", kDepthId, 0.0, 100.0, 85.0);
        addRangeParameter("Mix", "%", kMixId, 0.0, 100.0, 100.0);
        addRangeParameter("Glattung", "ms", kSmoothingId, 0.0, 12.0, 2.0);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
        return size == Steinberg::Vst::kSample32 || size == Steinberg::Vst::kSample64
            ? Steinberg::kResultTrue
            : Steinberg::kResultFalse;
    }

    Steinberg::uint32 PLUGIN_API getProcessContextRequirements() override {
        return Steinberg::Vst::IProcessContextRequirements::kNeedTempo
            | Steinberg::Vst::IProcessContextRequirements::kNeedProjectTimeMusic
            | Steinberg::Vst::IProcessContextRequirements::kNeedTransportState;
    }

    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override {
        if (!state) {
            gainEnvelope_ = 1.0;
            freePhase_ = 0.0;
        }
        return SingleComponentEffect::setActive(state);
    }

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate > 0.0 ? setup.sampleRate : 44100.0;
        return SingleComponentEffect::setupProcessing(setup);
    }

    Steinberg::tresult PLUGIN_API setParamNormalized(
        Steinberg::Vst::ParamID tag,
        Steinberg::Vst::ParamValue value) override {
        const auto result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result == Steinberg::kResultOk && tag < kParameterCount) {
            parameterValues_[tag].store(clampNormalized(value), std::memory_order_relaxed);
        }
        return result;
    }

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override {
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        Steinberg::int32 version = 0;
        if (!streamer.readInt32(version) || version != 1) {
            return Steinberg::kResultFalse;
        }
        for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
            double value = 0.0;
            if (!streamer.readDouble(value)) {
                return Steinberg::kResultFalse;
            }
            setParamNormalized(id, value);
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override {
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        if (!streamer.writeInt32(1)) {
            return Steinberg::kResultFalse;
        }
        for (const auto& value : parameterValues_) {
            if (!streamer.writeDouble(value.load(std::memory_order_relaxed))) {
                return Steinberg::kResultFalse;
            }
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override {
        return setState(state);
    }

    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override {
        if (name && Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor)) {
            auto* editor = new VSTGUI::VST3Editor(this, "view", "shutter.uidesc");
            editor->setDelegate(this);
            return editor;
        }
        return nullptr;
    }

    VSTGUI::CView* createCustomView(
        VSTGUI::UTF8StringPtr name,
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription*,
        VSTGUI::VST3Editor*) override {
        if (std::strcmp(name, "SonoraShutterCard") != 0) {
            return nullptr;
        }
        VSTGUI::CPoint size;
        if (!attributes.getPointAttribute("size", size)) {
            return nullptr;
        }
        return new ShutterCard({0.0, 0.0, size.x, size.y});
    }

    VSTGUI::CView* verifyView(
        VSTGUI::CView* view,
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription* description,
        VSTGUI::VST3Editor* editor) override {
        if (auto* control = dynamic_cast<VSTGUI::CControl*>(view);
            control && control->getTag() >= 0
            && control->getTag() < static_cast<Steinberg::Vst::ParamID>(kParameterCount)) {
            control->registerControlListener(this);
            controls_[static_cast<std::size_t>(control->getTag())] = control;
        }
        editor_ = editor;
        return VSTGUI::VST3EditorDelegate::verifyView(
            view, attributes, description, editor);
    }

    void willClose(VSTGUI::VST3Editor* editor) override {
        if (editor_ == editor) {
            for (auto*& control : controls_) {
                if (control) {
                    control->unregisterControlListener(this);
                    control = nullptr;
                }
            }
            editor_ = nullptr;
        }
    }

    void valueChanged(VSTGUI::CControl* control) override {
        if (!control || !editor_) {
            return;
        }
        const auto tag = static_cast<Steinberg::Vst::ParamID>(control->getTag());
        if (tag >= kParameterCount) {
            return;
        }
        beginEdit(tag);
        performEdit(tag, control->getValueNormalized());
        endEdit(tag);
    }

    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override {
        if (data.numInputs <= 0 || data.numOutputs <= 0 || data.numSamples <= 0) {
            return Steinberg::kResultOk;
        }

        std::array<double, kParameterCount> values{};
        std::array<ParameterTimeline, kParameterCount> timelines{};
        for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
            values[id] = parameterValues_[id].load(std::memory_order_relaxed);
        }
        if (data.inputParameterChanges) {
            const auto count = data.inputParameterChanges->getParameterCount();
            for (Steinberg::int32 index = 0; index < count; ++index) {
                auto* queue = data.inputParameterChanges->getParameterData(index);
                if (!queue) {
                    continue;
                }
                const auto id = queue->getParameterId();
                if (id >= kParameterCount || queue->getPointCount() <= 0) {
                    continue;
                }
                auto& timeline = timelines[id];
                timeline.queue = queue;
                timeline.nextPoint = 0;
                readNextPoint(timeline);
            }
        }

        const auto channels = std::min(data.inputs[0].numChannels, data.outputs[0].numChannels);
        const auto* processContext = data.processContext;
        const bool hasTempo = processContext
            && (processContext->state & Steinberg::Vst::ProcessContext::kTempoValid)
            && std::isfinite(processContext->tempo)
            && processContext->tempo > 0.0;
        const double tempo = hasTempo ? processContext->tempo : kDefaultTempo;
        const bool hasMusicPosition = processContext
            && (processContext->state
                & Steinberg::Vst::ProcessContext::kProjectTimeMusicValid)
            && std::isfinite(processContext->projectTimeMusic);
        const double startQuarterNotes = hasMusicPosition
            ? processContext->projectTimeMusic
            : 0.0;
        const bool transportPlaying = !processContext
            || (processContext->state & Steinberg::Vst::ProcessContext::kPlaying);
        const double samplesPerQuarter = sampleRate_ > 0.0
            ? sampleRate_ * 60.0 / tempo
            : 0.0;
        const auto makeSmoothingCoefficient = [this](double normalized) {
            const double seconds = normalized * 0.012;
            return seconds > 0.0 && sampleRate_ > 0.0
                ? std::exp(-1.0 / (seconds * sampleRate_))
                : 0.0;
        };
        double freeRateHz = 0.5 + values[kRateId] * 19.5;
        double smoothingCoefficient = makeSmoothingCoefficient(values[kSmoothingId]);

        for (Steinberg::int32 frame = 0; frame < data.numSamples; ++frame) {
            for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
                auto& timeline = timelines[id];
                while (timeline.hasPoint && timeline.sampleOffset <= frame) {
                    values[id] = timeline.value;
                    if (id == kRateId) {
                        freeRateHz = 0.5 + values[kRateId] * 19.5;
                    } else if (id == kSmoothingId) {
                        smoothingCoefficient =
                            makeSmoothingCoefficient(values[kSmoothingId]);
                    }
                    readNextPoint(timeline);
                }
            }

            const auto divisionIndex = static_cast<std::size_t>(
                std::clamp(std::lround(values[kDivisionId] * 8.0), 0L, 8L));
            const double cycleQuarterNotes = kQuarterNotesPerCycle[divisionIndex];
            double phase = freePhase_;
            if (values[kSyncId] >= 0.5 && hasMusicPosition) {
                const double quarterPosition = startQuarterNotes
                    + static_cast<double>(frame) / samplesPerQuarter;
                phase = positiveFraction(quarterPosition / cycleQuarterNotes);
            } else if (values[kSyncId] >= 0.5) {
                phase = freePhase_;
            }

            const bool active = transportPlaying && values[kEnableId] >= 0.5;
            const double phaseRate = values[kSyncId] >= 0.5
                ? (1.0 / cycleQuarterNotes) * tempo / (60.0 * sampleRate_)
                : freeRateHz / sampleRate_;
            if (!hasMusicPosition || values[kSyncId] < 0.5) {
                if (transportPlaying) {
                    freePhase_ = positiveFraction(freePhase_ + phaseRate);
                }
            } else {
                freePhase_ = phase;
            }

            const double depth = values[kDepthId];
            const double dryGate = phase < 0.5 ? 1.0 : 1.0 - depth;
            const double targetGain = active ? dryGate : 1.0;
            gainEnvelope_ = targetGain + (gainEnvelope_ - targetGain)
                * smoothingCoefficient;
            const double mix = values[kMixId];
            const double wetGain = 1.0 + (gainEnvelope_ - 1.0) * mix;

            if (data.symbolicSampleSize == Steinberg::Vst::kSample32) {
                for (Steinberg::int32 channel = 0; channel < channels; ++channel) {
                    const float input = data.inputs[0].channelBuffers32[channel][frame];
                    data.outputs[0].channelBuffers32[channel][frame] =
                        static_cast<float>(input * wetGain);
                }
            } else if (data.symbolicSampleSize == Steinberg::Vst::kSample64) {
                for (Steinberg::int32 channel = 0; channel < channels; ++channel) {
                    const double input = data.inputs[0].channelBuffers64[channel][frame];
                    data.outputs[0].channelBuffers64[channel][frame] = input * wetGain;
                }
            }
        }

        for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
            parameterValues_[id].store(values[id], std::memory_order_relaxed);
        }
        return Steinberg::kResultOk;
    }

private:
    struct ParameterTimeline {
        Steinberg::Vst::IParamValueQueue* queue = nullptr;
        Steinberg::int32 nextPoint = 0;
        Steinberg::int32 sampleOffset = 0;
        double value = 0.0;
        bool hasPoint = false;
    };

    static double positiveFraction(double value) {
        const double fraction = value - std::floor(value);
        return fraction < 0.0 ? fraction + 1.0 : fraction;
    }

    static void readNextPoint(ParameterTimeline& timeline) {
        timeline.hasPoint = false;
        if (!timeline.queue || timeline.nextPoint >= timeline.queue->getPointCount()) {
            return;
        }
        Steinberg::Vst::ParamValue value = 0.0;
        Steinberg::int32 sampleOffset = 0;
        if (timeline.queue->getPoint(timeline.nextPoint, sampleOffset, value)
            == Steinberg::kResultTrue) {
            timeline.sampleOffset = sampleOffset;
            timeline.value = clampNormalized(value);
            timeline.hasPoint = true;
        }
        ++timeline.nextPoint;
    }

    void addRangeParameter(const char* title, const char* units,
                           Steinberg::Vst::ParamID id,
                           double minimum, double maximum, double defaultValue) {
        Steinberg::Vst::String128 parameterTitle{};
        Steinberg::Vst::String128 parameterUnits{};
        Steinberg::UString(
            parameterTitle,
            sizeof(parameterTitle) / sizeof(Steinberg::Vst::TChar)).fromAscii(title);
        Steinberg::UString(
            parameterUnits,
            sizeof(parameterUnits) / sizeof(Steinberg::Vst::TChar)).fromAscii(units);
        parameters.addParameter(new Steinberg::Vst::RangeParameter(
            parameterTitle, id, parameterUnits, minimum, maximum, defaultValue));
        parameterValues_[id].store((defaultValue - minimum) / (maximum - minimum));
    }

    std::array<std::atomic<double>, kParameterCount> parameterValues_{};
    double sampleRate_ = 44100.0;
    double freePhase_ = 0.0;
    double gainEnvelope_ = 1.0;
    VSTGUI::VST3Editor* editor_ = nullptr;
    std::array<VSTGUI::CControl*, kParameterCount> controls_{};
};

BEGIN_FACTORY_DEF("SONORA Audio", "https://github.com/simon-of-course/sonora-mastering", "")

DEF_CLASS2(
    INLINE_UID(0x9C41C85E, 0xD91F4D72, 0xA6B83F27, 0x68120A55),
    Steinberg::PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    "SONORA Shutter",
    0,
    "Fx|Effect",
    "0.1.0",
    kVstVersionString,
    SonoraShutter::createInstance)

END_FACTORY
