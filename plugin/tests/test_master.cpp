// SONORA Master v0.2 engine tests (SDK-free). Build:
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I plugin/src -I plugin/src/dsp -I plugin/src/ui -I plugin/tests
//       plugin/tests/test_master.cpp   (or: plugin/tests/run_tests.sh master)
#include "loudness.h"
#include "master_engine.h"
#include "test_util.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#define SONORA_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define SONORA_SANITIZED 1
#endif
#endif
#ifndef SONORA_SANITIZED
#define SONORA_SANITIZED 0
#endif

using namespace sonora;
using namespace sonora::test;
namespace M = sonora::dsp::master;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond, ...)                                                                      \
    do {                                                                                      \
        ++gChecks;                                                                            \
        if (!(cond)) {                                                                        \
            ++gFailures;                                                                      \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                   \
            std::printf(__VA_ARGS__);                                                         \
            std::printf("\n");                                                                \
        }                                                                                     \
    } while (0)

static void section(const char* name) { std::printf("[%s]\n", name); std::fflush(stdout); }

// fadeMs > 0 applies raised-cosine fades (avoids Gibbs overshoot of abrupt starts in TP tests).
static Audio sine(double sr, double f, double ampDb, double seconds, int channels, double phase = 0.0, double fadeMs = 0.0) {
    Audio a;
    a.sampleRate = sr;
    const int n = static_cast<int>(std::lround(sr * seconds));
    a.resize(channels, n);
    const double amp = dsp::dbToGain(ampDb);
    const int fade = static_cast<int>(fadeMs * 0.001 * sr);
    for (int i = 0; i < n; ++i) {
        double g = 1.0;
        const int edge = std::min(i, n - 1 - i);
        if (edge < fade) g = 0.5 - 0.5 * std::cos(dsp::kPi * edge / fade);
        const float v = static_cast<float>(g * amp * std::sin(2.0 * dsp::kPi * f * i / sr + phase));
        for (int c = 0; c < channels; ++c) a.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = v;
    }
    return a;
}

static Audio concat(const std::vector<Audio>& parts) {
    Audio out;
    out.sampleRate = parts[0].sampleRate;
    out.resize(parts[0].channels(), 0);
    for (const auto& p : parts)
        for (int c = 0; c < out.channels(); ++c)
            out.ch[static_cast<std::size_t>(c)].insert(out.ch[static_cast<std::size_t>(c)].end(), p.ch[static_cast<std::size_t>(c)].begin(), p.ch[static_cast<std::size_t>(c)].end());
    return out;
}

static Audio slice(const Audio& a, int start, int end) {
    Audio o;
    o.sampleRate = a.sampleRate;
    o.resize(a.channels(), end - start);
    for (int c = 0; c < a.channels(); ++c)
        std::copy(a.ch[static_cast<std::size_t>(c)].begin() + start, a.ch[static_cast<std::size_t>(c)].begin() + end, o.ch[static_cast<std::size_t>(c)].begin());
    return o;
}

static Audio monoOf(const Audio& a) {
    Audio o;
    o.sampleRate = a.sampleRate;
    o.resize(1, a.frames());
    for (int i = 0; i < a.frames(); ++i) {
        double s = 0.0;
        for (int c = 0; c < a.channels(); ++c) s += a.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
        o.ch[0][static_cast<std::size_t>(i)] = static_cast<float>(s / a.channels());
    }
    return o;
}

static Audio sideOf(const Audio& a) {
    Audio o;
    o.sampleRate = a.sampleRate;
    o.resize(1, a.frames());
    for (int i = 0; i < a.frames(); ++i)
        o.ch[0][static_cast<std::size_t>(i)] = 0.5f * (a.ch[0][static_cast<std::size_t>(i)] - a.ch[1][static_cast<std::size_t>(i)]);
    return o;
}

// Fresh engine with neutral settings: Master mode, Natural, Stärke 0 (everything identity except
// the limiter at the default -1 dBTP ceiling).
static void neutral(dsp::MasterEngine& e, int mode = M::kModeMaster) {
    for (int id = 0; id < M::kNumParams; ++id) e.setParameterPlain(id, M::kParams[static_cast<std::size_t>(id)].defaultValue);
    e.setParameterPlain(M::kMode, mode);
    e.setParameterPlain(M::kAmount, 0.0);
    e.setParameterPlain(M::kVocalFocus, 0.0);
}

struct Render {
    Audio out;
    double maxLimGr = 0.0, maxGlueGr = 0.0, maxClipGr = 0.0, maxDuck = 0.0;
};

static Render renderWithMeters(dsp::MasterEngine& e, const Audio& in, const Audio* sc = nullptr, int block = 512) {
    Render r;
    const int lat = e.latencySamples();
    const int frames = in.frames(), total = frames + lat, chs = in.channels();
    std::vector<std::vector<float>> ib(static_cast<std::size_t>(chs), std::vector<float>(static_cast<std::size_t>(total), 0.0f));
    for (int c = 0; c < chs; ++c) std::copy(in.ch[static_cast<std::size_t>(c)].begin(), in.ch[static_cast<std::size_t>(c)].end(), ib[static_cast<std::size_t>(c)].begin());
    const int scChs = sc ? sc->channels() : 0;
    std::vector<std::vector<float>> sb(static_cast<std::size_t>(scChs), std::vector<float>(static_cast<std::size_t>(total), 0.0f));
    for (int c = 0; c < scChs; ++c) std::copy(sc->ch[static_cast<std::size_t>(c)].begin(), sc->ch[static_cast<std::size_t>(c)].begin() + std::min(frames, sc->frames()), sb[static_cast<std::size_t>(c)].begin());
    std::vector<float*> p(static_cast<std::size_t>(chs));
    std::vector<const float*> s(static_cast<std::size_t>(std::max(1, scChs)));
    for (int pos = 0; pos < total; pos += block) {
        const int len = std::min(block, total - pos);
        for (int c = 0; c < chs; ++c) p[static_cast<std::size_t>(c)] = ib[static_cast<std::size_t>(c)].data() + pos;
        for (int c = 0; c < scChs; ++c) s[static_cast<std::size_t>(c)] = sb[static_cast<std::size_t>(c)].data() + pos;
        e.process(p.data(), p.data(), chs, len, scChs ? s.data() : nullptr, scChs); // in-place
        const auto& m = e.meters();
        r.maxLimGr = std::max(r.maxLimGr, static_cast<double>(m.limiterGrDb));
        r.maxGlueGr = std::max(r.maxGlueGr, static_cast<double>(m.glueGrDb));
        r.maxClipGr = std::max(r.maxClipGr, static_cast<double>(m.clipGrDb));
        r.maxDuck = std::max(r.maxDuck, static_cast<double>(m.duckDb));
    }
    r.out.sampleRate = in.sampleRate;
    r.out.resize(chs, frames);
    for (int c = 0; c < chs; ++c)
        std::copy(ib[static_cast<std::size_t>(c)].begin() + lat, ib[static_cast<std::size_t>(c)].end(), r.out.ch[static_cast<std::size_t>(c)].begin());
    return r;
}

