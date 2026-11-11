// Unit tests + demo renders for the SONORA Shutter engine.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -I plugin/src -I plugin/src/dsp plugin/tests/test_shutter.cpp
// Run:   ./a.out                 (tests)
//        ./a.out --render <dir>  (tests + demo WAVs)

#include "shutter_engine.h"
#include "fx_test_util.h"

#include <cstdio>
#include <functional>
#include <string>

using namespace sonora::dsp::shutter;
using fxtest::Buffer;
using fxtest::TimedEvent;

namespace {

struct Setup {
    double sr = 48000.0;
    double bpm = 140.0;
    double startPpq = 0.0;
    bool hasPosition = true;
    long stopAt = -1;     // transport stops at this sample (block aligned in the tests)
    long restartAt = -1;  // and restarts here
};

void set(ShutterEngine& e, int id, double plain) { e.setParameter(id, toNormalized(id, plain)); }
TimedEvent at(long sample, int id, double plain) { return {sample, id, toNormalized(id, plain)}; }

ShutterEngine makeEngine(double sr, const std::vector<std::pair<int, double>>& params) {
    ShutterEngine e;
    e.prepare(sr, 4096);
    set(e, kEnable, 1);
    for (const auto& p : params) set(e, p.first, p.second);
    e.reset();  // start with settled smoothers
    return e;
}

template <typename T = double>
Buffer run(ShutterEngine& eng, const Buffer& in, int block, const Setup& s,
           const std::vector<TimedEvent>& events = {}) {
    return fxtest::runBlocks<T, ParamEvent>(
        in, block, events,
        [&](const T* const* i, T* const* o, int ch, int n, long start, const ParamEvent* ev, int ne) {
            TransportInfo t;
            t.tempo = s.bpm;
            t.hasPosition = s.hasPosition;
            t.ppqPosition = s.startPpq + static_cast<double>(start) * s.bpm / (60.0 * s.sr);
            t.isPlaying = !(s.stopAt >= 0 && start >= s.stopAt && (s.restartAt < 0 || start < s.restartAt));
            eng.process(i, o, ch, n, t, ev, ne);
        });
}

Buffer dc(int channels, std::size_t n, double v = 1.0) { return Buffer(channels, std::vector<double>(n, v)); }

struct Crossing {
    double pos;
    bool rising;
};
std::vector<Crossing> findCrossings(const std::vector<double>& y, double level, std::size_t begin, std::size_t end) {
    std::vector<Crossing> c;
    for (std::size_t i = std::max<std::size_t>(begin, 1); i < std::min(end, y.size()); ++i) {
        const double a = y[i - 1] - level, b = y[i] - level;
        if ((a < 0.0 && b >= 0.0) || (a >= 0.0 && b < 0.0))
            c.push_back({static_cast<double>(i - 1) + a / (a - b), b >= 0.0});
    }
    return c;
}

// Expected 50 % crossings of the Hart gate (edge start + half the raised-cosine ramp).
std::vector<Crossing> expectedCrossings(double stepQ, double duty, double swing, double offset, double sr,
                                        double bpm, double startPpq, double rampSamples, double beginS,
                                        double endS) {
    const double spq = sr * 60.0 / bpm;
    const double d = swing * 2.0 / 3.0;
    std::vector<Crossing> c;
    const long k0 = static_cast<long>(std::floor(startPpq / stepQ - offset)) - 3;
    for (long k = k0;; ++k) {
        const bool even = ((k % 2) + 2) % 2 == 0;
        const double start = even ? k + offset : k + d + offset;
        const double len = even ? 1.0 + d : 1.0 - d;
        const double open = (start * stepQ - startPpq) * spq + rampSamples * 0.5;
        const double close = ((start + duty * len) * stepQ - startPpq) * spq + rampSamples * 0.5;
        if (open > endS) break;
        if (open >= beginS && open < endS) c.push_back({open, true});
        if (close >= beginS && close < endS) c.push_back({close, false});
    }
    return c;
}

// ------------------------------------------------------------------------------------------
void testMappings() {
    std::printf("mappings\n");
    for (int id = 0; id < kNumParams; ++id) {
        const auto& p = kParamInfo[static_cast<std::size_t>(id)];
        FX_CHECK(std::abs(toPlain(id, toNormalized(id, p.defaultPlain)) - p.defaultPlain) < 1e-9, "id %d", id);
        FX_CHECK(toPlain(id, 0.0) == p.minPlain && toPlain(id, 1.0) == p.maxPlain, "id %d range", id);
    }
    static_assert(toNormalized(kDivision, 3) == 3.0 / 11.0, "default 1/8");
    static_assert(migrateDivisionV1ToV2(4.0 / 8.0) == 4.0 / 11.0, "v1 1/16 -> v2 1/16");
    static_assert(listIndex(kDivision, defaultNormalized(kDivision)) == 3, "1/8");
    FX_CHECK(std::abs(toPlain(kRate, 0.35) - (0.5 + 0.35 * 19.5)) < 1e-12, "v1 rate mapping kept");
    for (int i = 0; i < kNumDivisionsV1; ++i) {
        const double v2 = migrateDivisionV1ToV2(i / 8.0);
        FX_CHECK(listIndex(kDivision, v2) == i, "v1 index %d -> %d", i, listIndex(kDivision, v2));
    }
    // v1 quarter-note table (first 9 entries) must be unchanged.
    const double v1[9] = {4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0};
    for (int i = 0; i < 9; ++i) FX_CHECK(kDivisionQuarters[static_cast<std::size_t>(i)] == v1[i], "division %d", i);
    FX_CHECK(kDivisionQuarters[9] == 1.5 && kDivisionQuarters[10] == 0.75 && kDivisionQuarters[11] == 0.375, "dotted");
    FX_CHECK(std::string(kDivisionNames[9]) == "1/4 P" && std::string(kDivisionNames[6]) == "1/8 T", "names");
}

void checkAlignment(const char* label, double sr, double bpm, double startPpq, int division, double duty,
                    double swing, double offset) {
    auto eng = makeEngine(sr, {{kDivision, division}, {kDepth, 100}, {kMix, 100}, {kSmoothing, 0},
                               {kDuty, duty}, {kSwing, swing}, {kOffset, offset}});
    const std::size_t n = static_cast<std::size_t>(sr * 4.5);
    Setup s;
    s.sr = sr;
    s.bpm = bpm;
    s.startPpq = startPpq;
    const Buffer y = run(eng, dc(1, n), 512, s);
    const double ramp = 0.6e-3 * sr;
    const double begin = 0.03 * sr, end = static_cast<double>(n) - 0.01 * sr;
    const auto got = findCrossings(y[0], 0.5, static_cast<std::size_t>(begin), static_cast<std::size_t>(end));
    const auto want = expectedCrossings(kDivisionQuarters[static_cast<std::size_t>(division)], duty * 0.01,
                                        swing * 0.01, offset * 0.01, sr, bpm, startPpq, ramp, begin + 2, end - 2);
    double worst = 0.0;
    std::size_t matched = 0;
    for (const auto& w : want) {
        double best = 1e9;
        for (const auto& g : got)
            if (g.rising == w.rising) best = std::min(best, std::abs(g.pos - w.pos));
        worst = std::max(worst, best);
        if (best < 0.5) ++matched;
    }
    FX_CHECK(want.size() >= 4 && matched == want.size() && got.size() >= want.size() && got.size() <= want.size() + 2,
             "%s: %zu/%zu edges matched (got %zu), worst %.3f samples", label, matched, want.size(), got.size(), worst);
    FX_CHECK(worst < 0.5, "%s: worst edge error %.3f samples", label, worst);
    std::printf("  %-40s %zu edges, worst error %.4f samples\n", label, want.size(), worst);
}

void testGateAlignment() {
    std::printf("gate alignment\n");
    for (int d = 0; d < kNumDivisions; ++d) {
        char label[64];
        std::snprintf(label, sizeof label, "%s @140", kDivisionNames[static_cast<std::size_t>(d)]);
        checkAlignment(label, 48000.0, 140.0, 0.0, d, 50, 0, 0);
    }
    checkAlignment("1/8 swing50 off25 duty30", 48000.0, 140.0, 0.0, 3, 30, 50, 25);
    checkAlignment("1/16 T swing75 off60 duty70", 48000.0, 140.0, 0.0, 7, 70, 75, 60);
    checkAlignment("1/8 P swing20 off10 @93 44.1k ppq 3.7", 44100.0, 93.0, 3.7, 10, 40, 20, 10);
    checkAlignment("1/16 duty10 @160 96k ppq 17.25", 96000.0, 160.0, 17.25, 4, 10, 0, 0);
    checkAlignment("1/4 duty90 swing33 @85 192k", 192000.0, 85.0, 1.0, 2, 90, 33, 5);
}

void testTripletDotted() {
    std::printf("triplet / dotted lengths\n");
    const double sr = 48000.0, bpm = 120.0, spq = sr * 60.0 / bpm;
    for (int d : {6, 7, 8, 9, 10, 11}) {
        auto eng = makeEngine(sr, {{kDivision, d}, {kDepth, 100}, {kSmoothing, 0}});
        const Buffer y = run(eng, dc(1, static_cast<std::size_t>(sr * 6)), 256, Setup{sr, bpm});
        std::vector<double> rises;
        for (const auto& c : findCrossings(y[0], 0.5, static_cast<std::size_t>(0.03 * sr), y[0].size()))
            if (c.rising) rises.push_back(c.pos);
        double maxErr = 0.0;
        const double expected = kDivisionQuarters[static_cast<std::size_t>(d)] * spq;
        for (std::size_t i = 1; i < rises.size(); ++i) maxErr = std::max(maxErr, std::abs(rises[i] - rises[i - 1] - expected));
        FX_CHECK(rises.size() >= 3 && maxErr < 0.05, "%s: period error %.4f samples (%zu edges, want %.2f)",
                 kDivisionNames[static_cast<std::size_t>(d)], maxErr, rises.size(), expected);
    }
}

void testFreeRun() {
    std::printf("free run\n");
    const double sr = 48000.0;
    {   // Sync off: Rate drives the step rate even with a valid position.
        auto eng = makeEngine(sr, {{kSync, 0}, {kRate, 5}, {kDepth, 100}, {kSmoothing, 0}});
        const Buffer y = run(eng, dc(1, static_cast<std::size_t>(sr * 3)), 333, Setup{sr, 140.0});
        std::vector<double> rises;
        for (const auto& c : findCrossings(y[0], 0.5, 1000, y[0].size()))
            if (c.rising) rises.push_back(c.pos);
        double maxErr = 0.0;
        for (std::size_t i = 1; i < rises.size(); ++i) maxErr = std::max(maxErr, std::abs(rises[i] - rises[i - 1] - sr / 5.0));
        FX_CHECK(rises.size() >= 10 && maxErr < 0.05, "rate 5 Hz period error %.4f", maxErr);
    }
    {   // Sync on, no position: free-runs at the tempo-derived step rate.
        auto eng = makeEngine(sr, {{kDivision, 3}, {kDepth, 100}, {kSmoothing, 0}});
        Setup s{sr, 120.0};
        s.hasPosition = false;
        const Buffer y = run(eng, dc(1, static_cast<std::size_t>(sr * 3)), 333, s);
        std::vector<double> rises;
        for (const auto& c : findCrossings(y[0], 0.5, 1000, y[0].size()))
            if (c.rising) rises.push_back(c.pos);
        double maxErr = 0.0;
        for (std::size_t i = 1; i < rises.size(); ++i) maxErr = std::max(maxErr, std::abs(rises[i] - rises[i - 1] - sr * 0.25));
        FX_CHECK(rises.size() >= 10 && maxErr < 0.05, "no-position 1/8 @120 period error %.4f", maxErr);
    }
}

void testTransportStop() {
    std::printf("transport stop\n");
    const double sr = 48000.0;
    const auto x = fxtest::makeVocal(sr, 3.0);
    auto eng = makeEngine(sr, {{kDivision, 4}, {kDepth, 100}, {kGateTone, 1}});
    Setup s{sr, 140.0};
    s.stopAt = 512 * 100;     // ≈ 1.07 s
    s.restartAt = 512 * 200;  // ≈ 2.13 s
    const Buffer y = run(eng, Buffer{x}, 512, s);
    const std::size_t dryFrom = static_cast<std::size_t>(s.stopAt) + static_cast<std::size_t>(0.009 * sr);
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], dryFrom, static_cast<std::size_t>(s.restartAt)) == 0.0, "stopped -> bit-exact dry");
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], static_cast<std::size_t>(s.restartAt) + 2000, x.size()) > 0.05, "gates again after restart");
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], 2000, static_cast<std::size_t>(s.stopAt)) > 0.05, "gates while playing");

    // Fade on stop/start must not click: sine input, gate closed at the stop/start moments.
    const auto sn = fxtest::sine(200.0, 0.5, sr, static_cast<std::size_t>(sr * 2.0));
    auto e2 = makeEngine(sr, {{kDivision, 0}, {kDuty, 10}, {kDepth, 100}});
    Setup s2{sr, 120.0};
    s2.stopAt = 24000;     // 0.5 s: inside the closed part of the 2 s step
    s2.restartAt = 48000;  // 1.0 s
    const Buffer y2 = run(e2, Buffer{sn}, 64, s2);
    const double bound = fxtest::maxStep(sn);
    const double got = fxtest::maxStep(y2[0], 22000, 60000);
    FX_CHECK(got <= 1.2 * bound, "stop/start fade step %.5f vs input %.5f", got, bound);
}

