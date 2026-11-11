// sonora_render - offline renderer for the SONORA Master engine (SDK-free).
//
//   sonora_render in.wav out.wav [--mode vocal|beat|master] [--preset "Trap 808"|index]
//                 [--param id=value ...] [--sidechain vocal.wav] [--block N] [--analyze] [--bits 16|24|32]
//   sonora_render --demo outdir [--seconds S]
//
// --param values are plain units (e.g. 4=-1.5 for Ausgang -1.5 dB); append 'n' for a normalized
// 0..1 value (e.g. 1=0.75n). Parameters are applied after --mode/--preset, in command-line order.
// --analyze runs the 8 s analysis on the start of the file first and renders with its auto-EQ.
// --demo writes the deterministic synthetic rap test mix (mix, beat, vocal) and renders of every
// preset (Master mode, Stärke 100 %) to outdir and prints a metrics table.
//
// Build: g++ -std=c++17 -O2 -I plugin/src -I plugin/src/dsp plugin/tools/sonora_render.cpp -o sonora_render

#include "../tests/test_util.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

using sonora::test::Audio;
namespace M = sonora::dsp::master;

void usage() {
    std::fprintf(stderr,
                 "usage: sonora_render in.wav out.wav [--mode vocal|beat|master] [--preset NAME|INDEX]\n"
                 "                     [--param id=value[n] ...] [--sidechain sc.wav] [--block N] [--analyze]\n"
                 "                     [--bits 16|24|32]\n"
                 "       sonora_render --demo outdir [--seconds S]\n"
                 "presets:");
    for (int i = 0; i < M::kNumPresets; ++i) std::fprintf(stderr, " %d=\"%s\"", i, M::kPresets[static_cast<std::size_t>(i)].name);
    std::fprintf(stderr, "\nparams:");
    for (int i = 0; i < M::kNumParams; ++i) {
        const auto& p = M::kParams[static_cast<std::size_t>(i)];
        std::fprintf(stderr, "%s %d=%s [%g..%g %s]", i % 3 == 0 ? "\n " : "", i, p.name, p.minValue, p.maxValue, p.unit);
    }
    std::fprintf(stderr, "\n");
}

void printHeader() {
    std::printf("%-16s %7s %7s %6s %6s | %6s %6s %6s %6s %6s  (bands dB rel. to LUFS)\n", "", "LUFS-I", "TP", "PLR",
                "crest", "25-60", "60-100", "1-3H", "1-4k", "6-16k");
}

void printMetrics(const char* label, const Audio& a) {
    const auto m = sonora::test::measureMix(a);
    std::printf("%-16s %7.2f %7.2f %6.2f %6.2f | %6.1f %6.1f %6.1f %6.1f %6.1f\n", label, m.lufs, m.tp, m.plr, m.crest,
                m.sub - m.lufs, m.low - m.lufs, m.harm - m.lufs, m.mid - m.lufs, m.high - m.lufs);
}

bool parseMode(const std::string& s, int& mode) {
    if (s == "vocal" || s == "0") mode = M::kModeVocal;
    else if (s == "beat" || s == "1") mode = M::kModeBeat;
    else if (s == "master" || s == "2") mode = M::kModeMaster;
    else return false;
    return true;
}

bool parsePreset(const std::string& s, int& preset) {
    preset = sonora::test::presetIndexByName(s);
    if (preset >= 0) return true;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end && *end == '\0' && v >= 0 && v < M::kNumPresets) {
        preset = static_cast<int>(v);
        return true;
    }
    return false;
}

struct ParamSetting {
    int id;
    double value;
    bool normalized;
};

