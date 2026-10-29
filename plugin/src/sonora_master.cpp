#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/vst/vstguieditor.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstplugview.h"
#include "base/source/fstreamer.h"
#include "vstgui/lib/controls/cslider.h"
#include "vstgui/lib/controls/icontrollistener.h"
#include "vstgui/plugin-bindings/vst3editor.h"
#include "vstgui/uidescription/uiattributes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

enum ParameterId : Steinberg::Vst::ParamID {
    kModeId = 0,
    kAmountId = 1,
    kBassId = 2,
    kToneId = 3,
    kOutputId = 4,
    kBypassId = 5,
    kPresetId = 6,
    kNasalId = 7,
    kResonanceId = 8,
    kBeatToneId = 9,
    kSaturationId = 10,
    kEq60Id = 11,
    kEq250Id = 12,
    kEq1000Id = 13,
    kEq4000Id = 14,
    kEq12000Id = 15,
    kVocalFocusId = 16,
    kAnalyzeId = 17,
    kParameterCount
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultAmount = 30.0;
constexpr double kDefaultCenter = 0.5;
constexpr Steinberg::Vst::ParamID kLegacyParameterCount = 6;
constexpr Steinberg::Vst::ParamID kVersionTwoParameterCount = kVocalFocusId;
constexpr Steinberg::Vst::ParamID kVersionThreeParameterCount = kAnalyzeId;
constexpr Steinberg::int32 kAnalysisDurationSeconds = 8;

enum AnalysisStatus : int {
    kAnalysisReady,
    kAnalysisRunning,
    kAnalysisComplete,
    kAnalysisNoVocal,
    kAnalysisMissingSidechain,
    kAnalysisWrongMode,
    kAnalysisTooQuiet
};

struct AnalysisReadout {
    std::atomic<int> status{kAnalysisReady};
    std::atomic<double> progress{0.0};
    std::array<std::atomic<double>, 5> beatBandsDb{};
    std::array<std::atomic<double>, 5> vocalBandsDb{};
    std::atomic<double> beatRmsDb{-120.0};
    std::atomic<double> vocalRmsDb{-120.0};
    std::atomic<double> beatCrestDb{0.0};
};

struct MasterPreset {
    std::array<double, 5> eq;
    double ratio;
    double threshold;
};

constexpr std::array<MasterPreset, 6> kPresets{{
    {{{0.0, 0.0, 0.0, 0.0, 0.0}}, 1.35, -18.0},
    {{{0.2, 0.8, 0.0, -0.5, -0.5}}, 1.5, -18.5},
    {{{0.2, 0.0, 0.0, 0.5, 0.0}}, 1.8, -20.0},
    {{{-0.4, -0.2, 0.0, 0.2, 0.7}}, 1.4, -18.0},
    {{{0.6, 0.4, -0.3, 0.0, -0.2}}, 1.5, -19.0},
    {{{-0.3, -0.4, 0.2, 0.4, 0.2}}, 1.4, -18.0}
}};

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct BiquadState {
    double z1 = 0.0;
    double z2 = 0.0;
};

BiquadCoefficients makePeak(double sampleRate, double frequency, double q, double gainDb) {
    if (std::abs(gainDb) < 0.0001) {
        return {};
    }
    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double a0 = 1.0 + alpha / amplitude;
    return {
        (1.0 + alpha * amplitude) / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha * amplitude) / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha / amplitude) / a0
    };
}

BiquadCoefficients makeBandPass(double sampleRate, double frequency, double q) {
    frequency = std::clamp(frequency, 1.0, sampleRate * 0.45);
    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    return {
        alpha / a0,
        0.0,
        -alpha / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha) / a0
    };
}

BiquadCoefficients makeLowShelf(double sampleRate, double frequency, double gainDb) {
    if (std::abs(gainDb) < 0.0001) {
        return {};
    }

    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double alpha = sine / std::sqrt(2.0);
    const double beta = 2.0 * std::sqrt(amplitude) * alpha;
    const double a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + beta;

    return {
        (amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + beta)) / a0,
        (2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine)) / a0,
        (amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - beta)) / a0,
        (-2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine)) / a0,
        ((amplitude + 1.0) + (amplitude - 1.0) * cosine - beta) / a0
    };
}

BiquadCoefficients makeHighShelf(double sampleRate, double frequency, double gainDb) {
    if (std::abs(gainDb) < 0.0001) {
        return {};
    }

    const double amplitude = std::pow(10.0, gainDb / 40.0);
    const double omega = 2.0 * kPi * frequency / sampleRate;
    const double cosine = std::cos(omega);
    const double alpha = std::sin(omega) / std::sqrt(2.0);
    const double beta = 2.0 * std::sqrt(amplitude) * alpha;
    const double a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine + beta;

    return {
        (amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + beta)) / a0,
        (-2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine)) / a0,
        (amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - beta)) / a0,
        (2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine)) / a0,
        ((amplitude + 1.0) - (amplitude - 1.0) * cosine - beta) / a0
    };
}

template <typename Sample>
Sample runFilter(Sample input, const BiquadCoefficients& coefficients, BiquadState& state) {
    const double output = coefficients.b0 * input + state.z1;
    state.z1 = coefficients.b1 * input - coefficients.a1 * output + state.z2;
    state.z2 = coefficients.b2 * input - coefficients.a2 * output;
    return static_cast<Sample>(output);
}