void testDisabledBitExact() {
    std::printf("disabled bit-exact\n");
    const double sr = 44100.0;
    const auto x = fxtest::makeVocal(sr, 2.0, 140.0, 3);
    const auto xr = fxtest::makeVocal(sr, 2.0, 140.0, 4);
    ShutterEngine e;
    e.prepare(sr, 1024);
    set(e, kDepth, 100);
    set(e, kGateTone, 2);
    set(e, kStereo, 1);
    set(e, kDivision, 5);
    e.reset();
    const Buffer yd = run(e, Buffer{x, xr}, 1024, Setup{sr, 140.0});
    FX_CHECK(yd[0] == x && yd[1] == xr, "double: disabled output == input");
    e.reset();
    const Buffer yf = run<float>(e, Buffer{x, xr}, 1024, Setup{sr, 140.0});
    bool exact = true;
    for (std::size_t i = 0; i < x.size(); ++i)
        exact = exact && yf[0][i] == static_cast<double>(static_cast<float>(x[i]))
                && yf[1][i] == static_cast<double>(static_cast<float>(xr[i]));
    FX_CHECK(exact, "float: disabled output == input");
}

void testEnableAndDepthClickFree() {
    std::printf("enable / depth automation click-free\n");
    const double sr = 48000.0;
    const auto sn = fxtest::sine(200.0, 0.5, sr, static_cast<std::size_t>(sr * 1.9));
    const double bound = fxtest::maxStep(sn);
    {
        auto e = makeEngine(sr, {{kDivision, 0}, {kDuty, 10}, {kDepth, 100}, {kGateTone, 0}});
        set(e, kEnable, 0);
        e.reset();
        // Closed part of the 2 s step (120 BPM, 1/1, duty 10 %) is 0.2 s … 2.0 s.
        std::vector<TimedEvent> ev{at(24000, kEnable, 1), at(40000, kEnable, 0), at(52000, kEnable, 1),
                                   at(52100, kEnable, 0), at(70000, kEnable, 1)};
        const Buffer y = run(e, Buffer{sn}, 128, Setup{sr, 120.0}, ev);
        const double got = fxtest::maxStep(y[0], 12000, sn.size());
        FX_CHECK(got <= 1.2 * bound, "enable/disable step %.5f vs input %.5f", got, bound);
        FX_CHECK(fxtest::peak(y[0], 30000, 39000) < 1e-9, "enabled + closed + depth 100 -> silent");
        FX_CHECK(fxtest::maxAbsDiff(sn, y[0], 41000, 52000) == 0.0, "disabled again -> bit-exact");
    }
    {
        auto e = makeEngine(sr, {{kDivision, 0}, {kDuty, 10}, {kDepth, 0}, {kGateTone, 1}});
        std::vector<TimedEvent> ev;
        for (int k = 0; k < 30; ++k) {
            ev.push_back(at(12000 + k * 2400, kDepth, (k % 3) * 50.0));
            ev.push_back(at(12000 + k * 2400 + 1200, kMix, k % 2 ? 100.0 : 30.0));
        }
        const Buffer y = run(e, Buffer{sn}, 100, Setup{sr, 120.0}, ev);
        const double got = fxtest::maxStep(y[0], 11000, sn.size());
        FX_CHECK(got <= 1.2 * bound, "depth/mix automation step %.5f vs input %.5f", got, bound);
    }
    {   // Gate-Klang switching while closed must crossfade.
        auto e = makeEngine(sr, {{kDivision, 0}, {kDuty, 10}, {kDepth, 100}, {kGateTone, 0}});
        std::vector<TimedEvent> ev;
        for (int k = 0; k < 20; ++k) ev.push_back(at(12000 + k * 3000, kGateTone, k % 3));
        const Buffer y = run(e, Buffer{sn}, 100, Setup{sr, 120.0}, ev);
        const double got = fxtest::maxStep(y[0], 11000, sn.size());
        FX_CHECK(got <= 1.2 * bound, "gate-tone switching step %.5f vs input %.5f", got, bound);
    }
}

