#pragma once

// =================================================================================================
// SONORA parameter text formatting (controller side, VST3 SDK).
//
// VST3Editor fills every CParamDisplay / option menu / segment button from
// EditController::getParamStringByValue -> Parameter::toString. A plain RangeParameter prints a
// bare number ("1.5"), so SONORA registers continuous parameters as FormattedParameter to get
// German, unit-aware texts such as "+1,5 dB", "50 %", "120 Hz", "Aus", "0,05 s".
//
//   using sonora::ui::FormattedParameter;
//   using sonora::ui::ParamFormat;
//   parameters.addParameter(new FormattedParameter(STR16("Bass"), kBass, -6.0, 6.0, 0.0,
//                                                  ParamFormat::db(1, true)));
//   parameters.addParameter(new FormattedParameter(STR16("Bass-Mono"), kBassMono, 0.0, 250.0, 0.0,
//                                                  ParamFormat::hz(0).withZeroText("Aus")));
//   parameters.addParameter(new FormattedParameter(STR16("Abtauchzeit"), kDive, 0.01, 2.0, 0.05,
//                                                  ParamFormat::seconds(2).withLog()));
// List parameters (Bus-Modus, Preset, Notenwert, Form, ...) stay StringListParameter.
// =================================================================================================

#include "public.sdk/source/vst/vstparameters.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace sonora::ui {

struct ParamFormat {
    int decimals = 0;
    const char* unit = "";      // UTF-8, appended with a (narrow) space if non-empty
    bool showPlus = false;      // "+1,5 dB" for bipolar parameters
    const char* zeroText = nullptr; // e.g. "Aus" when the plain value is 0
    double scale = 1.0;         // display = plain * scale (e.g. 100 for 0..1 -> %)
    bool logarithmic = false;   // plain = min * (max/min)^normalized (min > 0), e.g. Abtauchzeit

    static ParamFormat make(int d, const char* u, bool plus = false) {
        ParamFormat f;
        f.decimals = d;
        f.unit = u;
        f.showPlus = plus;
        return f;
    }
    static ParamFormat db(int d = 1, bool plus = true) { return make(d, "dB", plus); }
    static ParamFormat percent(int d = 0) { return make(d, "%"); }
    static ParamFormat hz(int d = 0) { return make(d, "Hz"); }
    static ParamFormat ms(int d = 1) { return make(d, "ms"); }
    static ParamFormat seconds(int d = 2) { return make(d, "s"); }
    static ParamFormat dbtp(int d = 1) { return make(d, "dBTP"); }
    ParamFormat withZeroText(const char* t) const { ParamFormat f = *this; f.zeroText = t; return f; }
    ParamFormat withScale(double s) const { ParamFormat f = *this; f.scale = s; return f; }
    ParamFormat withLog() const { ParamFormat f = *this; f.logarithmic = true; return f; }

    std::string format(double plain) const {
        const double v = plain * scale;
        const double rounded = std::round(v * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
        if (zeroText && std::fabs(rounded) < 1e-9) return zeroText;
        char buf[64];
        std::snprintf(buf, sizeof(buf), (showPlus && rounded > 0.0) ? "+%.*f" : "%.*f", decimals,
                      rounded == 0.0 ? 0.0 : rounded);
        std::string s(buf);
        for (auto& c : s)
            if (c == '.') c = ',';
        if (unit && *unit) {
            s += ' ';
            s += unit;
        }
        return s;
    }
};

class FormattedParameter : public Steinberg::Vst::RangeParameter {
public:
    FormattedParameter(const Steinberg::Vst::TChar* title, Steinberg::Vst::ParamID tag, double minValue,
                       double maxValue, double defaultPlain, ParamFormat fmt,
                       Steinberg::int32 flags = Steinberg::Vst::ParameterInfo::kCanAutomate,
                       Steinberg::Vst::UnitID unitID = Steinberg::Vst::kRootUnitId)
    : RangeParameter(title, tag, nullptr, minValue, maxValue, defaultPlain,
                     Steinberg::Vst::kStepCountContinuous, flags, unitID),
      fmt_(fmt) {
        // RangeParameter stored a linear default; recompute it for logarithmic mappings.
        if (fmt_.logarithmic) setNormalized(info.defaultNormalizedValue = toNormalized(defaultPlain));
        // Expose the unit to hosts that show it separately.
        if (fmt.unit && *fmt.unit) utf8ToUtf16(fmt.unit, info.units, 128);
    }