// ------------------------------------------------------------------------------------------------

static void testParameterMapping() {
    section("parameter mapping");
    for (int id = 0; id < M::kNumParams; ++id) {
        const auto& p = M::kParams[static_cast<std::size_t>(id)];
        CHECK(p.minValue < p.maxValue, "id %d", id);
        CHECK(p.defaultValue >= p.minValue && p.defaultValue <= p.maxValue, "id %d", id);
        CHECK(std::abs(M::toPlain(id, 0.0) - p.minValue) < 1e-12 && std::abs(M::toPlain(id, 1.0) - p.maxValue) < 1e-12, "id %d", id);
        CHECK(std::abs(M::toPlain(id, M::defaultNormalized(id)) - p.defaultValue) < 1e-9, "id %d default", id);
        for (double n : {0.0, 0.13, 0.5, 0.77, 1.0}) {
            const double plain = M::toPlain(id, n);
            CHECK(std::abs(M::toPlain(id, M::toNormalized(id, plain)) - plain) < 1e-9, "id %d n %g", id, n);
        }
    }
    CHECK(M::kParams[M::kPreset].stepCount == 11, "preset steps");
    CHECK(M::kParams[M::kMode].stepCount == 2, "mode steps");
    CHECK(std::lround(M::toPlain(M::kPreset, 6.0 / 11.0)) == 6, "preset 6");
    CHECK(std::abs(M::toPlain(M::kLoudness, 0.5) - 9.0) < 1e-12, "lautheit");
    CHECK(std::abs(M::toPlain(M::kCeiling, M::defaultNormalized(M::kCeiling)) + 1.0) < 1e-12, "ceiling default");
    CHECK(std::abs(M::toPlain(M::kWidth, 0.5) - 100.0) < 1e-12, "width");
    // v0.1 preset list (6 entries) migrates onto the first six of 12.
    for (int i = 0; i < 6; ++i)
        CHECK(std::lround(M::toPlain(M::kPreset, M::migrateLegacyPresetNormalized(i / 5.0))) == i, "legacy %d", i);
    CHECK(std::string(M::kPresets[6].name) == "Trap 808" && std::string(M::kPresets[11].name) == "Club Loud", "names");
    dsp::MasterEngine e;
    e.prepare(48000.0, 256);
    e.setParameter(M::kPreset, 1.0);
    CHECK(std::lround(e.getParameterPlain(M::kPreset)) == 11, "engine preset");
    e.setParameter(M::kInput, 0.25);
    CHECK(std::abs(e.getParameterPlain(M::kInput) + 6.0) < 1e-12, "input");
    e.setParameterPlain(M::kEq1k, 99.0);
    CHECK(e.getParameterPlain(M::kEq1k) == 8.0, "clamp");
    e.setParameterPlain(M::kEq1k, std::nan(""));
    CHECK(e.getParameterPlain(M::kEq1k) == 0.0, "nan -> default");
}

static void testLoudnessMeter() {
    section("loudness meter (BS.1770-4 / EBU 3341)");
    for (double sr : {44100.0, 48000.0, 96000.0}) {
        // EBU 3341 test 1: stereo 1 kHz at -23 dBFS -> -23.0 LUFS (M, S, I)
        {
            Audio a = sine(sr, 1000.0, -23.0, 20.0, 2);
            const double l = lufsIntegrated(a);
            CHECK(std::abs(l + 23.0) <= 0.1, "sr %g stereo -23 dBFS -> %.3f", sr, l);
            dsp::LoudnessMeter m;
            m.prepare(sr);
            auto p = a.ptrs();
            m.process(p.data(), 2, a.frames());
            CHECK(std::abs(m.momentaryLufs() + 23.0) <= 0.1 && std::abs(m.shortTermLufs() + 23.0) <= 0.1,
                  "M %.3f S %.3f", m.momentaryLufs(), m.shortTermLufs());
        }
        // Full-scale 997 Hz in one channel only (mono file) -> -3.01 LUFS; on both channels -> ~0.0 LUFS
        {
            Audio mono = sine(sr, 997.0, 0.0, 10.0, 1);
            const double l = lufsIntegrated(mono);
            CHECK(std::abs(l + 3.01) <= 0.1, "sr %g mono FS 997 Hz -> %.3f", sr, l);
            Audio st = sine(sr, 997.0, 0.0, 10.0, 2);
            const double ls = lufsIntegrated(st);
            CHECK(std::abs(ls - 0.0) <= 0.1, "sr %g stereo FS 997 Hz -> %.3f", sr, ls);
        }
        // EBU 3341 test 3: -36 / -23 / -36 dBFS, 10 / 60 / 10 s -> -23.0 (relative gate)
        {
            Audio a = concat({sine(sr, 1000.0, -36.0, 10.0, 2), sine(sr, 1000.0, -23.0, 60.0, 2), sine(sr, 1000.0, -36.0, 10.0, 2)});
            const double l = lufsIntegrated(a);
            CHECK(std::abs(l + 23.0) <= 0.1, "sr %g test3 -> %.3f", sr, l);
        }
        // EBU 3341 test 5: -26 / -20 / -26, 20 / 20.1 / 20 s -> -23.0
        {
            Audio a = concat({sine(sr, 1000.0, -26.0, 20.0, 2), sine(sr, 1000.0, -20.0, 20.1, 2), sine(sr, 1000.0, -26.0, 20.0, 2)});
            const double l = lufsIntegrated(a);
            CHECK(std::abs(l + 23.0) <= 0.1, "sr %g test5 -> %.3f", sr, l);
        }
    }
    // Absolute gate: silence + tone
    {
        Audio a = concat({sine(48000.0, 1000.0, -200.0, 5.0, 2), sine(48000.0, 1000.0, -23.0, 10.0, 2)});
        CHECK(std::abs(lufsIntegrated(a) + 23.0) <= 0.1, "abs gate %.3f", lufsIntegrated(a));
    }
    // True peak: fs/4 sine with 45 deg phase -> sample peak -3.01 dB, true peak 0 dBTP (4x meter may
    // under-read by up to 0.69 dB for this worst case); 1 kHz sine reads its amplitude.
    {
        Audio a = sine(48000.0, 12000.0, 0.0, 1.0, 2, dsp::kPi / 4.0, 20.0);
        const double sp = samplePeakDb(a), tp = truePeakDb(a), ref = referenceTruePeakDb(a);
        CHECK(std::abs(sp + 3.01) < 0.02, "sample peak %.3f", sp);
        CHECK(tp > -0.75 && tp < 0.1, "fs/4 TP %.3f", tp);
        CHECK(std::abs(ref) < 0.05, "reference TP %.3f", ref);
        Audio b = sine(48000.0, 997.0, -6.0, 1.0, 2, 0.3, 20.0);
        CHECK(std::abs(truePeakDb(b) + 6.0) < 0.02, "1k TP %.3f", truePeakDb(b));
        // Inter-sample peak at 44.1 kHz: fs/4 sine at -1 dBFS with 45 deg phase -> SP -4.01, TP ~ -1.
        Audio c = sine(44100.0, 11025.0, -1.0, 1.0, 1, dsp::kPi / 4.0, 20.0);
        CHECK(truePeakDb(c) > samplePeakDb(c) + 2.3 && truePeakDb(c) < -0.9, "isp tp %.3f sp %.3f", truePeakDb(c), samplePeakDb(c));
    }
}