void testGlaettung() {
    std::printf("Glättung edges\n");
    const double sr = 48000.0;
    const auto sn = fxtest::sine(150.0, 0.5, sr, static_cast<std::size_t>(sr * 3));
    const double bound = fxtest::maxStep(sn);
    // 2 ms raised-cosine edges: gain slope ≤ π/2 / 96 per sample -> step ≤ bound + 0.5·0.0164.
    auto e = makeEngine(sr, {{kDivision, 4}, {kDepth, 100}, {kSmoothing, 2}});
    const Buffer y = run(e, Buffer{sn}, 256, Setup{sr, 140.0});
    const double got = fxtest::maxStep(y[0], 1000, sn.size());
    FX_CHECK(got <= bound + 0.5 * ((sonora::dsp::kPi / 2.0) / (0.002 * sr)) + 1e-6, "2 ms edges step %.5f", got);
    // Weich is softer still.
    auto w = makeEngine(sr, {{kDivision, 4}, {kDepth, 100}, {kSmoothing, 0}, {kShape, kShapeSoft}});
    const Buffer yw = run(w, Buffer{sn}, 256, Setup{sr, 140.0});
    FX_CHECK(fxtest::maxStep(yw[0], 1000, sn.size()) <= 1.2 * bound, "Weich step %.5f", fxtest::maxStep(yw[0], 1000, sn.size()));
}

