// sonora_hostcheck — loads the built SONORA .vst3 bundles like a host does and checks the glue
// that the SDK validator cannot know about:
//   * v0.1 state migration (Master v1/v4, Shutter v1, Drowner v1) onto the v0.2 parameter set
//   * v0.2 state round trip into a fresh instance
//   * latency report, 32/64-bit processing, finite output, ceiling, sample-accurate automation
//
// Usage: sonora_hostcheck <SONORA_Master.vst3> <SONORA_Shutter.vst3> <SONORA_Drowner.vst3>
// Exit code 0 = all checks passed.

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "base/source/fstreamer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int gFailures = 0;
int gChecks = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        ++gChecks;                                                                                 \
        if (!(cond)) {                                                                             \
            ++gFailures;                                                                           \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                     \
            std::printf(__VA_ARGS__);                                                              \
            std::printf("\n");                                                                     \
        }                                                                                          \
    } while (0)

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

struct Instance {
    IPtr<IComponent> component;
    IPtr<IAudioProcessor> processor;
    IPtr<IEditController> controller;

    ~Instance() {
        if (component) {
            component->setActive(false);
            component->terminate();
        }
    }
    ParamValue get(ParamID id) const { return controller->getParamNormalized(id); }
};

HostApplication* hostContext() {
    static HostApplication* host = new HostApplication();
    return host;
}

bool create(const VST3::Hosting::Module::Ptr& module, Instance& inst) {
    for (const auto& ci : module->getFactory().classInfos()) {
        if (ci.category() != kVstAudioEffectClass) continue;
        inst.component = module->getFactory().createInstance<IComponent>(ci.ID());
        if (!inst.component) return false;
        if (inst.component->initialize(hostContext()) != kResultOk) return false;
        inst.processor = U::cast<IAudioProcessor>(inst.component);
        inst.controller = U::cast<IEditController>(inst.component);
        return inst.processor && inst.controller;
    }
    return false;
}

// Writes a state blob with the given version; `withCount` adds the v0.2 parameter-count field.
IPtr<MemoryStream> makeState(int32 version, const std::vector<double>& values, bool withCount) {
    auto stream = owned(new MemoryStream());
    IBStreamer s(stream, kLittleEndian);
    s.writeInt32(version);
    if (withCount) s.writeInt32(static_cast<int32>(values.size()));
    for (double v : values) s.writeDouble(v);
    stream->seek(0, IBStream::kIBSeekSet, nullptr);
    return stream;
}

bool loadState(Instance& inst, IBStream* stream) {
    stream->seek(0, IBStream::kIBSeekSet, nullptr);
    if (inst.component->setState(stream) != kResultOk) return false;
    stream->seek(0, IBStream::kIBSeekSet, nullptr);
    return inst.controller->setComponentState(stream) == kResultOk;
}

void checkRoundTrip(const VST3::Hosting::Module::Ptr& module, Instance& a, const char* name) {
    auto blob = owned(new MemoryStream());
    CHECK(a.component->getState(blob) == kResultOk, "%s getState", name);
    Instance b;
    CHECK(create(module, b), "%s second instance", name);
    CHECK(loadState(b, blob), "%s setState(v0.2 blob)", name);
    const int32 n = a.controller->getParameterCount();
    CHECK(n == b.controller->getParameterCount(), "%s parameter count", name);
    for (int32 i = 0; i < n; ++i) {
        ParameterInfo info{};
        a.controller->getParameterInfo(i, info);
        CHECK(near(a.get(info.id), b.get(info.id)), "%s round trip id %u: %.6f vs %.6f", name, info.id,
              a.get(info.id), b.get(info.id));
    }
}

struct RunResult {
    bool finite = true;
    double inPeak = 0.0, outPeak = 0.0, inRms = 0.0, outRms = 0.0;
    std::vector<double> outL;
};