static void testBypassNull(const RapMix& mix) {
    section("bypass null + latency");
    dsp::MasterEngine e;
    e.prepare(48000.0, 512);
    applyPreset(e, test::presetIndexByName("Club Loud"), 100.0);
    e.setParameterPlain(M::kBypass, 1.0);
    e.reset();
    const int lat = e.latencySamples();
    CHECK(lat > 0 && lat <= 144, "latency %d samples (<= 3 ms @48k)", lat);
    Audio out = renderEngine(e, mix.mix, &mix.vocal, 512, false);
    double d = 0.0;
    for (int c = 0; c < 2; ++c) d = std::max(d, maxAbsDiff(std::vector<float>(mix.mix.ch[static_cast<std::size_t>(c)].begin(), mix.mix.ch[static_cast<std::size_t>(c)].end() - lat), out.ch[static_cast<std::size_t>(c)], static_cast<std::size_t>(lat)));
    CHECK(d == 0.0, "bypass max diff %g", d);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < lat; ++i) CHECK(out.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] == 0.0f, "pre-latency sample");
    // The processed path is aligned with the reported latency: neutral mono render of a band-limited
    // signal matches the input (oversampling filters are linear phase and flat to ~20 kHz).
    {
        dsp::MasterEngine w;
        w.prepare(48000.0, 512);
        neutral(w);
        w.reset();
        Audio s = sine(48000.0, 1000.0, -12.0, 1.0, 1, 0.0, 10.0);
        for (int i = 0; i < s.frames(); ++i) s.ch[0][static_cast<std::size_t>(i)] += static_cast<float>(0.1 * std::sin(2.0 * dsp::kPi * 7300.0 * i / 48000.0) * std::sin(dsp::kPi * i / s.frames()));
        const Audio o = renderEngine(w, s);
        int bestLag = 99;
        double best = 1e9;
        for (int lag = -4; lag <= 4; ++lag) {
            double e2 = 0.0;
            for (int i = 1000; i < s.frames() - 1000; ++i) {
                const double d = o.ch[0][static_cast<std::size_t>(i)] - s.ch[0][static_cast<std::size_t>(i - lag)];
                e2 += d * d;
            }
            if (e2 < best) { best = e2; bestLag = lag; }
        }
        const double diff = maxAbsDiff(s.ch[0], o.ch[0]);
        CHECK(bestLag == 0 && diff < 0.02, "wet path aligned with latency (lag %d, max diff %g)", bestLag, diff);
    }
    // Latency is identical in every mode and in bypass.
    for (int mode = 0; mode < 3; ++mode) {
        e.setParameterPlain(M::kMode, mode);
        CHECK(e.latencySamples() == lat, "mode %d latency", mode);
    }
    // Toggling bypass mid-stream: after the crossfade the output is the exact delayed input again.
    dsp::MasterEngine f;
    f.prepare(48000.0, 512);
    applyPreset(f, test::presetIndexByName("Trap 808"), 100.0);
    f.reset();
    const int n = mix.mix.frames();
    Audio buf = mix.mix;
    auto p = buf.ptrs();
    const int half = 48000 * 3;
    std::vector<float*> q(2);
    for (int pos = 0; pos < n; pos += 480) {
        if (pos == half) f.setParameterPlain(M::kBypass, 1.0);
        const int len = std::min(480, n - pos);
        for (int c = 0; c < 2; ++c) q[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + pos;
        f.process(q.data(), q.data(), 2, len);
    }
    double d2 = 0.0, jump = 0.0;
    const int settle = half + 4800;
    for (int c = 0; c < 2; ++c)
        for (int i = settle; i < n; ++i)
            d2 = std::max(d2, std::abs(static_cast<double>(buf.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) - mix.mix.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i - lat)]));
    for (int c = 0; c < 2; ++c)
        for (int i = half - 100; i < half + 1000; ++i)
            jump = std::max(jump, std::abs(static_cast<double>(buf.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) - buf.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i - 1)]));
    CHECK(d2 == 0.0, "bypass after toggle diff %g", d2);
    CHECK(jump < 0.5, "bypass crossfade jump %g", jump);
}