void testPump() {
    std::printf("Pumpe\n");
    const double sr = 48000.0, bpm = 120.0;
    auto e = makeEngine(sr, {{kDivision, 2}, {kDepth, 80}, {kShape, kShapePump}, {kDuty, 70}, {kSmoothing, 2}});
    const Buffer y = run(e, dc(1, static_cast<std::size_t>(sr * 3)), 512, Setup{sr, bpm});
    const std::size_t step = static_cast<std::size_t>(sr * 0.5);  // 1/4 @ 120 = 0.5 s
    for (std::size_t k = 1; k < 5; ++k) {
        const std::size_t s0 = k * step;
        const double atDip = y[0][s0 + static_cast<std::size_t>(0.004 * sr)];
        const double mid = y[0][s0 + static_cast<std::size_t>(0.35 * 0.5 * sr * 0.7)];
        const double end = y[0][s0 + static_cast<std::size_t>(0.75 * 0.5 * sr)];
        const double before = y[0][s0 - 10];
        FX_CHECK(std::abs(atDip - 0.2) < 0.01, "dip %.3f (want 0.2)", atDip);
        FX_CHECK(mid > 0.25 && mid < 0.95, "mid recovery %.3f", mid);
        FX_CHECK(std::abs(end - 1.0) < 1e-9 && std::abs(before - 1.0) < 1e-9, "recovered %.4f / %.4f", end, before);
    }
    // Monotonic recovery after the dip (no wiggles).
    bool mono = true;
    for (std::size_t i = step + static_cast<std::size_t>(0.003 * sr); i < step + static_cast<std::size_t>(0.36 * sr); ++i)
        mono = mono && y[0][i] >= y[0][i - 1] - 1e-12;
    FX_CHECK(mono, "pump recovery monotonic");
}