bool parseParam(const std::string& s, ParamSetting& out) {
    const auto eq = s.find('=');
    if (eq == std::string::npos) return false;
    char* end = nullptr;
    const long id = std::strtol(s.substr(0, eq).c_str(), &end, 10);
    if (!end || *end != '\0' || id < 0 || id >= M::kNumParams) return false;
    std::string v = s.substr(eq + 1);
    out.normalized = !v.empty() && (v.back() == 'n' || v.back() == 'N');
    if (out.normalized) v.pop_back();
    const double value = std::strtod(v.c_str(), &end);
    if (v.empty() || !end || *end != '\0' || !std::isfinite(value)) return false;
    out.id = static_cast<int>(id);
    out.value = value;
    return true;
}

void configure(sonora::dsp::MasterEngine& e, int mode, int preset, const std::vector<ParamSetting>& params) {
    if (mode >= 0) e.setParameterPlain(M::kMode, mode);
    if (preset >= 0) e.setParameterPlain(M::kPreset, preset);
    for (const auto& p : params) {
        if (p.normalized) e.setParameter(p.id, p.value);
        else e.setParameterPlain(p.id, p.value);
    }
}

const char* analysisStatusName(int s) {
    switch (s) {
    case M::kAnalysisReady: return "ready (not run)";
    case M::kAnalysisRunning: return "running (input shorter than 8 s?)";
    case M::kAnalysisComplete: return "complete";
    case M::kAnalysisNoVocal: return "no vocal detected";
    case M::kAnalysisMissingSidechain: return "missing sidechain";
    case M::kAnalysisWrongMode: return "not available in Vocal mode";
    case M::kAnalysisTooQuiet: return "signal too quiet";
    case M::kAnalysisCompleteMixOnly: return "complete (mix only, no sidechain)";
    default: return "?";
    }
}