static void testBlockSizeInvariance(const RapMix& mix) {
    section("block-size invariance");
    const Audio in = slice(mix.mix, 0, 48000 * 4);
    const Audio sc = slice(mix.vocal, 0, 48000 * 4);
    for (const char* preset : {"Club Loud", "Vocal Upfront"}) {
        for (int mode : {M::kModeMaster, M::kModeBeat, M::kModeVocal}) {
            std::vector<Audio> outs;
            for (int bs : {1, 37, 512, 4096}) {
                dsp::MasterEngine e;
                e.prepare(48000.0, bs);
                applyPreset(e, test::presetIndexByName(preset), 100.0, mode);
                e.setParameterPlain(M::kVocalFocus, 100.0);
                e.reset();
                outs.push_back(renderEngine(e, in, &sc, bs));
            }
            double d = 0.0;
            for (std::size_t k = 1; k < outs.size(); ++k)
                for (int c = 0; c < 2; ++c) d = std::max(d, maxAbsDiff(outs[0].ch[static_cast<std::size_t>(c)], outs[k].ch[static_cast<std::size_t>(c)]));
            CHECK(d <= 1e-6, "%s mode %d block diff %g", preset, mode, d);
        }
    }
    // Parameter automation between blocks must not depend on block size either when changes land on
    // the same sample positions (multiples of 4096 here).
    {
        std::vector<Audio> outs;
        for (int bs : {64, 4096}) {
            dsp::MasterEngine e;
            e.prepare(48000.0, bs);
            applyPreset(e, 7, 60.0);
            e.reset();
            Audio buf = in;
            auto p = buf.ptrs();
            std::vector<float*> q(2);
            for (int pos = 0; pos < buf.frames(); pos += bs) {
                if (pos % 4096 == 0) {
                    e.setParameterPlain(M::kLoudness, (pos / 4096) % 7);
                    e.setParameterPlain(M::kEq4k, ((pos / 4096) % 5) - 2.0);
                    e.setParameterPlain(M::kMode, (pos / 4096) % 9 == 8 ? 1 : 2);
                }
                const int len = std::min(bs, buf.frames() - pos);
                for (int c = 0; c < 2; ++c) q[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + pos;
                e.process(q.data(), q.data(), 2, len);
            }
            outs.push_back(buf);
        }
        double d = 0.0;
        for (int c = 0; c < 2; ++c) d = std::max(d, maxAbsDiff(outs[0].ch[static_cast<std::size_t>(c)], outs[1].ch[static_cast<std::size_t>(c)]));
        CHECK(d <= 1e-6, "automation block diff %g", d);
    }
}

static void testSampleRates() {
    section("sample rates 44.1/48/96/192 kHz");
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const RapMix m = generateRapMix(sr, sr > 100000.0 ? 3.0 : 5.0, 11);
        for (int preset : {0, 6, 11}) {
            dsp::MasterEngine e;
            e.prepare(sr, 1024);
            applyPreset(e, preset, 100.0);
            e.setParameterPlain(M::kSaturation, 50.0);
            e.reset();
            const Audio out = renderEngine(e, m.mix, &m.vocal, 1024);
            const double lat_ms = 1000.0 * e.latencySamples() / sr;
            CHECK(allFinite(out), "sr %g preset %d finite", sr, preset);
            CHECK(dcOffset(out) < 1e-3, "sr %g preset %d DC %g", sr, preset, dcOffset(out));
            CHECK(lat_ms <= 3.0, "sr %g latency %.3f ms", sr, lat_ms);
            const double tp = truePeakDb(out);
            CHECK(tp <= -1.0 + 0.1, "sr %g preset %d TP %.3f", sr, preset, tp);
            const double l = lufsIntegrated(out);
            CHECK(l > -20.0 && l < -5.0, "sr %g preset %d LUFS %.2f", sr, preset, l);
        }
    }
}

static void testTruePeakCeiling(const RapMix& mix) {
    section("true peak <= ceiling + 0.1 dB on hot material");
    const Audio in = slice(mix.mix, 0, 48000 * 6);
    for (double ceiling : {-1.0, -0.1, -3.0}) {
        for (int preset : {0, 6, 11}) {
            for (double inGain : {6.0, 12.0}) {
                dsp::MasterEngine e;
                e.prepare(48000.0, 512);
                applyPreset(e, preset, 100.0);
                e.setParameterPlain(M::kCeiling, ceiling);
                e.setParameterPlain(M::kInput, inGain);
                e.setParameterPlain(M::kLoudness, preset == 0 ? 12.0 : 6.0);
                e.reset();
                const Audio out = renderEngine(e, in, nullptr, 512);
                const double ref = referenceTruePeakDb(slice(out, 0, 48000 * 3));
                const double tp = truePeakDb(out);
                CHECK(tp <= ceiling + 0.1 && ref <= ceiling + 0.1, "ceil %.1f preset %d in +%g: TP %.3f ref %.3f", ceiling, preset, inGain, tp, ref);
                CHECK(e.meters().truePeakMaxDb <= ceiling + 0.1, "meter hold %.3f", e.meters().truePeakMaxDb);
            }
        }
    }
    // Synthetic worst cases: full-scale square-ish bursts, high-frequency sines near Nyquist, impulses.
    {
        Audio a;
        a.sampleRate = 48000.0;
        a.resize(2, 48000 * 2);
        Rng rng(3);
        for (int i = 0; i < a.frames(); ++i) {
            const double t = i / 48000.0;
            double v = 0.0;
            if (t < 0.5) v = std::sin(2 * dsp::kPi * 60.0 * t) > 0 ? 1.0 : -1.0;
            else if (t < 1.0) v = std::sin(2 * dsp::kPi * 19000.0 * t + 0.3);
            else if (t < 1.5) v = (i % 2400 == 0) ? 1.0 : 0.0;
            else v = rng.bip();
            a.ch[0][static_cast<std::size_t>(i)] = static_cast<float>(v * 2.0);
            a.ch[1][static_cast<std::size_t>(i)] = static_cast<float>(-v * 2.0);
        }
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        neutral(e);
        e.setParameterPlain(M::kCeiling, -0.3);
        e.reset();
        const Audio out = renderEngine(e, a);
        const double ref = referenceTruePeakDb(out);
        CHECK(ref <= -0.3 + 0.1, "synthetic worst case ref TP %.3f", ref);
        CHECK(allFinite(out), "finite");
    }
}

static void testLoudnessTargets(const RapMix& mix) {
    section("loudness targets on -14 LUFS rap mix");
    const double inL = lufsIntegrated(mix.mix);
    CHECK(std::abs(inL + 14.0) < 0.05, "test mix %.2f LUFS", inL);
    struct Target { const char* name; double lufs, tol; };
    for (const Target& t : {Target{"Club Loud", -8.0, 1.5}, Target{"Trap 808", -9.0, 1.5}, Target{"Drill", -9.0, 1.5}}) {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        applyPreset(e, test::presetIndexByName(t.name), 100.0);
        e.reset();
        const Audio out = renderEngine(e, mix.mix);
        const double l = lufsIntegrated(out), tp = truePeakDb(out);
        CHECK(std::abs(l - t.lufs) <= t.tol, "%s LUFS-I %.2f (target %.1f)", t.name, l, t.lufs);
        CHECK(tp <= -1.0 + 0.1, "%s TP %.2f", t.name, tp);
    }
    // Natural @30 %: gentle — loudness change within +-1.5 dB, no real limiting.
    {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        applyPreset(e, 0, 30.0);
        e.reset();
        const Render r = renderWithMeters(e, mix.mix);
        const double l = lufsIntegrated(r.out);
        CHECK(std::abs(l - inL) <= 1.5, "Natural@30 dLUFS %.2f", l - inL);
        CHECK(r.maxLimGr < 0.5, "Natural@30 limiter GR max %.2f dB", r.maxLimGr);
    }
}