void testPingPong() {
    std::printf("Ping-Pong\n");
    const double sr = 48000.0;
    const std::size_t step = static_cast<std::size_t>(sr * 0.25);  // 1/8 @ 120
    auto e = makeEngine(sr, {{kDivision, 3}, {kDepth, 100}, {kStereo, 1}});
    const Buffer y = run(e, dc(2, step * 9), 512, Setup{sr, 120.0});
    for (std::size_t k = 1; k < 8; ++k) {
        const std::size_t mid = k * step + step / 4;      // inside the open half
        const std::size_t closed = k * step + 3 * step / 4;  // closed half
        const bool even = k % 2 == 0;
        FX_CHECK(std::abs(y[even ? 0 : 1][mid] - 1.0) < 1e-12, "step %zu active channel open", k);
        FX_CHECK(std::abs(y[even ? 1 : 0][mid]) < 1e-12, "step %zu other channel closed", k);
        FX_CHECK(std::abs(y[0][closed]) < 1e-12 && std::abs(y[1][closed]) < 1e-12, "step %zu both closed", k);
    }
    // Ping-Pong pump: the inactive side stays un-ducked.
    auto p = makeEngine(sr, {{kDivision, 3}, {kDepth, 100}, {kStereo, 1}, {kShape, kShapePump}});
    const Buffer yp = run(p, dc(2, step * 9), 512, Setup{sr, 120.0});
    for (std::size_t k = 1; k < 8; ++k) {
        const std::size_t dip = k * step + static_cast<std::size_t>(0.003 * sr);
        const bool even = k % 2 == 0;
        FX_CHECK(yp[even ? 0 : 1][dip] < 0.05 && std::abs(yp[even ? 1 : 0][dip] - 1.0) < 1e-12, "pump ping-pong step %zu", k);
    }
    // Mono bus: Ping-Pong falls back to the mono pattern (gates every step).
    auto m = makeEngine(sr, {{kDivision, 3}, {kDepth, 100}, {kStereo, 1}});
    const Buffer ym = run(m, dc(1, step * 9), 512, Setup{sr, 120.0});
    for (std::size_t k = 1; k < 8; ++k) FX_CHECK(std::abs(ym[0][k * step + step / 4] - 1.0) < 1e-12, "mono step %zu open", k);
}