int runDemo(const std::string& dir, double seconds) {
    using namespace sonora::test;
    const RapMix mix = generateRapMix(48000.0, seconds, 7);
    auto path = [&](const std::string& name) { return dir + "/" + name; };
    if (!writeWav(path("00_input_mix.wav"), mix.mix, 24) || !writeWav(path("00_input_beat.wav"), mix.beat, 24)
        || !writeWav(path("00_input_vocal.wav"), mix.vocal, 24)) {
        std::fprintf(stderr, "cannot write to %s (does the directory exist?)\n", dir.c_str());
        return 1;
    }
    printHeader();
    printMetrics("(input)", mix.mix);
    for (int p = 0; p < M::kNumPresets; ++p) {
        sonora::dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        applyPreset(e, p, 100.0);
        e.reset();
        const Audio out = renderEngine(e, mix.mix, nullptr, 512);
        std::string name = M::kPresets[static_cast<std::size_t>(p)].name;
        for (auto& c : name) c = c == ' ' ? '_' : c;
        char file[128];
        std::snprintf(file, sizeof file, "%02d_%s.wav", p + 1, name.c_str());
        if (!writeWav(path(file), out, 24)) {
            std::fprintf(stderr, "cannot write %s\n", path(file).c_str());
            return 1;
        }
        printMetrics(M::kPresets[static_cast<std::size_t>(p)].name, out);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace sonora::test;
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string in, out, scPath, demoDir;
    int mode = -1, preset = -1, block = 512, bits = 24;
    double seconds = 16.0;
    bool analyze = false;
    std::vector<ParamSetting> params;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= args.size()) return false;
            v = args[++i];
            return true;
        };
        std::string v;
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--demo") { if (!next(demoDir)) { usage(); return 2; } }
        else if (a == "--seconds") { if (!next(v) || (seconds = std::atof(v.c_str())) < 1.0) { usage(); return 2; } }
        else if (a == "--mode") { if (!next(v) || !parseMode(v, mode)) { std::fprintf(stderr, "bad --mode\n"); return 2; } }
        else if (a == "--preset") { if (!next(v) || !parsePreset(v, preset)) { std::fprintf(stderr, "unknown preset\n"); usage(); return 2; } }
        else if (a == "--param") {
            ParamSetting p{};
            if (!next(v) || !parseParam(v, p)) { std::fprintf(stderr, "bad --param (expected id=value or id=valuen)\n"); return 2; }
            params.push_back(p);
        }
        else if (a == "--sidechain") { if (!next(scPath)) { usage(); return 2; } }
        else if (a == "--block") { if (!next(v) || (block = std::atoi(v.c_str())) < 1 || block > 65536) { std::fprintf(stderr, "bad --block\n"); return 2; } }
        else if (a == "--bits") { if (!next(v) || ((bits = std::atoi(v.c_str())) != 16 && bits != 24 && bits != 32)) { std::fprintf(stderr, "bad --bits\n"); return 2; } }
        else if (a == "--analyze") analyze = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
        else if (in.empty()) in = a;
        else if (out.empty()) out = a;
        else { usage(); return 2; }
    }
    if (!demoDir.empty()) return runDemo(demoDir, seconds);
    if (in.empty() || out.empty()) { usage(); return 2; }

    Audio input, sc;
    std::string err;
    if (!readWav(in, input, &err)) { std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str()); return 1; }
    if (input.channels() < 1 || input.channels() > 2) { std::fprintf(stderr, "only mono/stereo input is supported\n"); return 1; }
    const bool hasSc = !scPath.empty();
    if (hasSc) {
        if (!readWav(scPath, sc, &err)) { std::fprintf(stderr, "%s: %s\n", scPath.c_str(), err.c_str()); return 1; }
        if (sc.channels() > 2) sc.ch.resize(2);
        if (sc.sampleRate != input.sampleRate) std::fprintf(stderr, "warning: sidechain sample rate differs from input\n");
    }

    sonora::dsp::MasterEngine e;
    e.prepare(input.sampleRate, block);
    configure(e, mode, preset, params);
    e.reset();
    if (analyze) {
        e.requestAnalysis();
        (void)renderEngine(e, input, hasSc ? &sc : nullptr, block);
        const auto& m = e.meters();
        std::printf("analysis: %s, auto-EQ [%.2f %.2f %.2f %.2f %.2f] dB, ratio %.3f\n", analysisStatusName(m.analysisStatus),
                    m.autoEqDb[0], m.autoEqDb[1], m.autoEqDb[2], m.autoEqDb[3], m.autoEqDb[4], m.autoCompRatio);
        double eq[M::kBands];
        for (int b = 0; b < M::kBands; ++b) eq[b] = m.autoEqDb[b];
        const double ratio = m.autoCompRatio;
        e.prepare(input.sampleRate, block);
        configure(e, mode, preset, params);
        e.setAutoEq(eq, ratio);
        e.reset();
    }
    const Audio output = renderEngine(e, input, hasSc ? &sc : nullptr, block);
    if (!writeWav(out, output, bits)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }

    const int modeNow = static_cast<int>(e.getParameterPlain(M::kMode));
    const int presetNow = static_cast<int>(e.getParameterPlain(M::kPreset));
    std::printf("%s -> %s  (%.0f Hz, %d ch, %.2f s, mode %s, preset \"%s\", Stärke %.0f %%, block %d, latency %d)\n",
                in.c_str(), out.c_str(), input.sampleRate, input.channels(), input.frames() / input.sampleRate,
                modeNow == M::kModeVocal ? "vocal" : modeNow == M::kModeBeat ? "beat" : "master",
                M::kPresets[static_cast<std::size_t>(presetNow)].name, e.getParameterPlain(M::kAmount), block,
                e.latencySamples());
    printHeader();
    printMetrics("input", input);
    printMetrics("output", output);
    const auto& m = e.meters();
    std::printf("final meters: glue GR %.1f dB, limiter GR %.1f dB, clip activity %.2f, correlation %.2f, TP hold %.2f dBTP\n",
                m.glueGrDb, m.limiterGrDb, m.clipActivity, m.correlation, m.truePeakMaxDb);
    return allFinite(output) ? 0 : 3;
}