static void testClipperAliasing() {
    section("clipper aliasing (5 kHz sine, +12 dB drive)");
    for (double sr : {44100.0, 48000.0}) {
        const Audio in = sine(sr, 5000.0, -6.0, 2.0, 2);
        dsp::MasterEngine e;
        e.prepare(sr, 512);
        neutral(e);
        e.setParameterPlain(M::kClipper, 100.0);
        e.setParameterPlain(M::kLoudness, 12.0);
        e.reset();
        Render r = renderWithMeters(e, in);
        const double a = aliasingDbc(r.out.ch[0], sr, 5000.0, static_cast<std::size_t>(sr * 0.5), 65536);
        CHECK(a <= -60.0, "sr %g aliasing %.1f dBc", sr, a);
        CHECK(r.maxClipGr > 3.0, "clipper actually working: %.2f dB", r.maxClipGr);
        std::printf("  sr %.0f: non-harmonic 20 Hz-20 kHz %.1f dBc, clip GR %.1f dB, TP %.2f dBTP\n", sr, a, r.maxClipGr, truePeakDb(r.out));
        // Saturation at 100 % is also oversampled
        dsp::MasterEngine s;
        s.prepare(sr, 512);
        neutral(s);
        s.setParameterPlain(M::kSaturation, 100.0);
        s.setParameterPlain(M::kLoudness, 12.0);
        s.reset();
        const Audio so = renderEngine(s, in);
        const double as = aliasingDbc(so.ch[0], sr, 5000.0, static_cast<std::size_t>(sr * 0.5), 65536);
        CHECK(as <= -60.0, "saturation aliasing %.1f dBc", as);
    }
}

static void testCpu() {
    section("CPU (60 s stereo 48 kHz, Club Loud 100 %, all stages on)");
    const RapMix m = generateRapMix(48000.0, 10.0, 5);
    dsp::MasterEngine e;
    e.prepare(48000.0, 512);
    applyPreset(e, 11, 100.0);
    e.setParameterPlain(M::kSaturation, 50.0);
    e.setParameterPlain(M::kDeEsser, 50.0);
    e.setParameterPlain(M::kVocalFocus, 100.0);
    e.reset();
    Audio buf = m.mix;
    auto p = buf.ptrs();
    std::vector<float*> q(2);
    std::vector<const float*> s(1);
    const auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 6; ++rep) {
        for (int pos = 0; pos < buf.frames(); pos += 512) {
            const int len = std::min(512, buf.frames() - pos);
            for (int c = 0; c < 2; ++c) q[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + pos;
            s[0] = m.vocal.ch[0].data() + pos;
            e.process(q.data(), q.data(), 2, len, s.data(), 1);
        }
        buf = m.mix;
        p = buf.ptrs();
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  60 s rendered in %.3f s (%.1fx realtime)%s\n", secs, 60.0 / secs, SONORA_SANITIZED ? " [sanitizer build: not gated]" : "");
    // Target is < 2 s (30x realtime) on a desktop CPU; shared CI runners vary, so the hard gate is
    // 10x realtime and the target is reported only.
    if (!SONORA_SANITIZED) {
        if (secs >= 2.0) std::printf("  note: above the 2 s desktop target (shared/slow machine?)\n");
        CHECK(secs < 6.0, "CPU %.3f s (must stay > 10x realtime)", secs);
    }
}

static void testPresetsAndModes(const RapMix& mix) {
    section("all presets x 0/50/100 %, all modes mono+stereo");
    const Audio in = slice(mix.mix, 0, 48000 * 2);
    const Audio sc = slice(mix.vocal, 0, 48000 * 2);
    for (int p = 0; p < M::kNumPresets; ++p) {
        for (double amt : {0.0, 50.0, 100.0}) {
            dsp::MasterEngine e;
            e.prepare(48000.0, 256);
            applyPreset(e, p, amt);
            e.reset();
            const Audio out = renderEngine(e, in, &sc, 256);
            CHECK(allFinite(out) && truePeakDb(out) <= -0.9, "preset %d amt %g TP %.2f", p, amt, truePeakDb(out));
        }
    }
    const Audio monoIn = monoOf(in);
    for (int mode = 0; mode < 3; ++mode) {
        for (int p : {0, 6, 10}) {
            for (const Audio* src : {&in, &monoIn}) {
                dsp::MasterEngine e;
                e.prepare(48000.0, 300);
                applyPreset(e, p, 100.0, mode);
                e.setParameterPlain(M::kDeEsser, 60.0);
                e.setParameterPlain(M::kVocalFocus, 80.0);
                e.reset();
                const Audio out = renderEngine(e, *src, &sc, 300);
                CHECK(allFinite(out), "mode %d preset %d ch %d finite", mode, p, src->channels());
                CHECK(rmsDb(out) > -40.0, "mode %d preset %d ch %d has signal (%.1f)", mode, p, src->channels(), rmsDb(out));
                CHECK(samplePeakDb(out) < 3.0, "mode %d preset %d ch %d peak %.2f", mode, p, src->channels(), samplePeakDb(out));
                if (mode == M::kModeMaster) CHECK(truePeakDb(out) <= -0.9, "master TP %.2f", truePeakDb(out));
                if (mode == M::kModeVocal) CHECK(samplePeakDb(out) <= -0.2, "vocal safety clip %.2f", samplePeakDb(out));
            }
        }
    }
    // Extra channels are cleared; zero-length / null inputs are ignored.
    {
        dsp::MasterEngine e;
        e.prepare(48000.0, 64);
        std::vector<float> a(64, 0.5f), b(64, 0.5f), c(64, 0.5f);
        float* io[3] = {a.data(), b.data(), c.data()};
        e.process(io, io, 3, 64);
        e.process(io, io, 3, 0);
        CHECK(c[10] == 0.0f, "3rd channel cleared");
        std::vector<float> nanBuf(64, std::nanf(""));
        float* io2[2] = {nanBuf.data(), b.data()};
        e.process(io2, io2, 2, 64);
        CHECK(std::isfinite(nanBuf[63]), "NaN input sanitized");
    }
    // Mode switching while playing is click-free (bounded sample-to-sample step vs. the input's).
    {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        applyPreset(e, 6, 100.0);
        e.reset();
        Audio buf = in;
        auto p = buf.ptrs();
        std::vector<float*> q(2);
        for (int pos = 0; pos < buf.frames(); pos += 512) {
            if (pos % (512 * 20) == 0) e.setParameterPlain(M::kMode, (pos / (512 * 20)) % 3);
            const int len = std::min(512, buf.frames() - pos);
            for (int c = 0; c < 2; ++c) q[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + pos;
            e.process(q.data(), q.data(), 2, len);
        }
        double jumpOut = 0.0, jumpIn = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 1; i < buf.frames(); ++i) {
                jumpOut = std::max(jumpOut, std::abs(static_cast<double>(buf.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) - buf.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i - 1)]));
                jumpIn = std::max(jumpIn, std::abs(static_cast<double>(in.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]) - in.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i - 1)]));
            }
        CHECK(allFinite(buf) && jumpOut < 2.5 * jumpIn + 0.05, "mode switch jump %.3f (input %.3f)", jumpOut, jumpIn);
    }
}