// Processes `seconds` of a rap-ish test signal (kick-like 55 Hz bursts + noise) in blocks.
// `automation(blockIndex, changes)` may add parameter points per block.
template <typename Sample>
RunResult run(Instance& inst, double sr, int32 block, double seconds, double gain,
              const std::function<void(int, ParameterChanges&)>& automation = {}) {
    ProcessSetup setup{kRealtime, sizeof(Sample) == 8 ? kSample64 : kSample32, block, sr};
    RunResult r;
    if (inst.processor->setupProcessing(setup) != kResultOk) {
        r.finite = false;
        return r;
    }
    inst.component->setActive(true);
    inst.processor->setProcessing(true);

    std::vector<Sample> inL(block), inR(block), outLb(block), outRb(block);
    Sample* inPtr[2] = {inL.data(), inR.data()};
    Sample* outPtr[2] = {outLb.data(), outRb.data()};
    AudioBusBuffers inBus{}, outBus{};
    inBus.numChannels = outBus.numChannels = 2;
    if constexpr (sizeof(Sample) == 8) {
        inBus.channelBuffers64 = inPtr;
        outBus.channelBuffers64 = outPtr;
    } else {
        inBus.channelBuffers32 = inPtr;
        outBus.channelBuffers32 = outPtr;
    }
    ProcessContext ctx{};
    ctx.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
    ctx.tempo = 140.0;
    ctx.sampleRate = sr;

    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 1.0);
    const int64 total = static_cast<int64>(seconds * sr);
    double inSq = 0.0, outSq = 0.0;
    int64 t = 0;
    for (int bi = 0; t < total; ++bi) {
        const int32 n = static_cast<int32>(std::min<int64>(block, total - t));
        for (int32 i = 0; i < n; ++i) {
            const double time = static_cast<double>(t + i) / sr;
            const double beat = std::fmod(time, 60.0 / 140.0);
            const double kick = std::sin(2.0 * M_PI * 55.0 * time) * std::exp(-beat * 6.0);
            const double l = gain * (0.7 * kick + 0.15 * noise(rng));
            const double rr = gain * (0.7 * kick + 0.15 * noise(rng));
            inL[i] = static_cast<Sample>(l);
            inR[i] = static_cast<Sample>(rr);
            r.inPeak = std::max({r.inPeak, std::fabs(l), std::fabs(rr)});
            inSq += l * l + rr * rr;
        }
        ParameterChanges changes(8);
        if (automation) automation(bi, changes);
        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = setup.symbolicSampleSize;
        data.numSamples = n;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;
        data.inputParameterChanges = &changes;
        ctx.projectTimeSamples = t;
        ctx.projectTimeMusic = static_cast<double>(t) / sr * ctx.tempo / 60.0;
        data.processContext = &ctx;
        if (inst.processor->process(data) != kResultOk) r.finite = false;
        for (int32 i = 0; i < n; ++i) {
            for (int ch = 0; ch < 2; ++ch) {
                const double v = static_cast<double>(outPtr[ch][i]);
                if (!std::isfinite(v)) r.finite = false;
                r.outPeak = std::max(r.outPeak, std::fabs(v));
                outSq += v * v;
            }
            r.outL.push_back(static_cast<double>(outLb[i]));
        }
        t += n;
    }
    inst.processor->setProcessing(false);
    inst.component->setActive(false);
    r.inRms = std::sqrt(inSq / (2.0 * static_cast<double>(total)));
    r.outRms = std::sqrt(outSq / (2.0 * static_cast<double>(total)));
    return r;
}

double db(double x) { return 20.0 * std::log10(std::max(x, 1e-12)); }