    Steinberg::Vst::ParamValue toPlain(Steinberg::Vst::ParamValue normalized) const SMTG_OVERRIDE {
        if (!fmt_.logarithmic || getMin() <= 0.0) return RangeParameter::toPlain(normalized);
        const double n = normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized);
        return getMin() * std::pow(getMax() / getMin(), n);
    }

    Steinberg::Vst::ParamValue toNormalized(Steinberg::Vst::ParamValue plain) const SMTG_OVERRIDE {
        if (!fmt_.logarithmic || getMin() <= 0.0) return RangeParameter::toNormalized(plain);
        const double p = plain < getMin() ? getMin() : (plain > getMax() ? getMax() : plain);
        return std::log(p / getMin()) / std::log(getMax() / getMin());
    }

    void toString(Steinberg::Vst::ParamValue normalized, Steinberg::Vst::String128 string) const SMTG_OVERRIDE {
        utf8ToUtf16(fmt_.format(toPlain(normalized)).c_str(), string, 128);
    }

    bool fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& normalized) const SMTG_OVERRIDE {
        std::string ascii;
        for (int i = 0; i < 127 && string[i]; ++i) {
            const auto c = string[i];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+') ascii += static_cast<char>(c);
            else if (c == ',' || c == '.') ascii += '.';
            else if (!ascii.empty() && c != ' ') break;
        }
        if (ascii.empty()) {
            if (fmt_.zeroText && string[0] && (string[0] == fmt_.zeroText[0] || string[0] == (fmt_.zeroText[0] | 0x20))) {
                normalized = toNormalized(0.0);
                return true;
            }
            return false;
        }
        char* end = nullptr;
        const double v = std::strtod(ascii.c_str(), &end);
        if (end == ascii.c_str()) return false;
        normalized = toNormalized(fmt_.scale != 0.0 ? v / fmt_.scale : v);
        if (normalized < 0.0) normalized = 0.0;
        if (normalized > 1.0) normalized = 1.0;
        return true;
    }

    OBJ_METHODS(FormattedParameter, RangeParameter)

    static void utf8ToUtf16(const char* in, Steinberg::Vst::TChar* out, int capacity) {
        int o = 0;
        const auto* s = reinterpret_cast<const unsigned char*>(in);
        while (*s && o < capacity - 1) {
            unsigned cp = *s;
            if (cp >= 0xF0 && s[1] && s[2] && s[3]) {
                cp = ((cp & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
                s += 4;
            } else if (cp >= 0xE0 && s[1] && s[2]) {
                cp = ((cp & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
                s += 3;
            } else if (cp >= 0xC0 && s[1]) {
                cp = ((cp & 0x1Fu) << 6) | (s[1] & 0x3Fu);
                s += 2;
            } else {
                ++s;
            }
            if (cp > 0xFFFF) {
                if (o >= capacity - 2) break;
                cp -= 0x10000;
                out[o++] = static_cast<Steinberg::Vst::TChar>(0xD800 + (cp >> 10));
                out[o++] = static_cast<Steinberg::Vst::TChar>(0xDC00 + (cp & 0x3FF));
            } else {
                out[o++] = static_cast<Steinberg::Vst::TChar>(cp);
            }
        }
        out[o] = 0;
    }

private:
    ParamFormat fmt_;
};

} // namespace sonora::ui