static void testDucking(const RapMix& mix) {
    section("vocal sidechain ducking (Beat mode)");
    const Audio beat = slice(mix.beat, 0, 48000 * 8);
    const Audio voc = slice(mix.vocal, 0, 48000 * 8);
    Audio silent = voc;
    for (auto& v : silent.ch[0]) v = 0.0f;
    auto run = [&](const Audio* sc, double focus, Render* r = nullptr) {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        neutral(e, M::kModeBeat);
        e.setParameterPlain(M::kVocalFocus, focus);
        e.reset();
        Render out = renderWithMeters(e, beat, sc);
        if (r) *r = out;
        return out.out;
    };
    const Audio ref = run(nullptr, 100.0);
    Render rd;
    const Audio ducked = run(&voc, 100.0, &rd);
    const Audio silentSc = run(&silent, 100.0);
    const Audio noFocus = run(&voc, 0.0);
    // Only where the vocal is active (after the first bar).
    const int s0 = static_cast<int>(48000 * 60.0 / 140.0 * 4.0) + 4800;
    const int s1 = beat.frames();
    const double d900 = bandRmsDb(ducked, 700.0, 1150.0, s0, s1) - bandRmsDb(ref, 700.0, 1150.0, s0, s1);
    const double d3k2 = bandRmsDb(ducked, 2600.0, 3900.0, s0, s1) - bandRmsDb(ref, 2600.0, 3900.0, s0, s1);
    const double dLow = bandRmsDb(ducked, 30.0, 200.0, s0, s1) - bandRmsDb(ref, 30.0, 200.0, s0, s1);
    const double dHigh = bandRmsDb(ducked, 9000.0, 16000.0, s0, s1) - bandRmsDb(ref, 9000.0, 16000.0, s0, s1);
    std::printf("  ducking: 900 Hz %.2f dB, 3.2 kHz %.2f dB, low %.2f dB, air %.2f dB, max duck meter %.2f dB\n", d900, d3k2, dLow, dHigh, rd.maxDuck);
    CHECK(d900 < -0.5 && d3k2 < -0.5, "bands ducked %.2f %.2f", d900, d3k2);
    CHECK(std::abs(dLow) < 0.2 && std::abs(dHigh) < 0.3, "other bands untouched %.2f %.2f", dLow, dHigh);
    CHECK(rd.maxDuck > 1.0 && rd.maxDuck <= 4.01, "duck meter %.2f", rd.maxDuck);
    double dSil = 0.0, dNoF = 0.0;
    for (int c = 0; c < 2; ++c) {
        dSil = std::max(dSil, maxAbsDiff(ref.ch[static_cast<std::size_t>(c)], silentSc.ch[static_cast<std::size_t>(c)]));
        dNoF = std::max(dNoF, maxAbsDiff(ref.ch[static_cast<std::size_t>(c)], noFocus.ch[static_cast<std::size_t>(c)]));
    }
    CHECK(dSil < 1e-6, "silent sidechain changes nothing (%g)", dSil);
    CHECK(dNoF < 1e-6, "focus 0 changes nothing (%g)", dNoF);
    // In the first bar (no vocal yet) nothing is ducked.
    const double early = bandRmsDb(ducked, 700.0, 4000.0, 0, s0 - 9600) - bandRmsDb(ref, 700.0, 4000.0, 0, s0 - 9600);
    CHECK(std::abs(early) < 0.05, "no vocal -> no ducking (%.3f dB)", early);
}