void testGateTone() {
    std::printf("Gate-Klang\n");
    const double sr = 48000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 4);
    std::vector<double> x(n);
    const auto a = fxtest::sine(150.0, 0.2, sr, n), b = fxtest::sine(1000.0, 0.2, sr, n), c = fxtest::sine(8000.0, 0.2, sr, n);
    for (std::size_t i = 0; i < n; ++i) x[i] = a[i] + b[i] + c[i];
    struct Expect { int tone; double lo150, hi150, lo1k, hi1k, hi8k; };
    // Levels (dB re input tone) in the closed part with Tiefe 100 %.
    const Expect cases[] = {
        {kToneMute, -1e9, -80, -1e9, -80, -80},
        {kToneTelephone, -1e9, -12, -6, 0.5, -12},
        {kToneDull, -4.5, -1.5, -12, -5, -40},
    };
    for (const auto& cs : cases) {
        auto e = makeEngine(sr, {{kDivision, 1}, {kDepth, 100}, {kGateTone, cs.tone}});
        const Buffer y = run(e, Buffer{x}, 512, Setup{sr, 120.0});  // 1/2 @120 = 1 s steps
        bool openExact = true;
        for (int k = 1; k < 4; ++k)
            openExact = openExact && fxtest::maxAbsDiff(x, y[0], k * 48000 + 2000, k * 48000 + 22000) == 0.0;
        FX_CHECK(openExact, "tone %d: open part untouched", cs.tone);
        const std::size_t c0 = 2 * 48000 + 28800, c1 = 2 * 48000 + 45600;  // 0.6 … 0.95 s of step 2
        const double l150 = fxtest::toDb(fxtest::toneAmplitude(y[0], 150, sr, c0, c1) / 0.2);
        const double l1k = fxtest::toDb(fxtest::toneAmplitude(y[0], 1000, sr, c0, c1) / 0.2);
        const double l8k = fxtest::toDb(fxtest::toneAmplitude(y[0], 8000, sr, c0, c1) / 0.2);
        FX_CHECK(l150 >= cs.lo150 && l150 <= cs.hi150, "tone %d: 150 Hz %.1f dB", cs.tone, l150);
        FX_CHECK(l1k >= cs.lo1k && l1k <= cs.hi1k, "tone %d: 1 kHz %.1f dB", cs.tone, l1k);
        FX_CHECK(l8k <= cs.hi8k, "tone %d: 8 kHz %.1f dB", cs.tone, l8k);
    }
}