// ------------------------------------------------------------------------------------------------
// Master (IDs as in plugin/src/dsp/master_engine.h)
// ------------------------------------------------------------------------------------------------
void checkMaster(const VST3::Hosting::Module::Ptr& module) {
    std::printf("SONORA Master\n");
    enum : ParamID { kMode = 0, kAmount = 1, kBass = 2, kPresence = 3, kOutput = 4, kBypass = 5, kPreset = 6,
                     kNasal = 7, kResonance = 8, kBeatHighs = 9, kSaturation = 10, kEq60 = 11, kFocus = 16,
                     kAnalyze = 17, kLoudness = 18, kCeiling = 19, kStereo = 24, kInput = 26 };
    Instance inst;
    if (!create(module, inst)) {
        CHECK(false, "create Master");
        return;
    }
    CHECK(inst.controller->getParameterCount() == 27, "Master has 27 parameters (got %d)",
          inst.controller->getParameterCount());

    // v4 (v0.1, 18 doubles): Stärke 50 %, Bass +4 dB, Präsenz +2 dB, Nasalität 40 %, Resonanzen 20 %,
    // Beat-Höhen -4 dB, Preset "Air" (index 3 of 6), Analyse 1.
    std::vector<double> v4 = {1.0, 0.5, 10.0 / 12.0, 6.0 / 8.0, 0.5, 0.0, 0.6, 0.4, 0.2, 2.0 / 12.0, 0.3,
                              0.5, 0.5, 0.5, 0.5, 0.5, 0.7, 1.0};
    auto s4 = makeState(4, v4, false);
    CHECK(loadState(inst, s4), "Master accepts v4 state");
    CHECK(near(inst.get(kMode), 1.0), "v4 mode kept");
    CHECK(near(inst.get(kAmount), 0.5), "v4 Stärke kept");
    CHECK(near(inst.get(kBass), 8.0 / 12.0), "v4 Bass +4 dB * 50 %% -> +2 dB (%.4f)", inst.get(kBass));
    CHECK(near(inst.get(kPresence), 5.0 / 8.0), "v4 Präsenz +2 dB * 50 %% -> +1 dB (%.4f)", inst.get(kPresence));
    CHECK(near(inst.get(kNasal), 0.2), "v4 Nasalität 40 %% -> 20 %% (%.4f)", inst.get(kNasal));
    CHECK(near(inst.get(kResonance), 0.1), "v4 Resonanzen 20 %% -> 10 %% (%.4f)", inst.get(kResonance));
    CHECK(near(inst.get(kBeatHighs), 4.0 / 12.0), "v4 Beat-Höhen -4 dB -> -2 dB (%.4f)", inst.get(kBeatHighs));
    CHECK(near(inst.get(kSaturation), 0.3), "v4 Sättigung kept");
    CHECK(near(inst.get(kPreset), 3.0 / 11.0), "v4 preset Air -> index 3 of 12 (%.4f)", inst.get(kPreset));
    CHECK(near(inst.get(kFocus), 0.7), "v4 Vocal-Fokus kept");
    CHECK(near(inst.get(kAnalyze), 0.0), "v4 Analyse not restored");
    CHECK(near(inst.get(kCeiling), 2.0 / 3.0), "new Ceiling default -1 dBTP (%.4f)", inst.get(kCeiling));
    CHECK(near(inst.get(kStereo), 0.5), "new Stereo-Breite default 100 %%");
    CHECK(near(inst.get(kInput), 0.5), "new Eingang default 0 dB");

    // v1 (6 doubles): Stärke 80 %, Bass -6 dB, Präsenz +4 dB, Preset index 5 (Vocal Clarity).
    Instance v1i;
    CHECK(create(module, v1i), "create Master (v1)");
    auto s1 = makeState(1, {0.5, 0.8, 0.0, 1.0, 0.5, 0.0}, false);
    s1->seek(0, IBStream::kIBSeekSet, nullptr);
    CHECK(loadState(v1i, s1), "Master accepts v1 state");
    CHECK(near(v1i.get(kBass), (-4.8 + 6.0) / 12.0), "v1 Bass -6 dB * 80 %% (%.4f)", v1i.get(kBass));
    CHECK(near(v1i.get(kPresence), (3.2 + 4.0) / 8.0), "v1 Präsenz +4 dB * 80 %% (%.4f)", v1i.get(kPresence));
    CHECK(near(v1i.get(kPreset), 0.0), "v1 preset 0 kept");
    CHECK(near(v1i.get(kNasal), 0.0), "v1 missing Nasalität -> default");

    // Garbage / future versions must not crash; unknown trailing data is tolerated.
    Instance g;
    CHECK(create(module, g), "create Master (garbage)");
    auto bad = makeState(5, {std::nan(""), 2.0, -1.0}, true);
    CHECK(loadState(g, bad), "Master tolerates short v5 state");
    CHECK(near(g.get(kAmount), 1.0) && near(g.get(kBass), 0.0), "v5 values clamped (%.3f %.3f)", g.get(kAmount),
          g.get(kBass));

    checkRoundTrip(module, inst, "Master");

    // Processing: Master mode, preset Trap 808, Lautheit default, ceiling -1 dBTP.
    Instance p;
    CHECK(create(module, p), "create Master (process)");
    p.controller->setParamNormalized(kMode, 1.0);
    p.controller->setParamNormalized(kPreset, 6.0 / 11.0);
    p.controller->setParamNormalized(kAmount, 1.0);
    p.controller->setParamNormalized(kLoudness, 8.0 / 18.0);
    auto r32 = run<float>(p, 48000.0, 512, 6.0, 0.25);
    CHECK(p.processor->getLatencySamples() == 135, "latency 135 @48k (got %u)", p.processor->getLatencySamples());
    CHECK(r32.finite, "32-bit output finite");
    CHECK(db(r32.outPeak) <= -0.85, "32-bit sample peak %.2f dBFS <= ceiling", db(r32.outPeak));
    CHECK(db(r32.outRms) > db(r32.inRms) + 3.0, "Trap 808 + Lautheit louder: in %.1f dB out %.1f dB RMS",
          db(r32.inRms), db(r32.outRms));
    std::printf("  Trap 808 @ +8 dB Lautheit: in RMS %.1f dBFS -> out RMS %.1f dBFS, out peak %.2f dBFS\n",
                db(r32.inRms), db(r32.outRms), db(r32.outPeak));

    auto r64 = run<double>(p, 44100.0, 333, 2.0, 0.25);
    CHECK(r64.finite, "64-bit output finite");
    CHECK(p.processor->getLatencySamples() == 130, "latency 130 @44.1k (got %u)", p.processor->getLatencySamples());

    // Hot input (+12 dB over full scale) must still respect the ceiling.
    auto hot = run<float>(p, 48000.0, 64, 3.0, 4.0);
    CHECK(hot.finite && db(hot.outPeak) <= -0.85, "hot input peak %.2f dBFS", db(hot.outPeak));

    // Sample-accurate bypass: bypass engages at sample 1000 of block 2 -> after the 10 ms crossfade the
    // output equals the input delayed by the (constant) latency.
    Instance b;
    CHECK(create(module, b), "create Master (bypass)");
    b.controller->setParamNormalized(kMode, 1.0);
    b.controller->setParamNormalized(kPreset, 11.0 / 11.0);
    auto ref = run<double>(b, 48000.0, 2048, 1.0, 0.25, [](int bi, ParameterChanges& ch) {
        if (bi != 2) return;
        int32 idx = 0;
        if (auto* q = ch.addParameterData(kBypass, idx)) q->addPoint(1000, 1.0, idx);
    });
    auto dry = run<double>(b, 48000.0, 2048, 1.0, 0.25); // still bypassed now (last value stuck at 1)
    double maxDiff = 0.0;
    for (size_t i = 2 * 2048 + 1000 + 135 + 960; i < ref.outL.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(ref.outL[i] - dry.outL[i]));
    CHECK(maxDiff < 1e-9, "bypass at sample offset lands exactly (max diff %.3g)", maxDiff);
    double pre = 0.0;
    for (size_t i = 2048; i < 2 * 2048 + 1000 + 135 - 1 && i < ref.outL.size(); ++i)
        pre = std::max(pre, std::fabs(ref.outL[i] - dry.outL[i]));
    CHECK(pre > 1e-3, "processing active before the bypass point (diff %.3g)", pre);
}