static void testAnalysis(const RapMix& mix) {
    section("8 s analysis");
    auto analyse = [&](int mode, const Audio& in, const Audio* sc) {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        neutral(e, mode);
        e.reset();
        e.setParameter(M::kAnalyze, 1.0);
        double progressMid = -1.0;
        Audio a = in;
        auto p = a.ptrs();
        std::vector<float*> q(static_cast<std::size_t>(a.channels()));
        std::vector<const float*> s(1);
        for (int pos = 0; pos + 512 <= a.frames(); pos += 512) {
            for (int c = 0; c < a.channels(); ++c) q[static_cast<std::size_t>(c)] = p[static_cast<std::size_t>(c)] + pos;
            if (sc) s[0] = sc->ch[0].data() + pos;
            e.process(q.data(), q.data(), a.channels(), 512, sc ? s.data() : nullptr, sc ? 1 : 0);
            if (pos == 48000 * 4) progressMid = e.meters().analysisProgress;
        }
        std::printf("  mode %d sc %d: status %d, progress@4s %.2f, autoEQ [%.2f %.2f %.2f %.2f %.2f] ratio %.3f\n", mode, sc ? 1 : 0,
                    e.meters().analysisStatus, progressMid, e.meters().autoEqDb[0], e.meters().autoEqDb[1], e.meters().autoEqDb[2],
                    e.meters().autoEqDb[3], e.meters().autoEqDb[4], e.meters().autoCompRatio);
        CHECK((progressMid > 0.4 && progressMid < 0.6) || e.meters().analysisStatus >= 3, "progress %.2f", progressMid);
        return e.meters();
    };
    const Audio beat = slice(mix.beat, 0, 48000 * 9);
    const Audio voc = slice(mix.vocal, 0, 48000 * 9);
    const Audio full = slice(mix.mix, 0, 48000 * 9);
    {
        const auto m = analyse(M::kModeBeat, beat, &voc);
        CHECK(m.analysisStatus == M::kAnalysisComplete, "beat+sc complete (%d)", m.analysisStatus);
        double total = 0.0;
        for (int b = 0; b < M::kBands; ++b) {
            CHECK(m.autoEqDb[b] <= 0.0 && m.autoEqDb[b] >= -1.2, "restrained cut band %d %.2f", b, m.autoEqDb[b]);
            total += m.autoEqDb[b];
        }
        CHECK(total < 0.0, "some cut applied (%.2f)", total);
        CHECK(m.autoCompRatio >= 1.0 && m.autoCompRatio <= 1.12, "ratio %.3f", m.autoCompRatio);
        CHECK(m.vocalRmsDb > -60.0 && m.beatRmsDb > -40.0, "rms readout %.1f %.1f", m.vocalRmsDb, m.beatRmsDb);
    }
    {
        const auto m = analyse(M::kModeMaster, full, nullptr);
        CHECK(m.analysisStatus == M::kAnalysisCompleteMixOnly, "master mix-only (%d)", m.analysisStatus);
        for (int b = 0; b < M::kBands; ++b) CHECK(m.autoEqDb[b] <= 0.0 && m.autoEqDb[b] >= -1.2, "mix-only band %d %.2f", b, m.autoEqDb[b]);
    }
    {
        const auto m = analyse(M::kModeMaster, beat, &voc);
        CHECK(m.analysisStatus == M::kAnalysisComplete, "master+sc complete (%d)", m.analysisStatus);
    }
    CHECK(analyse(M::kModeVocal, full, nullptr).analysisStatus == M::kAnalysisWrongMode, "vocal mode -> wrong mode");
    CHECK(analyse(M::kModeBeat, beat, nullptr).analysisStatus == M::kAnalysisMissingSidechain, "beat w/o sc");
    {
        Audio silentV = voc;
        for (auto& v : silentV.ch[0]) v = 0.0f;
        CHECK(analyse(M::kModeBeat, beat, &silentV).analysisStatus == M::kAnalysisNoVocal, "no vocal");
        Audio silentB = beat;
        for (auto& c : silentB.ch) for (auto& v : c) v = 0.0f;
        CHECK(analyse(M::kModeBeat, silentB, &voc).analysisStatus == M::kAnalysisTooQuiet, "too quiet");
    }
    // Auto-EQ gains are applied to the EQ graph helper and can be restored/cleared.
    {
        std::array<double, M::kNumParams> plain{};
        for (int id = 0; id < M::kNumParams; ++id) plain[static_cast<std::size_t>(id)] = M::kParams[static_cast<std::size_t>(id)].defaultValue;
        const double cut[5] = {0.0, 0.0, -1.0, 0.0, 0.0};
        const double zero[5] = {0, 0, 0, 0, 0};
        const double r0 = dsp::MasterEngine::eqResponseDb(plain, zero, 48000.0, 1000.0);
        const double r1 = dsp::MasterEngine::eqResponseDb(plain, cut, 48000.0, 1000.0);
        CHECK(std::abs(r0) < 0.05 && std::abs(r1 + 1.0) < 0.1, "eq graph %.3f %.3f", r0, r1);
        plain[M::kEq4k] = 6.0;
        CHECK(std::abs(dsp::MasterEngine::eqResponseDb(plain, zero, 48000.0, 4000.0) - 6.0) < 0.3, "eq 4k");
    }
}