void testBlockSizeInvariance() {
    std::printf("block-size invariance\n");
    const double sr = 48000.0;
    const auto x = fxtest::makeVocal(sr, 3.0, 140.0, 11);
    const auto xr = fxtest::makeVocal(sr, 3.0, 140.0, 12);
    std::vector<TimedEvent> ev{at(0, kEnable, 1), at(3000, kDivision, 7), at(20000, kSwing, 60), at(30011, kShape, 2),
                               at(40000, kStereo, 1), at(50000, kGateTone, 1), at(61234, kOffset, 30),
                               at(70000, kDuty, 25), at(80000, kDepth, 40), at(90001, kDivision, 11),
                               at(100000, kSmoothing, 9), at(110000, kShape, 1), at(120000, kGateTone, 2),
                               at(125000, kSync, 0), at(130000, kEnable, 0), at(135000, kEnable, 1)};
    Setup s{sr, 147.0, 2.5};
    for (int precision = 0; precision < 2; ++precision) {
        Buffer ref;
        for (int block : {1, 37, 512, 4096}) {
            ShutterEngine e;
            e.prepare(sr, 4096);
            const Buffer y = precision == 0 ? run<double>(e, Buffer{x, xr}, block, s, ev) : run<float>(e, Buffer{x, xr}, block, s, ev);
            if (ref.empty()) {
                ref = y;
                FX_CHECK(fxtest::allFinite(y), "finite");
                continue;
            }
            const double d = std::max(fxtest::maxAbsDiff(ref[0], y[0]), fxtest::maxAbsDiff(ref[1], y[1]));
            FX_CHECK(d <= 1e-6, "%s block %d differs by %.3g", precision ? "float" : "double", block, d);
        }
    }
}