// ------------------------------------------------------------------------------------------------
// Shutter / Drowner
// ------------------------------------------------------------------------------------------------
void checkShutter(const VST3::Hosting::Module::Ptr& module) {
    std::printf("SONORA Shutter\n");
    Instance inst;
    if (!create(module, inst)) {
        CHECK(false, "create Shutter");
        return;
    }
    CHECK(inst.controller->getParameterCount() == 14, "Shutter has 14 parameters (got %d)",
          inst.controller->getParameterCount());
    // v1: aktiv, sync, Notenwert "1/16 T" (index 7 of 9), Rate, Tiefe, Mix, Glättung.
    auto s1 = makeState(1, {1.0, 1.0, 7.0 / 8.0, 0.25, 0.6, 0.9, 0.5}, false);
    CHECK(loadState(inst, s1), "Shutter accepts v1 state");
    CHECK(near(inst.get(0), 1.0) && near(inst.get(1), 1.0), "v1 aktiv/sync kept");
    CHECK(near(inst.get(2), 7.0 / 11.0), "v1 Notenwert 1/16 T -> index 7 of 12 (%.4f)", inst.get(2));
    CHECK(near(inst.get(3), 0.25) && near(inst.get(4), 0.6) && near(inst.get(5), 0.9) && near(inst.get(6), 0.5),
          "v1 Rate/Tiefe/Mix/Glättung kept");
    CHECK(near(inst.get(7), 0.5), "new Pulsbreite default 50 %%");
    checkRoundTrip(module, inst, "Shutter");
    auto r = run<float>(inst, 48000.0, 256, 2.0, 0.25);
    CHECK(r.finite, "Shutter output finite");
    CHECK(r.outRms < r.inRms * 0.9, "Shutter gates (in %.1f out %.1f dB)", db(r.inRms), db(r.outRms));
    auto r64 = run<double>(inst, 96000.0, 1000, 1.0, 0.25);
    CHECK(r64.finite, "Shutter 64-bit @96k finite");
}