static void testPunchAndDeEsser(const RapMix& mix) {
    section("808-Punch, de-esser, width/bass-mono");
    const Audio beat = slice(mix.beat, 0, 48000 * 6);
    auto run = [&](int mode, const Audio& in, int id, double v) {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        neutral(e, mode);
        if (id >= 0) e.setParameterPlain(id, v);
        e.reset();
        return renderEngine(e, in);
    };
    {
        const Audio a = run(M::kModeBeat, beat, -1, 0), b = run(M::kModeBeat, beat, M::kPunch808, 100.0);
        const double dH = bandRmsDb(b, 100.0, 300.0) - bandRmsDb(a, 100.0, 300.0);
        const double dS = bandRmsDb(sideOf(b), 20.0, 300.0) - bandRmsDb(sideOf(a), 20.0, 300.0);
        const double dHi = bandRmsDb(b, 2000.0, 16000.0) - bandRmsDb(a, 2000.0, 16000.0);
        std::printf("  808-Punch 100%%: 100-300 Hz %+.2f dB, side<300 %+.2f dB, >2k %+.2f dB\n", dH, dS, dHi);
        CHECK(dH > 1.5, "punch adds 100-300 Hz harmonics %.2f", dH);
        CHECK(dS < 0.1, "punch is mono-safe %.2f", dS);
        CHECK(std::abs(dHi) < 0.3, "punch leaves highs %.2f", dHi);
    }
    {
        const Audio voc = slice(mix.vocal, 0, 48000 * 6);
        const Audio a = run(M::kModeVocal, voc, -1, 0), b = run(M::kModeVocal, voc, M::kDeEsser, 100.0);
        const double dS = bandRmsDb(b, 6000.0, 9000.0) - bandRmsDb(a, 6000.0, 9000.0);
        const double dB = bandRmsDb(b, 200.0, 2000.0) - bandRmsDb(a, 200.0, 2000.0);
        std::printf("  de-esser 100%%: 6-9 kHz %+.2f dB, 200-2k %+.2f dB\n", dS, dB);
        CHECK(dS < -3.0, "sibilants reduced %.2f", dS);
        CHECK(std::abs(dB) < 0.3, "body untouched %.2f", dB);
    }
    {
        // Synthetic stereo material: strongly one-sided 50 Hz bass + decorrelated broadband noise.
        Audio st;
        st.sampleRate = 48000.0;
        st.resize(2, 48000 * 4);
        Rng rng(21);
        for (int i = 0; i < st.frames(); ++i) {
            const double b = std::sin(2.0 * dsp::kPi * 50.0 * i / 48000.0);
            st.ch[0][static_cast<std::size_t>(i)] = static_cast<float>(0.3 * b + 0.08 * rng.bip());
            st.ch[1][static_cast<std::size_t>(i)] = static_cast<float>(0.05 * b + 0.08 * rng.bip());
        }
        const Audio a = run(M::kModeMaster, st, -1, 0);
        const Audio bm = run(M::kModeMaster, st, M::kBassMono, 200.0);
        const Audio w0 = run(M::kModeMaster, st, M::kWidth, 0.0);
        const Audio w2 = run(M::kModeMaster, st, M::kWidth, 200.0);
        const int s0 = 4800;
        const int s1 = st.frames() - 4800;
        const double sLowA = bandRmsDb(sideOf(a), 20.0, 80.0, s0, s1), sLowB = bandRmsDb(sideOf(bm), 20.0, 80.0, s0, s1);
        const double mLowA = bandRmsDb(monoOf(a), 20.0, 80.0, s0, s1), mLowB = bandRmsDb(monoOf(bm), 20.0, 80.0, s0, s1);
        const double sHiA = bandRmsDb(sideOf(a), 2000.0, 16000.0, s0, s1), sHiBm = bandRmsDb(sideOf(bm), 2000.0, 16000.0, s0, s1);
        const double sHi0 = bandRmsDb(sideOf(w0), 2000.0, 16000.0, s0, s1), sHi2 = bandRmsDb(sideOf(w2), 2000.0, 16000.0, s0, s1);
        const double sLo2 = bandRmsDb(sideOf(w2), 20.0, 120.0, s0, s1), sLoA = bandRmsDb(sideOf(a), 20.0, 120.0, s0, s1);
        std::printf("  bass-mono 200 Hz: side <80 Hz %.1f -> %.1f dB (mid %.1f -> %.1f), side >2k %.1f -> %.1f dB\n", sLowA, sLowB, mLowA, mLowB, sHiA, sHiBm);
        std::printf("  width 0/100/200: side >2k %.1f / %.1f / %.1f dB, side <120 Hz %.1f -> %.1f dB\n", sHi0, sHiA, sHi2, sLoA, sLo2);
        CHECK(sLowB < sLowA - 20.0, "bass mono removes side lows");
        CHECK(std::abs(mLowB - mLowA) < 0.2, "bass mono keeps mid lows %.2f", mLowB - mLowA);
        CHECK(std::abs(sHiBm - sHiA) < 0.2, "bass mono keeps side highs %.2f", sHiBm - sHiA);
        CHECK(sHi0 < sHiA - 30.0, "width 0 -> mono highs");
        CHECK(std::abs(sHi2 - sHiA - 6.02) < 0.3, "width 200 = +6 dB side highs (%.2f)", sHi2 - sHiA);
        CHECK(std::abs(sLo2 - sLoA) < 0.5, "width leaves lows (%.2f)", sLo2 - sLoA);
        // Width 100 % / bass-mono off is an allpass on L/R: band levels unchanged.
        for (const auto& bnd : std::vector<std::pair<double, double>>{{30.0, 150.0}, {150.0, 400.0}, {400.0, 2000.0}, {2000.0, 16000.0}}) {
            for (int c = 0; c < 2; ++c) {
                Audio ai, ao;
                ai.sampleRate = ao.sampleRate = 48000.0;
                ai.ch = {st.ch[static_cast<std::size_t>(c)]};
                ao.ch = {a.ch[static_cast<std::size_t>(c)]};
                const double d = bandRmsDb(ao, bnd.first, bnd.second, s0, s1) - bandRmsDb(ai, bnd.first, bnd.second, s0, s1);
                // Lowest band includes the 20 Hz HPF (~ -0.1 dB at 50 Hz).
                CHECK(std::abs(d) < (bnd.first < 100.0 ? 0.25 : 0.15), "neutral stereo path flat: ch %d %g-%g Hz %.3f dB", c, bnd.first, bnd.second, d);
            }
        }
    }
}

static void printPresetTable(const RapMix& mix) {
    section("preset table on -14 LUFS rap test mix (Master mode, Stärke 100 %, sidechain off)");
    const MixMetrics in = measureMix(mix.mix);
    std::printf("  %-14s %7s %7s %6s %6s | %6s %6s %6s %6s %6s (band dB rel. to LUFS)\n", "preset", "LUFS-I", "TP", "PLR", "crest",
                "25-60", "60-100", "1-3H", "1-4k", "6-16k");
    auto row = [&](const char* name, const MixMetrics& m) {
        std::printf("  %-14s %7.2f %7.2f %6.2f %6.2f | %6.1f %6.1f %6.1f %6.1f %6.1f\n", name, m.lufs, m.tp, m.plr, m.crest,
                    m.sub - m.lufs, m.low - m.lufs, m.harm - m.lufs, m.mid - m.lufs, m.high - m.lufs);
    };
    row("(input)", in);
    for (int p = 0; p < M::kNumPresets; ++p) {
        dsp::MasterEngine e;
        e.prepare(48000.0, 512);
        applyPreset(e, p, 100.0);
        e.reset();
        const Audio out = renderEngine(e, mix.mix);
        const MixMetrics m = measureMix(out);
        row(M::kPresets[static_cast<std::size_t>(p)].name, m);
        CHECK(m.tp <= -0.9, "%s TP %.2f", M::kPresets[static_cast<std::size_t>(p)].name, m.tp);
        if (p >= 6) CHECK(m.lufs > in.lufs + 3.0, "rap preset %s clearly louder (%.2f)", M::kPresets[static_cast<std::size_t>(p)].name, m.lufs);
        if (p >= 6) CHECK(m.plr > 6.0, "rap preset %s PLR %.2f not crushed", M::kPresets[static_cast<std::size_t>(p)].name, m.plr);
        // Rap lives from the vocal: the 1-4 kHz band (relative to integrated loudness) must not
        // drop against the input mix; Vocal Upfront lifts it by 1-2 dB.
        const double vocalDelta = (m.mid - m.lufs) - (in.mid - in.lufs);
        const std::string name = M::kPresets[static_cast<std::size_t>(p)].name;
        if (name == "Vocal Upfront") {
            CHECK(vocalDelta >= 1.0 && vocalDelta <= 2.0, "Vocal Upfront vocal band %+.2f dB (want +1..+2)", vocalDelta);
        } else if (p >= 6) {
            CHECK(vocalDelta >= 0.0, "%s vocal band %+.2f dB (must not drop)", name.c_str(), vocalDelta);
        }
    }
}

int main() {
    std::printf("SONORA Master engine tests%s\n", SONORA_SANITIZED ? " (sanitizer build)" : "");
    const RapMix mix = generateRapMix(48000.0, 12.0, 7);
    testParameterMapping();
    testLoudnessMeter();
    testBypassNull(mix);
    testBlockSizeInvariance(mix);
    testSampleRates();
    testTruePeakCeiling(mix);
    testLoudnessTargets(mix);
    testClipperAliasing();
    testPresetsAndModes(mix);
    testDucking(mix);
    testAnalysis(mix);
    testPunchAndDeEsser(mix);
    testCpu();
    printPresetTable(mix);
    std::printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
