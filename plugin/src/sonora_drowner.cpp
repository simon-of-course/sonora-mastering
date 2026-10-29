#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstguieditor.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "base/source/fstreamer.h"
#include "vstgui/lib/controls/cknob.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/uidescription/uiattributes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

enum ParameterId : Steinberg::Vst::ParamID {
    kDepthId,
    kReturnTimeId,
    kParameterCount
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kMinimumCutoffHz = 520.0;
constexpr double kMaximumCutoffNyquistRatio = 0.48;
constexpr Steinberg::int32 kCoefficientUpdateInterval = 8;

double clampNormalized(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

struct LowPassCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct FilterState {
    double z1 = 0.0;
    double z2 = 0.0;
};

LowPassCoefficients makeLowPass(double cutoffHz, double sampleRate) {
    const double safeRate = std::max(sampleRate, 1.0);
    const double cutoff = std::clamp(cutoffHz, 1.0, safeRate * 0.49);
    const double omega = 2.0 * kPi * cutoff / safeRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) * std::sqrt(0.5);
    const double inverseA0 = 1.0 / (1.0 + alpha);
    const double oneMinusCosine = 1.0 - cosine;
    return {
        oneMinusCosine * 0.5 * inverseA0,
        oneMinusCosine * inverseA0,
        oneMinusCosine * 0.5 * inverseA0,
        -2.0 * cosine * inverseA0,
        (1.0 - alpha) * inverseA0
    };
}

template <typename Sample>
Sample processLowPass(Sample input, const LowPassCoefficients& coefficients,
                      FilterState& state) {
    const double output = coefficients.b0 * static_cast<double>(input) + state.z1;
    state.z1 = coefficients.b1 * static_cast<double>(input)
        - coefficients.a1 * output + state.z2;
    state.z2 = coefficients.b2 * static_cast<double>(input)
        - coefficients.a2 * output;
    return static_cast<Sample>(output);
}

} // namespace

class SonoraDrowner final : public Steinberg::Vst::SingleComponentEffect,
                            public VSTGUI::VST3EditorDelegate,
                            public VSTGUI::IControlListener {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new SonoraDrowner());
    }

    SonoraDrowner() {
        parameterValues_[kDepthId].store(0.0);
        parameterValues_[kReturnTimeId].store((2.0 - 0.2) / (6.0 - 0.2));
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != Steinberg::kResultOk) {
            return result;
        }
        addAudioInput(STR16("Vocal In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioOutput(STR16("Vocal Out"), Steinberg::Vst::SpeakerArr::kStereo);
        addRangeParameter("Unterwasser-Anteil", "%", kDepthId, 0.0, 100.0, 0.0);
        addRangeParameter("Auftauchzeit", "s", kReturnTimeId, 0.2, 6.0, 2.0);
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
        return size == Steinberg::Vst::kSample32 || size == Steinberg::Vst::kSample64
            ? Steinberg::kResultTrue
            : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate > 0.0 ? setup.sampleRate : 44100.0;
        const double cutoff = cutoffForDepth(smoothedDepth_, sampleRate_);
        for (auto& channel : coefficients_) {
            channel = makeLowPass(cutoff, sampleRate_);
        }
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
            if (!streamer.readDouble(value) || !std::isfinite(value)) {
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
            auto* editor = new VSTGUI::VST3Editor(this, "view", "drowner.uidesc");
            editor->setDelegate(this);
            return editor;
        }
        return nullptr;
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
                if (!queue || queue->getPointCount() <= 0) {
                    continue;
                }
                const auto id = queue->getParameterId();
                if (id >= kParameterCount) {
                    continue;
                }
                auto& timeline = timelines[id];
                timeline.queue = queue;
                readNextPoint(timeline);
            }
        }

        const auto channelCount = std::min(
            data.inputs[0].numChannels, data.outputs[0].numChannels);
        const auto channels = std::min(
            channelCount, static_cast<Steinberg::int32>(coefficients_.size()));
        for (Steinberg::int32 frame = 0; frame < data.numSamples; ++frame) {
            for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
                auto& timeline = timelines[id];
                while (timeline.hasPoint && timeline.sampleOffset <= frame) {
                    values[id] = timeline.value;
                    readNextPoint(timeline);
                }
            }

            const double targetDepth = values[kDepthId];
            const double returnSeconds = 0.2 + values[kReturnTimeId] * 5.8;
            const double transitionSeconds = targetDepth > smoothedDepth_
                ? 0.035
                : returnSeconds;
            const double smoothing = std::exp(
                -1.0 / (std::max(transitionSeconds, 0.001) * sampleRate_));
            smoothedDepth_ = targetDepth + (smoothedDepth_ - targetDepth) * smoothing;

            if (frame % kCoefficientUpdateInterval == 0) {
                const auto coefficients = makeLowPass(
                    cutoffForDepth(smoothedDepth_, sampleRate_), sampleRate_);
                coefficients_.fill(coefficients);
            }

            if (data.symbolicSampleSize == Steinberg::Vst::kSample32) {
                for (Steinberg::int32 channel = 0; channel < channels; ++channel) {
                    const float input = data.inputs[0].channelBuffers32[channel][frame];
                    data.outputs[0].channelBuffers32[channel][frame] =
                        processLowPass(input, coefficients_[channel], filterStates_[channel]);
                }
                for (Steinberg::int32 channel = channels; channel < channelCount; ++channel) {
                    data.outputs[0].channelBuffers32[channel][frame] =
                        data.inputs[0].channelBuffers32[channel][frame];
                }
            } else if (data.symbolicSampleSize == Steinberg::Vst::kSample64) {
                for (Steinberg::int32 channel = 0; channel < channels; ++channel) {
                    const double input = data.inputs[0].channelBuffers64[channel][frame];
                    data.outputs[0].channelBuffers64[channel][frame] =
                        processLowPass(input, coefficients_[channel], filterStates_[channel]);
                }
                for (Steinberg::int32 channel = channels; channel < channelCount; ++channel) {
                    data.outputs[0].channelBuffers64[channel][frame] =
                        data.inputs[0].channelBuffers64[channel][frame];
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

    static double cutoffForDepth(double depth, double sampleRate) {
        const double maximumCutoff = std::max(
            kMinimumCutoffHz, sampleRate * kMaximumCutoffNyquistRatio);
        return maximumCutoff * std::pow(kMinimumCutoffHz / maximumCutoff, depth);
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
    std::array<LowPassCoefficients, 2> coefficients_{};
    std::array<FilterState, 2> filterStates_{};
    double sampleRate_ = 44100.0;
    double smoothedDepth_ = 0.0;
    VSTGUI::VST3Editor* editor_ = nullptr;
    std::array<VSTGUI::CControl*, kParameterCount> controls_{};
};

BEGIN_FACTORY_DEF("SONORA Audio", "https://github.com/simon-of-course/sonora-mastering", "")

DEF_CLASS2(
    INLINE_UID(0xB6AE2913, 0xD05B4E62, 0x9C97B241, 0x6E1837A4),
    Steinberg::PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    "SONORA Drowner",
    0,
    "Fx|Effect",
    "0.1.0",
    kVstVersionString,
    SonoraDrowner::createInstance)

END_FACTORY