void checkDrowner(const VST3::Hosting::Module::Ptr& module) {
    std::printf("SONORA Drowner\n");
    Instance inst;
    if (!create(module, inst)) {
        CHECK(false, "create Drowner");
        return;
    }
    CHECK(inst.controller->getParameterCount() == 10, "Drowner has 10 parameters (got %d)",
          inst.controller->getParameterCount());
    auto s1 = makeState(1, {0.75, 0.3}, false);
    CHECK(loadState(inst, s1), "Drowner accepts v1 state");
    CHECK(near(inst.get(0), 0.75) && near(inst.get(1), 0.3), "v1 Anteil/Auftauchzeit kept");
    checkRoundTrip(module, inst, "Drowner");
    auto wet = run<float>(inst, 48000.0, 512, 2.0, 0.25);
    CHECK(wet.finite, "Drowner output finite");

    Instance d;
    CHECK(create(module, d), "create Drowner (dry)");
    auto dry = run<double>(d, 48000.0, 512, 1.0, 0.25); // depth 0 -> bit-exact dry
    CHECK(dry.finite && near(db(dry.outRms), db(dry.inRms), 0.01), "Drowner depth 0 is dry (in %.2f out %.2f)",
          db(dry.inRms), db(dry.outRms));
}

VST3::Hosting::Module::Ptr load(const char* path) {
    std::string err;
    auto m = VST3::Hosting::Module::create(path, err);
    if (!m) std::printf("cannot load %s: %s\n", path, err.c_str());
    return m;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: %s <SONORA_Master.vst3> <SONORA_Shutter.vst3> <SONORA_Drowner.vst3>\n", argv[0]);
        return 2;
    }
    PluginContextFactory::instance().setPluginContext(hostContext());
    auto master = load(argv[1]);
    auto shutter = load(argv[2]);
    auto drowner = load(argv[3]);
    if (!master || !shutter || !drowner) return 1;
    checkMaster(master);
    checkShutter(shutter);
    checkDrowner(drowner);
    std::printf("hostcheck: %d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