double clampNormalized(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool getCustomViewRect(const VSTGUI::UIAttributes& attributes, VSTGUI::CRect& rect) {
    VSTGUI::CPoint origin;
    VSTGUI::CPoint size;
    if (!attributes.getPointAttribute("origin", origin)
        || !attributes.getPointAttribute("size", size)) {
        return false;
    }
    rect = {origin.x, origin.y, origin.x + size.x, origin.y + size.y};
    return true;
}

constexpr VSTGUI::CColor kEqInk{36, 35, 41, 255};
constexpr VSTGUI::CColor kEqLine{231, 229, 225, 255};
constexpr VSTGUI::CColor kEqAccent{101, 70, 163, 255};
constexpr VSTGUI::CColor kEqFill{101, 70, 163, 18};

class SonoraCardSurface final : public VSTGUI::CView {
public:
    SonoraCardSurface(const VSTGUI::CRect& size, VSTGUI::CColor fill)
        : CView(size), fill_(fill) {
        setMouseEnabled(false);
    }

    void draw(VSTGUI::CDrawContext* context) override {
        context->setDrawMode(VSTGUI::kAntiAliasing);
        context->setFillColor(fill_);
        context->setFrameColor(VSTGUI::CColor{232, 229, 238, 255});
        context->setLineWidth(1.0);
        auto* path = context->createRoundRectGraphicsPath(getViewSize(), 10.0);
        if (path) {
            context->drawGraphicsPath(path, VSTGUI::CDrawContext::kPathFilled);
            context->drawGraphicsPath(path, VSTGUI::CDrawContext::kPathStroked);
            path->forget();
        }
        context->setDrawMode(VSTGUI::kAliasing);
        setDirty(false);
    }

    CLASS_METHODS(SonoraCardSurface, CView)

private:
    VSTGUI::CColor fill_;
};

class SonoraAnalysisStatus final : public VSTGUI::CView {
public:
    SonoraAnalysisStatus(const VSTGUI::CRect& size, const AnalysisReadout& readout)
        : CView(size), readout_(readout) {
        setWantsIdle(true);
    }

    void onIdle() override {
        const auto status = readout_.status.load(std::memory_order_relaxed);
        const auto progress = static_cast<int>(
            std::lround(readout_.progress.load(std::memory_order_relaxed) * 100.0));
        if (status != lastStatus_ || progress != lastProgress_) {
            lastStatus_ = status;
            lastProgress_ = progress;
            invalid();
        }
    }

    void draw(VSTGUI::CDrawContext* context) override {
        auto bounds = getViewSize();
        context->setDrawMode(VSTGUI::kAntiAliasing);
        context->setFillColor(VSTGUI::CColor{250, 249, 247, 255});
        context->drawRect(bounds, VSTGUI::kDrawFilled);
        context->setFrameColor(kEqLine);
        context->drawRect(bounds, VSTGUI::kDrawStroked);
        context->setFontColor(kEqInk);

        const auto status = readout_.status.load(std::memory_order_relaxed);
        const auto progress = readout_.progress.load(std::memory_order_relaxed);
        char statusMessage[128]{};
        const char* message = "Analyse bereit | Beat am Eingang, Vocal als Sidechain; Wiedergabe starten und Analyse klicken (8 s).";
        switch (status) {
            case kAnalysisRunning:
                message = "Analysiere Beat und Vocal-Sidechain ...";
                break;
            case kAnalysisComplete:
                std::snprintf(statusMessage, sizeof(statusMessage),
                              "Fertig | RMS Beat %.1f / Vocal %.1f dBFS | Beat-Crest %.1f dB",
                              readout_.beatRmsDb.load(std::memory_order_relaxed),
                              readout_.vocalRmsDb.load(std::memory_order_relaxed),
                              readout_.beatCrestDb.load(std::memory_order_relaxed));
                message = statusMessage;
                break;
            case kAnalysisNoVocal:
                std::snprintf(statusMessage, sizeof(statusMessage),
                              "Fertig | kein Vocal erkannt | Beat RMS %.1f dBFS, Crest %.1f dB",
                              readout_.beatRmsDb.load(std::memory_order_relaxed),
                              readout_.beatCrestDb.load(std::memory_order_relaxed));
                message = statusMessage;
                break;
            case kAnalysisMissingSidechain:
                message = "Vocal-Sidechain fehlt | Vocal zum Sidechain-Eingang des Beat-Busses routen.";
                break;
            case kAnalysisWrongMode:
                message = "Bitte Bus-Modus Master waehlen; auf Vocal-/Beat-Anzeige-Modi wird nichts analysiert.";
                break;
            case kAnalysisTooQuiet:
                message = "Signal zu leise | Pegel und Vocal-Sidechain pruefen und erneut analysieren.";
                break;
            default:
                break;
        }
        context->drawString(message, {bounds.left + 10.0, bounds.top + 6.0,
                                      bounds.right - 10.0, bounds.top + 20.0},
                            VSTGUI::kLeftText);

        const double barLeft = bounds.left + 10.0;
        const double barRight = bounds.right - 10.0;
        const double barTop = bounds.top + 24.0;
        context->setFillColor(kEqLine);
        context->drawRect({barLeft, barTop, barRight, barTop + 3.0},
                          VSTGUI::kDrawFilled);
        context->setFillColor(kEqAccent);
        context->drawRect({barLeft, barTop,
                           barLeft + (barRight - barLeft) * progress, barTop + 3.0},
                          VSTGUI::kDrawFilled);

        static constexpr std::array<const char*, 5> bandNames{{
            "60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz"
        }};
        const double rowWidth = (bounds.getWidth() - 70.0) / bandNames.size();
        for (std::size_t index = 0; index < bandNames.size(); ++index) {
            const double left = bounds.left + 44.0 + rowWidth * index;
            context->setFontColor(VSTGUI::CColor{118, 115, 123, 255});
            context->drawString(bandNames[index],
                                {left, bounds.top + 27.0, left + rowWidth, bounds.top + 38.0},
                                VSTGUI::kCenterText);
            char value[24]{};
            const auto beatDb = readout_.beatBandsDb[index].load(std::memory_order_relaxed);
            std::snprintf(value, sizeof(value), "%+.0f", beatDb);
            context->setFontColor(kEqInk);
            context->drawString(value,
                                {left, bounds.top + 39.0, left + rowWidth, bounds.top + 51.0},
                                VSTGUI::kCenterText);
            const auto vocalDb = readout_.vocalBandsDb[index].load(std::memory_order_relaxed);
            std::snprintf(value, sizeof(value), "%+.0f", vocalDb);
            context->setFontColor(VSTGUI::CColor{101, 70, 163, 255});
            context->drawString(value,
                                {left, bounds.top + 52.0, left + rowWidth, bounds.bottom - 1.0},
                                VSTGUI::kCenterText);
        }
        context->setFontColor(VSTGUI::CColor{118, 115, 123, 255});
        context->drawString("Beat",
                            {bounds.left + 7.0, bounds.top + 39.0,
                             bounds.left + 42.0, bounds.top + 51.0},
                            VSTGUI::kLeftText);
        context->drawString("Vocal",
                            {bounds.left + 7.0, bounds.top + 52.0,
                             bounds.left + 42.0, bounds.bottom - 1.0},
                            VSTGUI::kLeftText);
        context->setDrawMode(VSTGUI::kAliasing);
        setDirty(false);
    }

    CLASS_METHODS(SonoraAnalysisStatus, CView)

private:
    const AnalysisReadout& readout_;
    int lastStatus_ = -1;
    int lastProgress_ = -1;
};

class SonoraEqGraph final : public VSTGUI::CView {
public:
    SonoraEqGraph(const VSTGUI::CRect& size,
                  const std::array<std::atomic<double>, 5>& automaticGains)
        : CView(size), automaticGains_(automaticGains) {
        setWantsIdle(true);
    }

    void setBandGains(const std::array<double, 5>& gains) {
        gains_ = gains;
        invalid();
    }

    void onIdle() override {
        bool changed = false;
        for (std::size_t index = 0; index < automaticGains_.size(); ++index) {
            const double value = automaticGains_[index].load(std::memory_order_relaxed);
            if (std::abs(value - lastAutomaticGains_[index]) > 0.001) {
                lastAutomaticGains_[index] = value;
                changed = true;
            }
        }
        if (changed) {
            invalid();
        }
    }

    void draw(VSTGUI::CDrawContext* context) override {
        auto bounds = getViewSize();
        context->setFillColor(VSTGUI::CColor{250, 249, 247, 255});
        context->drawRect(bounds, VSTGUI::kDrawFilled);
        bounds.inset(1.0, 1.0);

        context->setDrawMode(VSTGUI::kAntiAliasing);
        context->setLineStyle(VSTGUI::kLineSolid);
        context->setLineWidth(1.0);
        context->setFrameColor(kEqLine);
        constexpr int gridRows = 4;
        const double zeroY = bounds.top + bounds.getHeight() * 0.5;
        for (int row = 0; row <= gridRows; ++row) {
            const double y = bounds.top + bounds.getHeight() * row / gridRows;
            context->setFrameColor(row == gridRows / 2
                ? VSTGUI::CColor{207, 202, 216, 255}
                : kEqLine);
            context->drawLine({bounds.left, y}, {bounds.right, y});
        }
        constexpr std::array<double, 5> frequencies{{60.0, 250.0, 1000.0, 4000.0, 12000.0}};
        constexpr double minFrequency = 20.0;
        constexpr double maxFrequency = 20000.0;
        const auto frequencyX = [&bounds, minFrequency, maxFrequency](double frequency) {
            return bounds.left + bounds.getWidth()
                * std::log(frequency / minFrequency) / std::log(maxFrequency / minFrequency);
        };
        for (const auto frequency : frequencies) {
            const double x = frequencyX(frequency);
            context->drawLine({x, bounds.top}, {x, bounds.bottom});
        }

        std::array<VSTGUI::CPoint, 7> points{};
        const auto gainY = [&bounds](double gain) {
            return bounds.top + bounds.getHeight() * (1.0 - (gain + 8.0) / 16.0);
        };
        points.front() = {bounds.left, gainY(
            gains_.front() + automaticGains_.front().load(std::memory_order_relaxed))};
        for (std::size_t index = 0; index < gains_.size(); ++index) {
            const double gain = gains_[index]
                + automaticGains_[index].load(std::memory_order_relaxed);
            points[index + 1] = {frequencyX(frequencies[index]), gainY(gain)};
        }
        points.back() = {bounds.right, gainY(
            gains_.back() + automaticGains_.back().load(std::memory_order_relaxed))};

        auto* area = context->createGraphicsPath();
        auto* curve = context->createGraphicsPath();
        if (area && curve) {
            area->beginSubpath(points.front().x, zeroY);
            area->addLine(points.front());
            curve->beginSubpath(points.front());
            for (std::size_t index = 0; index + 1 < points.size(); ++index) {
                const auto& previous = points[index == 0 ? index : index - 1];
                const auto& start = points[index];
                const auto& end = points[index + 1];
                const auto& next = points[std::min(index + 2, points.size() - 1)];
                const double span = end.x - start.x;
                area->addBezierCurve(
                    start.x + span / 3.0,
                    start.y + (end.y - previous.y) / 6.0,
                    end.x - span / 3.0,
                    end.y - (next.y - start.y) / 6.0,
                    end.x, end.y);
                curve->addBezierCurve(
                    start.x + span / 3.0,
                    start.y + (end.y - previous.y) / 6.0,
                    end.x - span / 3.0,
                    end.y - (next.y - start.y) / 6.0,
                    end.x, end.y);
            }
            area->addLine(points.back().x, zeroY);
            area->closeSubpath();
            context->setFillColor(kEqFill);
            context->drawGraphicsPath(area, VSTGUI::CDrawContext::kPathFilled);
            context->setFrameColor(kEqAccent);
            context->setLineWidth(2.0);
            context->drawGraphicsPath(curve, VSTGUI::CDrawContext::kPathStroked);
        }
        if (area) {
            area->forget();
        }
        if (curve) {
            curve->forget();
        }

        context->setFrameColor(kEqAccent);
        context->setFillColor(VSTGUI::CColor{255, 255, 255, 255});
        for (std::size_t index = 1; index + 1 < points.size(); ++index) {
            const auto& point = points[index];
            context->drawEllipse(
                {point.x - 3.5, point.y - 3.5, point.x + 3.5, point.y + 3.5},
                VSTGUI::kDrawFilledAndStroked);
        }
        context->setDrawMode(VSTGUI::kAliasing);
        setDirty(false);
    }

    CLASS_METHODS(SonoraEqGraph, CView)

private:
    std::array<double, 5> gains_{};
    const std::array<std::atomic<double>, 5>& automaticGains_;
    std::array<double, 5> lastAutomaticGains_{};
};

class SonoraEqSlider final : public VSTGUI::CVerticalSlider {
public:
    SonoraEqSlider(const VSTGUI::CRect& size, VSTGUI::IControlListener* listener,
                   Steinberg::Vst::ParamID tag)
        : CVerticalSlider(size, listener, static_cast<int32_t>(tag),
                          VSTGUI::CPoint{0.0, 7.0},
                          std::max(1, static_cast<int32_t>(size.getHeight() - 14.0)),
                          nullptr, nullptr, VSTGUI::CPoint{0.0, 0.0}, kBottom) {
        setSliderMode(VSTGUI::CSliderMode::FreeClick);
        setWantsFocus(true);
    }

    void draw(VSTGUI::CDrawContext* context) override {
        const auto bounds = getViewSize();
        const double centerX = bounds.getCenter().x;
        const double travelTop = bounds.top + 7.0;
        const double travelBottom = bounds.bottom - 7.0;
        const double middleY = (travelTop + travelBottom) * 0.5;
        const double handleY = travelBottom - getValueNormalized() * (travelBottom - travelTop);
        auto roundedRect = [context](const VSTGUI::CRect& rect, double radius,
                                     const VSTGUI::CColor& color) {
            auto* path = context->createRoundRectGraphicsPath(rect, radius);
            if (path) {
                context->setFillColor(color);
                context->drawGraphicsPath(path, VSTGUI::CDrawContext::kPathFilled);
                path->forget();
            }
        };

        context->setDrawMode(VSTGUI::kAntiAliasing);
        roundedRect({centerX - 2.0, travelTop, centerX + 2.0, travelBottom}, 2.0, kEqLine);
        const double valueTop = std::min(handleY, middleY);
        const double valueBottom = std::max(handleY, middleY);
        if (valueBottom - valueTop > 0.5) {
            roundedRect({centerX - 2.0, valueTop, centerX + 2.0, valueBottom},
                        2.0, kEqAccent);
        }

        context->setFrameColor(kEqLine);
        context->setFillColor(VSTGUI::CColor{255, 255, 255, 255});
        context->setLineWidth(1.5);
        context->drawEllipse(
            {centerX - 7.0, handleY - 7.0, centerX + 7.0, handleY + 7.0},
            VSTGUI::kDrawFilledAndStroked);
        context->setDrawMode(VSTGUI::kAliasing);
        setDirty(false);
    }
};

} // namespace