void testSampleRatesFinite() {
    std::printf("sample rates / stress\n");
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        const auto x = fxtest::noise(1.0, static_cast<std::size_t>(sr * 1.0), 5);
        std::vector<TimedEvent> ev;
        std::mt19937 rng(9);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int k = 0; k < 200; ++k)
            ev.push_back({static_cast<long>(k * sr / 200.0), static_cast<int>(u(rng) * kNumParams) % kNumParams, u(rng)});
        ev.push_back({0, kEnable, 1.0});
        std::stable_sort(ev.begin(), ev.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.sample < b.sample; });
        ShutterEngine e;
        e.prepare(sr, 2048);
        const Buffer y = run(e, Buffer{x, x}, 999, Setup{sr, 174.0, 0.0}, ev);
        FX_CHECK(fxtest::allFinite(y) && fxtest::peak(y[0]) <= 1.0 + 1e-9, "sr %.0f finite and bounded (peak %.3f)", sr, fxtest::peak(y[0]));
        const auto m = e.meters();
        FX_CHECK(m.phase >= 0.0f && m.phase <= 1.0f && m.gainL >= 0.0f && m.gainL <= 1.0f, "meters in range");
    }
}

// ------------------------------------------------------------------------------------------
void render(const std::string& dir) {
    const double sr = 48000.0, bpm = 140.0;
    const auto v = fxtest::makeVocal(sr, 8.0, bpm, 21);
    const Buffer in{v, v};
    fxtest::writeWav(dir + "/shutter_00_dry.wav", in, sr);
    struct Demo { const char* name; std::vector<std::pair<int, double>> params; };
    const Demo demos[] = {
        {"shutter_01_1-16_hart", {{kDivision, 4}, {kDepth, 100}, {kSmoothing, 1}}},
        {"shutter_02_1-8T_swing_telefon", {{kDivision, 6}, {kDepth, 100}, {kSwing, 50}, {kGateTone, 1}, {kDuty, 60}}},
        {"shutter_03_1-4_pumpe", {{kDivision, 2}, {kDepth, 85}, {kShape, 2}, {kDuty, 80}}},
        {"shutter_04_1-16_pingpong", {{kDivision, 4}, {kDepth, 100}, {kStereo, 1}, {kDuty, 70}}},
        {"shutter_05_1-8_weich_dumpf", {{kDivision, 3}, {kDepth, 100}, {kShape, 1}, {kGateTone, 2}}},
        {"shutter_06_1-8P_offset", {{kDivision, 10}, {kDepth, 90}, {kOffset, 50}, {kDuty, 40}}},
    };
    for (const auto& d : demos) {
        auto e = makeEngine(sr, d.params);
        fxtest::writeWav(dir + "/" + d.name + ".wav", run(e, in, 512, Setup{sr, bpm}), sr);
        auto g = makeEngine(sr, d.params);  // gate curve on DC for the plots
        fxtest::writeWav(dir + "/" + d.name + "_gate.wav", run(g, dc(2, static_cast<std::size_t>(sr * 2), 1.0), 512, Setup{sr, bpm}), sr);
    }
    std::printf("rendered shutter demos to %s\n", dir.c_str());
}

} // namespace

int main(int argc, char** argv) {
    testMappings();
    testGateAlignment();
    testTripletDotted();
    testFreeRun();
    testTransportStop();
    testDisabledBitExact();
    testEnableAndDepthClickFree();
    testGlaettung();
    testPump();
    testPingPong();
    testGateTone();
    testBlockSizeInvariance();
    testSampleRatesFinite();
    if (argc >= 3 && std::string(argv[1]) == "--render") render(argv[2]);
    return fxtest::finish("test_shutter");
}