class SonoraMaster final : public Steinberg::Vst::SingleComponentEffect,
                           public VSTGUI::VST3EditorDelegate,
                           public VSTGUI::IControlListener {
public:
    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new SonoraMaster());
    }

    SonoraMaster() {
        parameterValues_[kModeId].store(1.0);
        parameterValues_[kAmountId].store(kDefaultAmount / 100.0);
        parameterValues_[kBassId].store(kDefaultCenter);
        parameterValues_[kToneId].store(kDefaultCenter);
        parameterValues_[kOutputId].store(kDefaultCenter);
        parameterValues_[kBypassId].store(0.0);
        parameterValues_[kPresetId].store(0.0);
        parameterValues_[kNasalId].store(0.0);
        parameterValues_[kResonanceId].store(0.0);
        parameterValues_[kBeatToneId].store(kDefaultCenter);
        parameterValues_[kSaturationId].store(0.0);
        parameterValues_[kVocalFocusId].store(kDefaultCenter);
        parameterValues_[kAnalyzeId].store(0.0);
        for (auto& value : autoEqDb_) {
            value.store(0.0);
        }
        for (std::size_t band = 0; band < analysisReadout_.beatBandsDb.size(); ++band) {
            analysisReadout_.beatBandsDb[band].store(-120.0);
            analysisReadout_.vocalBandsDb[band].store(-120.0);
        }
        for (Steinberg::Vst::ParamID id = kEq60Id; id <= kEq12000Id; ++id) {
            parameterValues_[id].store(kDefaultCenter);
        }
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override {
        const auto result = SingleComponentEffect::initialize(context);
        if (result != Steinberg::kResultOk) {
            return result;
        }

        addAudioInput(STR16("Stereo In"), Steinberg::Vst::SpeakerArr::kStereo);
        addAudioInput(
            STR16("Vocal Sidechain"),
            Steinberg::Vst::SpeakerArr::kMono,
            Steinberg::Vst::kAux,
            0);
        addAudioOutput(STR16("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);

        auto* mode = new Steinberg::Vst::StringListParameter(
            STR16("Bus-Modus"), kModeId);
        mode->appendString(STR16("Vocal"));
        mode->appendString(STR16("Beat"));
        mode->appendString(STR16("Master"));
        mode->getInfo().defaultNormalizedValue = 1.0;
        mode->setNormalized(1.0);
        parameters.addParameter(mode);

        auto* preset = new Steinberg::Vst::StringListParameter(
            STR16("Preset"), kPresetId);
        preset->appendString(STR16("Natural"));
        preset->appendString(STR16("Warm"));
        preset->appendString(STR16("Punch"));
        preset->appendString(STR16("Air"));
        preset->appendString(STR16("Bass"));
        preset->appendString(STR16("Vocal Clarity"));
        preset->getInfo().defaultNormalizedValue = 0.0;
        preset->setNormalized(0.0);
        parameters.addParameter(preset);

        addRangeParameter("Staerke", "%", kAmountId, 0.0, 100.0, kDefaultAmount);
        addRangeParameter("Nasalitaet", "%", kNasalId, 0.0, 100.0, 0.0);
        addRangeParameter("Resonanzen", "%", kResonanceId, 0.0, 100.0, 0.0);
        addRangeParameter("Stimmen-Praesenz", "dB", kToneId, -4.0, 4.0, 0.0);
        addRangeParameter("Bass", "dB", kBassId, -6.0, 6.0, 0.0);
        addRangeParameter("Beat-Hoehen", "dB", kBeatToneId, -6.0, 6.0, 0.0);
        addRangeParameter("Saettigung", "%", kSaturationId, 0.0, 100.0, 0.0);
        addRangeParameter("EQ 60 Hz", "dB", kEq60Id, -8.0, 8.0, 0.0);
        addRangeParameter("EQ 250 Hz", "dB", kEq250Id, -8.0, 8.0, 0.0);
        addRangeParameter("EQ 1 kHz", "dB", kEq1000Id, -8.0, 8.0, 0.0);
        addRangeParameter("EQ 4 kHz", "dB", kEq4000Id, -8.0, 8.0, 0.0);
        addRangeParameter("EQ 12 kHz", "dB", kEq12000Id, -8.0, 8.0, 0.0);
        addRangeParameter("Vocal-Fokus", "%", kVocalFocusId, 0.0, 100.0, 50.0);
        parameters.addParameter(
            STR16("Analyse starten"), nullptr,
            Steinberg::Vst::kStepCountToggle, 0.0,
            Steinberg::Vst::ParameterInfo::kCanAutomate,
            kAnalyzeId);
        addRangeParameter("Ausgang", "dB", kOutputId, -6.0, 6.0, 0.0);
        parameters.addParameter(
            STR16("Bypass"), nullptr,
            Steinberg::Vst::kStepCountToggle, 0.0,
            Steinberg::Vst::ParameterInfo::kCanAutomate
                | Steinberg::Vst::ParameterInfo::kIsBypass,
            kBypassId);

        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override {
        if (!state) {
            for (auto& channel : filterStates_) {
                channel = {};
            }
            for (auto& channel : sidechainDetectorStates_) {
                channel = {};
            }
            for (auto& channel : duckBandStates_) {
                channel = {};
            }
            vocalBandEnvelopes_ = {};
            detectorEnvelope_ = 0.0;
            smoothedGain_ = 1.0;
            analysisRequest_.store(false, std::memory_order_relaxed);
            analysisReadout_.progress.store(0.0, std::memory_order_relaxed);
            analysisReadout_.status.store(kAnalysisReady, std::memory_order_relaxed);
        }
        return SingleComponentEffect::setActive(state);
    }

    Steinberg::tresult PLUGIN_API setParamNormalized(
        Steinberg::Vst::ParamID tag,
        Steinberg::Vst::ParamValue value) override {
        const auto result = SingleComponentEffect::setParamNormalized(tag, value);
        if (result == Steinberg::kResultOk && tag < kParameterCount) {
            parameterValues_[tag].store(clampNormalized(value), std::memory_order_relaxed);
            if (tag == kAnalyzeId && value >= 0.5) {
                analysisRequest_.store(true, std::memory_order_release);
            }
        }
        return result;
    }

    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override {
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        Steinberg::int32 version = 0;
        if (!streamer.readInt32(version) || (version < 1 || version > 4)) {
            return Steinberg::kResultFalse;
        }

        const auto count = version == 1
            ? kLegacyParameterCount
            : (version == 2 ? kVersionTwoParameterCount
               : (version == 3 ? kVersionThreeParameterCount : kParameterCount));
        for (Steinberg::Vst::ParamID id = 0; id < count; ++id) {
            double value = 0.0;
            if (!streamer.readDouble(value)) {
                return Steinberg::kResultFalse;
            }
            setParamNormalized(id, clampNormalized(value));
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override {
        Steinberg::IBStreamer streamer(state, kLittleEndian);
        if (!streamer.writeInt32(4)) {
            return Steinberg::kResultFalse;
        }
        for (const auto& value : parameterValues_) {
            if (!streamer.writeDouble(value.load(std::memory_order_relaxed))) {
                return Steinberg::kResultFalse;
            }
        }
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API setupProcessing(
        Steinberg::Vst::ProcessSetup& setup) override {
        sampleRate_ = std::max(1.0, setup.sampleRate);
        return SingleComponentEffect::setupProcessing(setup);
    }

    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 size) override {
        return size == Steinberg::Vst::kSample32 || size == Steinberg::Vst::kSample64
            ? Steinberg::kResultTrue
            : Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;

    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs,
        Steinberg::int32 inputCount,
        Steinberg::Vst::SpeakerArrangement* outputs,
        Steinberg::int32 outputCount) override {
        if (!inputs || !outputs || (inputCount != 1 && inputCount != 2)
            || outputCount != 1) {
            return Steinberg::kResultFalse;
        }
        const auto inputChannels = Steinberg::Vst::SpeakerArr::getChannelCount(inputs[0]);
        const auto outputChannels = Steinberg::Vst::SpeakerArr::getChannelCount(outputs[0]);
        if (inputChannels != outputChannels || (inputChannels != 1 && inputChannels != 2)) {
            return Steinberg::kResultFalse;
        }
        if (inputCount == 2) {
            const auto sidechainChannels =
                Steinberg::Vst::SpeakerArr::getChannelCount(inputs[1]);
            if (sidechainChannels != 1 && sidechainChannels != 2) {
                return Steinberg::kResultFalse;
            }
        }
        return SingleComponentEffect::setBusArrangements(inputs, inputCount, outputs, outputCount);
    }

    Steinberg::IPlugView* PLUGIN_API createView(const char* name) override {
        if (name && Steinberg::FIDStringsEqual(name, Steinberg::Vst::ViewType::kEditor)) {
            auto* editor = new VSTGUI::VST3Editor(this, "view", "sonora.uidesc");
            editor->setDelegate(this);
            return editor;
        }
        return nullptr;
    }

    VSTGUI::CView* createCustomView(
        VSTGUI::UTF8StringPtr name,
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription*,
        VSTGUI::VST3Editor* editor) override {
        if (std::strcmp(name, "SonoraCardSurface") == 0) {
            VSTGUI::CRect rect;
            if (!getCustomViewRect(attributes, rect)) {
                return nullptr;
            }
            const auto* surfaceName = attributes.getAttributeValue("name");
            const bool accentSurface = surfaceName && *surfaceName == "tone-surface";
            return new SonoraCardSurface(
                rect,
                accentSurface
                    ? VSTGUI::CColor{250, 248, 253, 255}
                    : VSTGUI::CColor{255, 255, 255, 255});
        }
        if (std::strcmp(name, "SonoraEqGraph") == 0) {
            VSTGUI::CRect rect;
            if (!getCustomViewRect(attributes, rect)) {
                return nullptr;
            }
            eqGraphView_ = new SonoraEqGraph(rect, autoEqDb_);
            return eqGraphView_;
        }
        if (std::strcmp(name, "SonoraAnalysisStatus") == 0) {
            VSTGUI::CRect rect;
            if (!getCustomViewRect(attributes, rect)) {
                return nullptr;
            }
            analysisStatusView_ = new SonoraAnalysisStatus(rect, analysisReadout_);
            return analysisStatusView_;
        }

        static constexpr std::array<std::pair<const char*, Steinberg::Vst::ParamID>, 5>
            eqFaders{{
                {"SonoraEqFader60", kEq60Id},
                {"SonoraEqFader250", kEq250Id},
                {"SonoraEqFader1000", kEq1000Id},
                {"SonoraEqFader4000", kEq4000Id},
                {"SonoraEqFader12000", kEq12000Id}
            }};
        for (const auto& [viewName, parameterId] : eqFaders) {
            if (std::strcmp(name, viewName) == 0) {
                VSTGUI::CRect rect;
                if (!getCustomViewRect(attributes, rect)) {
                    return nullptr;
                }
                auto* slider = new SonoraEqSlider(rect, editor, parameterId);
                slider->setDefaultValue(static_cast<float>(kDefaultCenter));
                return slider;
            }
        }
        return nullptr;
    }

    VSTGUI::CView* verifyView(
        VSTGUI::CView* view,
        const VSTGUI::UIAttributes& attributes,
        const VSTGUI::IUIDescription* description,
        VSTGUI::VST3Editor* editor) override {
        const auto* name = attributes.getAttributeValue("name");
        if (name && *name == "mode-selector") {
            modeControl_ = dynamic_cast<VSTGUI::CControl*>(view);
            if (modeControl_) {
                modeControl_->registerControlListener(this);
            }
        } else if (name && *name == "analyze-button") {
            analyzeControl_ = dynamic_cast<VSTGUI::CControl*>(view);
            if (analyzeControl_) {
                analyzeControl_->registerControlListener(this);
            }
        } else if (auto* control = dynamic_cast<VSTGUI::CControl*>(view);
                   control && control->getTag() >= kEq60Id
                   && control->getTag() <= kEq12000Id) {
            const auto index = static_cast<std::size_t>(control->getTag() - kEq60Id);
            if (index < eqControls_.size()) {
                eqControls_[index] = control;
                control->registerControlListener(this);
            }
        } else if (name && *name == "master-panel") {
            masterPanel_ = dynamic_cast<VSTGUI::CViewContainer*>(view);
        } else if (name && *name == "vocal-status") {
            vocalStatus_ = view;
        } else if (name && *name == "beat-status") {
            beatStatus_ = view;
        }
        editor_ = editor;
        return VSTGUI::VST3EditorDelegate::verifyView(view, attributes, description, editor);
    }

    void didOpen(VSTGUI::VST3Editor* editor) override {
        editor_ = editor;
        updateModeViews();
        updateEqGraph(eqControls_[0]);
    }

    void willClose(VSTGUI::VST3Editor* editor) override {
        if (editor_ == editor && modeControl_) {
            modeControl_->unregisterControlListener(this);
        }
        for (auto* control : eqControls_) {
            if (control) {
                control->unregisterControlListener(this);
            }
        }
        editor_ = nullptr;
        modeControl_ = nullptr;
        if (analyzeControl_) {
            analyzeControl_->unregisterControlListener(this);
        }
        analyzeControl_ = nullptr;
        masterPanel_ = nullptr;
        vocalStatus_ = nullptr;
        beatStatus_ = nullptr;
        eqGraphView_ = nullptr;
        analysisStatusView_ = nullptr;
        eqControls_.fill(nullptr);
    }

    void valueChanged(VSTGUI::CControl* control) override {
        if (control == modeControl_) {
            updateModeViews();
        }
        if (control == analyzeControl_) {
            if (control->getValueNormalized() >= 0.5) {
                analysisRequest_.store(true, std::memory_order_release);
            }
        }
        updateEqGraph(control);
    }

    void controlBeginEdit(VSTGUI::CControl*) override {}
    void controlEndEdit(VSTGUI::CControl*) override {}

    OBJ_METHODS(SonoraMaster, SingleComponentEffect)
    REFCOUNT_METHODS(SingleComponentEffect)

private:
    void addRangeParameter(
        const char* title,
        const char* units,
        Steinberg::Vst::ParamID id,
        double minimum,
        double maximum,
        double defaultValue) {
        Steinberg::Vst::String128 parameterTitle{};
        Steinberg::Vst::String128 parameterUnits{};
        Steinberg::UString(
            parameterTitle,
            sizeof(parameterTitle) / sizeof(Steinberg::Vst::TChar)).fromAscii(title);
        Steinberg::UString(
            parameterUnits,
            sizeof(parameterUnits) / sizeof(Steinberg::Vst::TChar)).fromAscii(units);
        parameters.addParameter(new Steinberg::Vst::RangeParameter(
            parameterTitle,
            id,
            parameterUnits,
            minimum,
            maximum,
            defaultValue));
    }

    void startAnalysis(int mode, Steinberg::int32 sidechainChannelCount) {
        analysisRequest_.store(false, std::memory_order_relaxed);
        analysisReadout_.progress.store(0.0, std::memory_order_relaxed);
        if (mode != 2) {
            analysisReadout_.status.store(kAnalysisWrongMode, std::memory_order_release);
            return;
        }
        if (sidechainChannelCount <= 0) {
            analysisReadout_.status.store(kAnalysisMissingSidechain, std::memory_order_release);
            return;
        }

        analysisFramesTarget_ = static_cast<std::uint64_t>(
            std::max(1.0, sampleRate_ * kAnalysisDurationSeconds));
        analysisFramesRemaining_ = analysisFramesTarget_;
        analysisFramesCaptured_ = 0;
        analysisChannelCount_ = std::clamp<Steinberg::int32>(channelCountForAnalysis_, 1, 2);
        analysisSidechainChannelCount_ = std::clamp<Steinberg::int32>(
            sidechainChannelCount, 1, 2);
        analysisPower_ = {};
        analysisBandPower_ = {};
        analysisPeak_ = {};
        analysisBandStates_ = {};
        static constexpr std::array<double, 5> analysisFrequencies{{
            60.0, 250.0, 1000.0, 4000.0, 12000.0
        }};
        for (std::size_t band = 0; band < analysisFrequencies.size(); ++band) {
            analysisFilters_[band] = makeBandPass(
                sampleRate_, analysisFrequencies[band], 0.8);
        }
        analysisReadout_.status.store(kAnalysisRunning, std::memory_order_release);
    }

    template <typename Sample>
    void captureAnalysis(Sample** input, Steinberg::int32 channelCount,
                        Sample** sidechain, Steinberg::int32 sidechainChannelCount,
                        Steinberg::int32 sampleCount, int mode) {
        channelCountForAnalysis_ = channelCount;
        if (analysisRequest_.exchange(false, std::memory_order_acq_rel)) {
            startAnalysis(mode, sidechainChannelCount);
        }
        if (analysisReadout_.status.load(std::memory_order_acquire) != kAnalysisRunning) {
            return;
        }
        if (mode != 2) {
            analysisReadout_.status.store(kAnalysisWrongMode, std::memory_order_release);
            return;
        }
        if (!sidechain || sidechainChannelCount <= 0) {
            analysisReadout_.status.store(kAnalysisMissingSidechain, std::memory_order_release);
            return;
        }

        const auto framesToCapture = static_cast<Steinberg::int32>(
            std::min<std::uint64_t>(analysisFramesRemaining_,
                                    static_cast<std::uint64_t>(sampleCount)));
        for (Steinberg::int32 frame = 0; frame < framesToCapture; ++frame) {
            const std::array<Sample**, 2> sources{{input, sidechain}};
            const std::array<Steinberg::int32, 2> sourceChannels{{
                analysisChannelCount_, analysisSidechainChannelCount_
            }};
            for (std::size_t source = 0; source < sources.size(); ++source) {
                for (Steinberg::int32 channel = 0; channel < sourceChannels[source]; ++channel) {
                    if (!sources[source][channel]) {
                        continue;
                    }
                    const auto channelIndex = static_cast<std::size_t>(channel);
                    const double sample = static_cast<double>(sources[source][channel][frame]);
                    if (!std::isfinite(sample)) {
                        continue;
                    }
                    analysisPower_[source] += sample * sample;
                    analysisPeak_[source] = std::max(analysisPeak_[source], std::abs(sample));
                    for (std::size_t band = 0; band < analysisFilters_.size(); ++band) {
                        const double filtered = runFilter(
                            sample, analysisFilters_[band],
                            analysisBandStates_[source][channelIndex][band]);
                        analysisBandPower_[source][band] += filtered * filtered;
                    }
                }
            }
        }
        analysisFramesCaptured_ += static_cast<std::uint64_t>(framesToCapture);
        analysisFramesRemaining_ -= static_cast<std::uint64_t>(framesToCapture);
        analysisReadout_.progress.store(
            static_cast<double>(analysisFramesCaptured_)
                / static_cast<double>(analysisFramesTarget_),
            std::memory_order_relaxed);
        if (analysisFramesRemaining_ == 0) {
            finishAnalysis();
        }
    }

    void finishAnalysis() {
        const std::array<Steinberg::int32, 2> channelCounts{{
            analysisChannelCount_, analysisSidechainChannelCount_
        }};
        std::array<double, 2> rmsDb{{-120.0, -120.0}};
        for (std::size_t source = 0; source < channelCounts.size(); ++source) {
            const double sampleCount = static_cast<double>(analysisFramesCaptured_)
                * static_cast<double>(channelCounts[source]);
            const double meanPower = sampleCount > 0.0
                ? analysisPower_[source] / sampleCount
                : 0.0;
            rmsDb[source] = meanPower > 1e-12
                ? 10.0 * std::log10(meanPower)
                : -120.0;
            for (std::size_t band = 0; band < analysisBandPower_[source].size(); ++band) {
                const double bandPower = sampleCount > 0.0
                    ? analysisBandPower_[source][band] / sampleCount
                    : 0.0;
                const double bandDb = bandPower > 1e-12
                    ? 10.0 * std::log10(bandPower)
                    : -120.0;
                auto& destination = source == 0
                    ? analysisReadout_.beatBandsDb[band]
                    : analysisReadout_.vocalBandsDb[band];
                destination.store(bandDb, std::memory_order_relaxed);
            }
        }
        analysisReadout_.beatRmsDb.store(rmsDb[0], std::memory_order_relaxed);
        analysisReadout_.vocalRmsDb.store(rmsDb[1], std::memory_order_relaxed);
        const double beatRms = std::pow(10.0, rmsDb[0] / 20.0);
        const double crestDb = beatRms > 1e-9 && analysisPeak_[0] > 0.0
            ? 20.0 * std::log10(analysisPeak_[0] / beatRms)
            : 0.0;
        analysisReadout_.beatCrestDb.store(crestDb, std::memory_order_relaxed);

        if (rmsDb[0] < -65.0) {
            analysisReadout_.status.store(kAnalysisTooQuiet, std::memory_order_release);
            return;
        }

        double meanBandDb = 0.0;
        for (const auto& band : analysisReadout_.beatBandsDb) {
            meanBandDb += band.load(std::memory_order_relaxed);
        }
        meanBandDb /= static_cast<double>(analysisReadout_.beatBandsDb.size());
        const bool vocalPresent = rmsDb[1] > -60.0;
        for (std::size_t band = 0; band < analysisReadout_.beatBandsDb.size(); ++band) {
            const double beatDb = analysisReadout_.beatBandsDb[band].load(
                std::memory_order_relaxed);
            const double spectralCut = std::clamp((beatDb - meanBandDb - 3.0) * 0.12,
                                                  0.0, 0.8);
            double overlapCut = 0.0;
            if (vocalPresent && (band == 2 || band == 3)) {
                const double vocalDb = analysisReadout_.vocalBandsDb[band].load(
                    std::memory_order_relaxed);
                overlapCut = std::clamp((beatDb - vocalDb - 1.0) * 0.12,
                                        0.0, 1.2);
            }
            autoEqDb_[band].store(-std::max(spectralCut, overlapCut),
                                  std::memory_order_relaxed);
        }
        autoCompressionRatio_.store(
            std::clamp(1.0 + std::max(0.0, crestDb - 10.0) * 0.015, 1.0, 1.12),
            std::memory_order_relaxed);
        analysisReadout_.progress.store(1.0, std::memory_order_relaxed);
        analysisReadout_.status.store(
            vocalPresent ? kAnalysisComplete : kAnalysisNoVocal,
            std::memory_order_release);
    }

    template <typename Sample>
    void processBlock(Sample** input, Sample** output, Steinberg::int32 channelCount,
                      Sample** sidechain, Steinberg::int32 sidechainChannelCount,
                      Steinberg::int32 sampleCount, int mode,
                      const std::array<double, kParameterCount>& values, bool bypass) {
        channelCountForAnalysis_ = channelCount;
        captureAnalysis(input, channelCount, sidechain, sidechainChannelCount,
                        sampleCount, mode);
        if (mode != 2 || bypass) {
            for (Steinberg::int32 channel = 0; channel < channelCount; ++channel) {
                std::copy_n(input[channel], sampleCount, output[channel]);
            }
            sidechainDetectorStates_ = {};
            duckBandStates_ = {};
            vocalBandEnvelopes_ = {};
            return;
        }

        const double amount = values[kAmountId];
        const auto presetIndex = static_cast<std::size_t>(
            std::clamp(std::lround(values[kPresetId] * (kPresets.size() - 1)), 0L,
                       static_cast<long>(kPresets.size() - 1)));
        const auto& preset = kPresets[presetIndex];
        std::array<double, 5> automaticEq{};
        for (std::size_t index = 0; index < automaticEq.size(); ++index) {
            const double target = autoEqDb_[index].load(std::memory_order_relaxed);
            smoothedAutoEqDb_[index] += (target - smoothedAutoEqDb_[index]) * 0.08;
            automaticEq[index] = smoothedAutoEqDb_[index];
        }
        const std::array<double, 5> manualEq{{
            values[kEq60Id] * 16.0 - 8.0 + automaticEq[0],
            values[kEq250Id] * 16.0 - 8.0 + automaticEq[1],
            values[kEq1000Id] * 16.0 - 8.0 + automaticEq[2],
            values[kEq4000Id] * 16.0 - 8.0 + automaticEq[3],
            values[kEq12000Id] * 16.0 - 8.0 + automaticEq[4]
        }};
        std::array<BiquadCoefficients, 7> filters{{
            makeLowShelf(sampleRate_, 60.0, preset.eq[0] * amount + manualEq[0]),
            makePeak(sampleRate_, 250.0, 0.65, preset.eq[1] * amount + manualEq[1]),
            makePeak(sampleRate_, 1000.0, 0.65, preset.eq[2] * amount + manualEq[2]),
            makePeak(sampleRate_, 4000.0, 0.65, preset.eq[3] * amount + manualEq[3]),
            makeHighShelf(sampleRate_, 12000.0, preset.eq[4] * amount + manualEq[4]),
            makePeak(sampleRate_, 900.0, 1.0, -2.5 * values[kNasalId] * amount),
            makePeak(sampleRate_, 3400.0, 3.2, -3.0 * values[kResonanceId] * amount)
        }};
        filters[0] = makeLowShelf(
            sampleRate_, 60.0,
            preset.eq[0] * amount + manualEq[0] + (values[kBassId] * 12.0 - 6.0) * amount);
        filters[3] = makePeak(
            sampleRate_, 4000.0, 0.65,
            preset.eq[3] * amount + manualEq[3] + (values[kToneId] * 8.0 - 4.0) * amount);
        filters[4] = makeHighShelf(
            sampleRate_, 12000.0,
            preset.eq[4] * amount + manualEq[4] + (values[kBeatToneId] * 12.0 - 6.0) * amount);
        const double outputGainDb = values[kOutputId] * 12.0 - 6.0;
        const double targetGain = std::pow(10.0, outputGainDb / 20.0);
        const double ceiling = std::pow(10.0, -1.0 / 20.0);
        const double attack = std::exp(-1.0 / (sampleRate_ * 0.01));
        const double release = std::exp(-1.0 / (sampleRate_ * 0.12));
        const double vocalAttack = std::exp(-1.0 / (sampleRate_ * 0.008));
        const double vocalRelease = std::exp(-1.0 / (sampleRate_ * 0.09));
        const double duckThreshold = 0.012;
        const double duckFullLevel = 0.075;
        const std::array<BiquadCoefficients, 2> vocalBands{{
            makeBandPass(sampleRate_, 900.0, 0.8),
            makeBandPass(sampleRate_, 3200.0, 0.8)
        }};
        const std::array<BiquadCoefficients, 2> duckBands = vocalBands;
        const double maxDuck = 1.0 - std::pow(10.0, -2.5 / 20.0);
        const double threshold = std::pow(
            10.0, (-16.0 + (preset.threshold + 16.0) * amount) / 20.0);
        const double targetAutoCompressionRatio =
            autoCompressionRatio_.load(std::memory_order_relaxed);
        smoothedAutoCompressionRatio_ +=
            (targetAutoCompressionRatio - smoothedAutoCompressionRatio_) * 0.08;
        const double ratio = (1.0 + (preset.ratio - 1.0) * amount)
            * smoothedAutoCompressionRatio_;
        const double saturation = values[kSaturationId];
        const double saturationDrive = 1.0 + saturation * 4.0;

        for (Steinberg::int32 frame = 0; frame < sampleCount; ++frame) {
            std::array<double, 2> vocalActivity{};
            for (std::size_t band = 0; band < vocalBands.size(); ++band) {
                double vocalLevel = 0.0;
                if (sidechain && sidechainChannelCount > 0) {
                    for (Steinberg::int32 channel = 0;
                         channel < sidechainChannelCount; ++channel) {
                        const auto channelIndex = static_cast<std::size_t>(
                            std::min<Steinberg::int32>(channel, 1));
                        const double filtered = runFilter(
                            static_cast<double>(sidechain[channel][frame]),
                            vocalBands[band],
                            sidechainDetectorStates_[channelIndex][band]);
                        vocalLevel = std::max(vocalLevel, std::abs(filtered));
                    }
                }
                const double coefficient = vocalLevel > vocalBandEnvelopes_[band]
                    ? vocalAttack
                    : vocalRelease;
                vocalBandEnvelopes_[band] = coefficient * vocalBandEnvelopes_[band]
                    + (1.0 - coefficient) * vocalLevel;
                vocalActivity[band] = std::clamp(
                    (vocalBandEnvelopes_[band] - duckThreshold)
                        / (duckFullLevel - duckThreshold),
                    0.0,
                    1.0);
            }

            double detector = 0.0;
            for (Steinberg::int32 channel = 0; channel < channelCount; ++channel) {
                detector = std::max(detector, std::abs(static_cast<double>(input[channel][frame])));
            }
            const double envelopeCoefficient = detector > detectorEnvelope_ ? attack : release;
            detectorEnvelope_ = envelopeCoefficient * detectorEnvelope_
                + (1.0 - envelopeCoefficient) * detector;
            const double compressedLevel = detectorEnvelope_ > threshold
                ? threshold + (detectorEnvelope_ - threshold) / ratio
                : detectorEnvelope_;
            const double compressionGain = detectorEnvelope_ > 1e-12
                ? compressedLevel / detectorEnvelope_
                : 1.0;
            smoothedGain_ += (targetGain - smoothedGain_) * 0.001;

            for (Steinberg::int32 channel = 0; channel < channelCount; ++channel) {
                const auto channelIndex = static_cast<std::size_t>(
                    std::min<Steinberg::int32>(channel, 1));
                const Sample dry = input[channel][frame];
                Sample processed = dry;
                for (std::size_t filterIndex = 0; filterIndex < filters.size(); ++filterIndex) {
                    processed = runFilter(processed, filters[filterIndex],
                                          filterStates_[channelIndex][filterIndex]);
                }
                for (std::size_t band = 0; band < duckBands.size(); ++band) {
                    const Sample bandSignal = runFilter(
                        processed, duckBands[band], duckBandStates_[channelIndex][band]);
                    const double duckAmount = maxDuck * values[kVocalFocusId]
                        * vocalActivity[band];
                    processed = static_cast<Sample>(
                        processed - bandSignal * static_cast<Sample>(duckAmount));
                }
                processed = static_cast<Sample>(processed * compressionGain * smoothedGain_);
                if (saturation > 0.0001) {
                    processed = static_cast<Sample>(
                        std::tanh(static_cast<double>(processed) * saturationDrive) / saturationDrive);
                }
                const double magnitude = std::abs(static_cast<double>(processed));
                if (magnitude > ceiling) {
                    const double softened = ceiling + (1.0 - ceiling)
                        * (1.0 - std::exp(-(magnitude - ceiling) / (1.0 - ceiling)));
                    processed = static_cast<Sample>(std::copysign(softened, processed));
                }
                output[channel][frame] = processed;
            }
        }
    }

    void updateModeViews() {
        if (!modeControl_) {
            return;
        }
        const int mode = std::clamp(
            static_cast<int>(std::lround(modeControl_->getValue() * 2.0)), 0, 2);
        if (masterPanel_) {
            masterPanel_->setVisible(mode == 2);
        }
        if (vocalStatus_) {
            vocalStatus_->setVisible(mode == 0);
        }
        if (beatStatus_) {
            beatStatus_->setVisible(mode == 1);
        }
    }

    void updateEqGraph(VSTGUI::CControl* changedControl) {
        if (!eqGraphView_ || !changedControl) {
            return;
        }
        std::array<double, 5> gains{};
        for (std::size_t index = 0; index < eqControls_.size(); ++index) {
            if (eqControls_[index]) {
                gains[index] = eqControls_[index]->getValueNormalized() * 16.0 - 8.0;
            }
        }
        eqGraphView_->setBandGains(gains);
    }

    std::array<std::atomic<double>, kParameterCount> parameterValues_{};
    std::array<std::array<BiquadState, 7>, 2> filterStates_{};
    std::array<std::array<BiquadState, 2>, 2> sidechainDetectorStates_{};
    std::array<std::array<BiquadState, 2>, 2> duckBandStates_{};
    std::array<double, 2> vocalBandEnvelopes_{};
    std::array<std::atomic<double>, 5> autoEqDb_{};
    std::array<double, 5> smoothedAutoEqDb_{};
    std::atomic<double> autoCompressionRatio_{1.0};
    double smoothedAutoCompressionRatio_ = 1.0;
    AnalysisReadout analysisReadout_{};
    std::atomic<bool> analysisRequest_{false};
    std::array<BiquadCoefficients, 5> analysisFilters_{};
    std::array<std::array<std::array<BiquadState, 5>, 2>, 2> analysisBandStates_{};
    std::array<std::array<double, 5>, 2> analysisBandPower_{};
    std::array<double, 2> analysisPower_{};
    std::array<double, 2> analysisPeak_{};
    std::uint64_t analysisFramesTarget_ = 0;
    std::uint64_t analysisFramesRemaining_ = 0;
    std::uint64_t analysisFramesCaptured_ = 0;
    Steinberg::int32 analysisChannelCount_ = 2;
    Steinberg::int32 analysisSidechainChannelCount_ = 0;
    Steinberg::int32 channelCountForAnalysis_ = 2;
    VSTGUI::VST3Editor* editor_ = nullptr;
    VSTGUI::CControl* modeControl_ = nullptr;
    VSTGUI::CControl* analyzeControl_ = nullptr;
    VSTGUI::CViewContainer* masterPanel_ = nullptr;
    VSTGUI::CView* vocalStatus_ = nullptr;
    VSTGUI::CView* beatStatus_ = nullptr;
    SonoraEqGraph* eqGraphView_ = nullptr;
    SonoraAnalysisStatus* analysisStatusView_ = nullptr;
    std::array<VSTGUI::CControl*, 5> eqControls_{};
    double sampleRate_ = 44100.0;
    double detectorEnvelope_ = 0.0;
    double smoothedGain_ = 1.0;
};

Steinberg::tresult PLUGIN_API SonoraMaster::process(Steinberg::Vst::ProcessData& data) {
    if (auto* changes = data.inputParameterChanges) {
        const auto count = changes->getParameterCount();
        for (Steinberg::int32 index = 0; index < count; ++index) {
            auto* queue = changes->getParameterData(index);
            if (!queue || queue->getPointCount() == 0) {
                continue;
            }
            Steinberg::int32 sampleOffset = 0;
            Steinberg::Vst::ParamValue value = 0.0;
            if (queue->getPoint(queue->getPointCount() - 1, sampleOffset, value) == Steinberg::kResultTrue) {
                const auto id = queue->getParameterId();
                if (id < kParameterCount) {
                    parameterValues_[id].store(clampNormalized(value), std::memory_order_relaxed);
                    if (id == kAnalyzeId && value >= 0.5) {
                        analysisRequest_.store(true, std::memory_order_release);
                    }
                }
            }
        }
    }

    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0) {
        return Steinberg::kResultOk;
    }

    const auto modeValue = parameterValues_[kModeId].load(std::memory_order_relaxed);
    const int mode = std::clamp(static_cast<int>(std::lround(modeValue * 2.0)), 0, 2);
    const double amount = parameterValues_[kAmountId].load(std::memory_order_relaxed);
    const bool bypass = parameterValues_[kBypassId].load(std::memory_order_relaxed) >= 0.5;
    const auto channels = std::min(data.inputs[0].numChannels, data.outputs[0].numChannels);
    std::array<double, kParameterCount> values{};
    for (Steinberg::Vst::ParamID id = 0; id < kParameterCount; ++id) {
        values[id] = parameterValues_[id].load(std::memory_order_relaxed);
    }
    const bool sidechainActive = data.numInputs > 1
        && data.inputs[1].numChannels > 0;
    const auto sidechainChannels = sidechainActive ? data.inputs[1].numChannels : 0;

    if (data.symbolicSampleSize == Steinberg::Vst::kSample32) {
        float** sidechain = sidechainActive ? data.inputs[1].channelBuffers32 : nullptr;
        processBlock(data.inputs[0].channelBuffers32, data.outputs[0].channelBuffers32,
                     channels, sidechain, sidechainChannels,
                     data.numSamples, mode, values, bypass);
    } else if (data.symbolicSampleSize == Steinberg::Vst::kSample64) {
        double** sidechain = sidechainActive ? data.inputs[1].channelBuffers64 : nullptr;
        processBlock(data.inputs[0].channelBuffers64, data.outputs[0].channelBuffers64,
                     channels, sidechain, sidechainChannels,
                     data.numSamples, mode, values, bypass);
    }

    return Steinberg::kResultOk;
}

BEGIN_FACTORY_DEF("SONORA Audio", "https://github.com/simon-of-course/sonora-mastering", "")

DEF_CLASS2(
    INLINE_UID(0x7B628C23, 0xBB614E25, 0x98B04D14, 0x23AC1F91),
    Steinberg::PClassInfo::kManyInstances,
    kVstAudioEffectClass,
    "SONORA Master",
    0,
    "Fx|Mastering",
    "0.1.0",
    kVstVersionString,
    SonoraMaster::createInstance)

END_FACTORY
