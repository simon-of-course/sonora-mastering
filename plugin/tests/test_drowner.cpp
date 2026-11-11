// Unit tests + demo renders for the SONORA Drowner engine.
// Build: g++ -std=c++17 -O2 -Wall -Wextra -Werror -I plugin/src -I plugin/src/dsp plugin/tests/test_drowner.cpp
// Run:   ./a.out                 (tests)
//        ./a.out --render <dir>  (tests + demo WAVs)

#include "drowner_engine.h"
#include "fx_test_util.h"

#include <cstdio>
#include <string>

using namespace sonora::dsp::drowner;
using fxtest::Buffer;
using fxtest::TimedEvent;

namespace {

void set(DrownerEngine& e, int id, double plain) { e.setParameter(id, toNormalized(id, plain)); }
TimedEvent at(long sample, int id, double plain) { return {sample, id, toNormalized(id, plain)}; }

DrownerEngine makeEngine(double sr, const std::vector<std::pair<int, double>>& params) {
    DrownerEngine e;
    e.prepare(sr, 4096);
    for (const auto& p : params) set(e, p.first, p.second);
    e.reset();  // settled smoothers; the depth envelope always starts at 0
    return e;
}

template <typename T = double>
Buffer run(DrownerEngine& eng, const Buffer& in, int block, const std::vector<TimedEvent>& events = {}) {
    return fxtest::runBlocks<T, ParamEvent>(
        in, block, events, [&](const T* const* i, T* const* o, int ch, int n, long, const ParamEvent* ev, int ne) {
            eng.process(i, o, ch, n, ev, ne);
        });
}

std::vector<double> mixOf(std::initializer_list<std::vector<double>> parts) {
    std::vector<double> x(parts.begin()->size(), 0.0);
    for (const auto& p : parts)
        for (std::size_t i = 0; i < x.size(); ++i) x[i] += p[i];
    return x;
}

// ------------------------------------------------------------------------------------------
void testMappings() {
    std::printf("mappings\n");
    for (int id = 0; id < kNumParams; ++id) {
        const auto& p = kParamInfo[static_cast<std::size_t>(id)];
        FX_CHECK(std::abs(toPlain(id, defaultNormalized(id)) - p.defaultPlain) < 1e-9, "id %d default", id);
        FX_CHECK(std::abs(toPlain(id, 0.0) - p.minPlain) < 1e-12 && std::abs(toPlain(id, 1.0) - p.maxPlain) < 1e-9, "id %d range", id);
        for (double n : {0.1, 0.37, 0.5, 0.93})
            FX_CHECK(std::abs(toNormalized(id, toPlain(id, n)) - n) < 1e-12, "id %d roundtrip %.2f", id, n);
    }
    FX_CHECK(std::abs(toPlain(kReturnTime, 0.5) - (0.2 + 0.5 * 5.8)) < 1e-12, "v1 return-time mapping kept");
    FX_CHECK(std::abs(toPlain(kDiveTime, 0.5) - std::sqrt(0.01 * 2.0)) < 1e-12, "dive time is logarithmic");
    FX_CHECK(std::abs(cutoffForDepth(1.0, 48000.0) - kMinCutoffHz) < 1e-9 && std::abs(cutoffForDepth(0.0, 48000.0) - 21000.0) < 1e-9
                 && std::abs(cutoffForDepth(0.0, 44100.0) - 21000.0) < 1e-9, "cutoff map");
}

void testDepthZeroNull() {
    std::printf("depth-0 null\n");
    const double sr = 48000.0;
    const auto l = fxtest::makeVocal(sr, 3.0, 140.0, 1), r = fxtest::makeVocal(sr, 3.0, 140.0, 2);
    auto e = makeEngine(sr, {{kResonance, 100}, {kWobble, 100}, {kReverb, 100}, {kBody, 6}, {kOutput, 6}, {kMix, 100}});
    const Buffer y = run(e, Buffer{l, r}, 512);
    const double nd = std::max(fxtest::nullDb(l, y[0]), fxtest::nullDb(r, y[1]));
    FX_CHECK(nd < -100.0, "stereo null %.1f dB", nd);
    FX_CHECK(y[0] == l && y[1] == r, "bit-exact at depth 0");
    auto m = makeEngine(sr, {});
    const Buffer ym = run<float>(m, Buffer{l}, 333);
    std::vector<double> lf(l.size());
    for (std::size_t i = 0; i < l.size(); ++i) lf[i] = static_cast<float>(l[i]);
    FX_CHECK(ym[0] == lf, "mono float bit-exact at depth 0");
}

void testDepthOneAttenuation() {
    std::printf("depth-1 attenuation\n");
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (double res : {0.0, 30.0, 100.0}) {
            const std::size_t n = static_cast<std::size_t>(sr * 2.0);
            const auto x = mixOf({fxtest::sine(200.0, 0.1, sr, n), fxtest::sine(6500.0, 0.1, sr, n),
                                  fxtest::sine(10000.0, 0.1, sr, n), fxtest::sine(15000.0, 0.1, sr, n)});
            auto e = makeEngine(sr, {{kDiveTime, 0.01}, {kResonance, res}, {kWobble, 100}});
            const Buffer y = run(e, Buffer{x, x}, 512, {at(0, kDepth, 100)});
            const std::size_t b = n / 2, en = n;
            for (double f : {6500.0, 10000.0, 15000.0}) {
                const double att = fxtest::toDb(std::max(fxtest::toneAmplitude(y[0], f, sr, b, en), fxtest::toneAmplitude(y[1], f, sr, b, en)) / 0.1);
                FX_CHECK(att < -30.0, "sr %.0f res %.0f: %.0f Hz at %.1f dB", sr, res, f, att);
            }
            const double pass = fxtest::toDb(fxtest::toneAmplitude(y[0], 200.0, sr, b, en) / 0.1);
            FX_CHECK(pass > -3.0 && pass < 13.0, "sr %.0f res %.0f: 200 Hz passband %.1f dB", sr, res, pass);
        }
    }
}

void testResonanceStable() {
    std::printf("resonance stability under fast automation\n");
    for (double sr : {44100.0, 96000.0, 192000.0}) {
        const std::size_t n = static_cast<std::size_t>(sr * 4.0);
        const auto v = fxtest::makeVocal(sr, 4.0, 140.0, 5, 0.7);
        const auto nz = fxtest::noise(0.3, n, 6);
        const auto x = mixOf({v, nz});
        auto e = makeEngine(sr, {{kResonance, 100}, {kWobble, 100}, {kDiveTime, 0.01}, {kReturnTime, 0.2}, {kReverb, 100}, {kBody, 6}, {kOutput, 6}});
        std::vector<TimedEvent> ev;
        const long step = static_cast<long>(0.03 * sr);
        for (long k = 0; k * step < static_cast<long>(n); ++k) {
            ev.push_back({k * step, kDepth, k % 2 ? 0.0 : 1.0});
            ev.push_back({k * step + step / 3, kResonance, (k % 4) / 3.0});
        }
        const Buffer y = run(e, Buffer{x, x}, 256, ev);
        const double pk = std::max(fxtest::peak(y[0]), fxtest::peak(y[1]));
        FX_CHECK(fxtest::allFinite(y) && pk < 6.0, "sr %.0f: finite, peak %.2f (input %.2f)", sr, pk, fxtest::peak(x));
    }
}

void testTiming() {
    std::printf("dive / return timing\n");
    const double sr = 48000.0;
    for (double dive : {0.01, 0.05, 0.5, 2.0}) {
        for (double ret : {0.2, 2.0, 6.0}) {
            auto e = makeEngine(sr, {{kDiveTime, dive}, {kReturnTime, ret}, {kDepth, 0}});
            set(e, kDepth, 100);
            float in = 0.0f, out = 0.0f;
            const float* ip = &in;
            float* op = &out;
            long t90 = -1;
            for (long i = 0; i < static_cast<long>((dive * 3.0 + 0.05) * sr); ++i) {
                e.process(&ip, &op, 1, 1);
                if (t90 < 0 && e.smoothedDepth() >= 0.9) t90 = i + 1;
            }
            const double diveErr = std::abs(t90 / sr - dive) / dive;
            FX_CHECK(t90 > 0 && diveErr < 0.01, "dive %.2f s: 90 %% after %.4f s", dive, t90 / sr);
            const double start = e.smoothedDepth();
            set(e, kDepth, 0);
            long t10 = -1;
            for (long i = 0; i < static_cast<long>((ret * 1.5 + 0.05) * sr) && t10 < 0; ++i) {
                e.process(&ip, &op, 1, 1);
                if (e.smoothedDepth() <= 1.0 - 0.9 * start) t10 = i + 1;
            }
            const double retErr = std::abs(t10 / sr - ret) / ret;
            FX_CHECK(t10 > 0 && retErr < 0.01, "return %.2f s: 90 %% after %.4f s", ret, t10 / sr);
        }
    }
    // Meter reports the envelope and cutoff.
    auto e = makeEngine(sr, {{kDiveTime, 0.01}, {kWobble, 0}});
    const auto x = fxtest::noise(0.1, 24000, 3);
    run(e, Buffer{x}, 512, {at(0, kDepth, 50)});
    FX_CHECK(std::abs(e.meters().depth - 0.5f) < 1e-4f, "meter depth %.4f", e.meters().depth);
    FX_CHECK(std::abs(e.meters().cutoffHz - cutoffForDepth(0.5, sr)) < 1.0, "meter cutoff %.1f", e.meters().cutoffHz);
}

void testReverbTail() {
    std::printf("reverb tail\n");
    const double sr = 48000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 9.0);
    auto burst = fxtest::makeVocal(sr, 9.0, 140.0, 8, 0.6);
    const auto later = burst;
    for (std::size_t i = static_cast<std::size_t>(sr); i < n; ++i) burst[i] = 0.0;  // 1 s of vocal, then silence
    for (std::size_t i = static_cast<std::size_t>(7.0 * sr); i < n; ++i) burst[i] = later[i];  // vocal again at 7 s
    auto e = makeEngine(sr, {{kDiveTime, 0.01}, {kReturnTime, 0.2}, {kReverb, 100}});
    const Buffer y = run(e, Buffer{burst, burst}, 512, {at(0, kDepth, 100), at(static_cast<long>(sr), kDepth, 0)});
    std::vector<double> levels;
    for (double t = 1.3; t < 3.0; t += 0.1)
        levels.push_back(fxtest::rms(y[0], static_cast<std::size_t>(t * sr), static_cast<std::size_t>((t + 0.1) * sr)));
    bool decreasing = true;
    for (std::size_t i = 1; i < levels.size(); ++i) decreasing = decreasing && levels[i] < levels[i - 1] * 1.02;
    const double drop = fxtest::toDb(levels.back() / levels.front());
    FX_CHECK(levels.front() > 1e-3, "tail audible after the return (%.1f dBFS)", fxtest::toDb(levels.front()));
    FX_CHECK(decreasing, "tail decays smoothly");
    FX_CHECK(drop < -40.0 && drop > -150.0, "tail drop over 1.6 s: %.1f dB", drop);
    // No abrupt cut while depth returns to 0 (envelope reaches 0 around 1.2 s).
    const double cutStep = fxtest::maxStep(y[0], static_cast<std::size_t>(1.15 * sr), static_cast<std::size_t>(2.0 * sr));
    FX_CHECK(cutStep < 0.05, "no step during return (%.4f)", cutStep);
    // Clean again afterwards.
    FX_CHECK(fxtest::peak(y[0], static_cast<std::size_t>(6.0 * sr), static_cast<std::size_t>(7.0 * sr)) == 0.0, "silence after tail");
    FX_CHECK(fxtest::maxAbsDiff(burst, y[0], static_cast<std::size_t>(7.0 * sr), n) == 0.0, "bit-exact dry again after the tail");
}

void testReturnToDry() {
    std::printf("return to bit-exact dry\n");
    const double sr = 48000.0;
    const auto x = fxtest::makeVocal(sr, 4.0, 140.0, 3, 0.6);
    auto e = makeEngine(sr, {{kDiveTime, 0.05}, {kReturnTime, 1.0}, {kReverb, 0}});
    const Buffer y = run(e, Buffer{x, x}, 256, {at(0, kDepth, 100), at(static_cast<long>(sr), kDepth, 0)});
    // 1 % depth is reached 2 return times after the release, the rest is finished within ~0.15 s.
    const auto from = static_cast<std::size_t>(3.2 * sr);
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], from, x.size()) == 0.0, "bit-exact dry 2.2 s after a 1 s return");
    FX_CHECK(fxtest::maxStep(y[0], static_cast<std::size_t>(2.5 * sr), from) < 2.0 * fxtest::maxStep(x, static_cast<std::size_t>(2.5 * sr), from),
             "final approach is click-free");
}

void testBypass() {
    std::printf("host bypass\n");
    const double sr = 48000.0;
    const auto x = fxtest::makeVocal(sr, 3.0, 140.0, 4, 0.6);
    const std::vector<std::pair<int, double>> params{{kDiveTime, 0.01}, {kReverb, 60}, {kResonance, 80}};
    const std::vector<TimedEvent> depth{at(0, kDepth, 100)};
    auto ref = makeEngine(sr, params);
    const Buffer wet = run(ref, Buffer{x, x}, 256, depth);
    auto ev = depth;
    for (const auto& b : {at(48000, kBypass, 1), at(96000, kBypass, 0), at(120000, kBypass, 1), at(120240, kBypass, 0)})
        ev.push_back(b);
    auto e = makeEngine(sr, params);
    const Buffer y = run(e, Buffer{x, x}, 173, ev);
    const auto fade = static_cast<std::size_t>(0.01 * sr);
    FX_CHECK(fxtest::maxAbsDiff(wet[0], y[0], 0, 48000) == 0.0, "identical to unbypassed before the bypass");
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], 48000 + fade + 1, 96000) == 0.0 && fxtest::maxAbsDiff(x, y[1], 48000 + fade + 1, 96000) == 0.0,
             "bypassed -> bit-exact dry after 10 ms (reverb tail cut by the fade)");
    FX_CHECK(fxtest::maxAbsDiff(x, y[0], 48000, 48000 + fade / 2) > 1e-3, "bypass is a crossfade, not a jump");
    FX_CHECK(fxtest::maxAbsDiff(wet[0], y[0], 96000 + fade + 1, 120000) < 1e-9, "un-bypass resumes the running wet state");
    const double bound = 1.2 * std::max(fxtest::maxStep(x), fxtest::maxStep(wet[0]));
    const double got = fxtest::maxStep(y[0]);
    FX_CHECK(got <= bound, "bypass toggling click-free: step %.4f (bound %.4f)", got, bound);
    FX_CHECK(defaultNormalized(kBypass) == 0.0, "bypass default off");
}

void testSampleRatesFinite() {
    std::printf("sample rates / stress\n");
    for (double sr : {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0}) {
        const std::size_t n = static_cast<std::size_t>(sr * 1.5);
        auto x = fxtest::noise(1.0, n, 4);
        for (std::size_t i = n / 3; i < n / 2; ++i) x[i] = 1e-35;   // denormal-range input
        for (std::size_t i = n / 2; i < 2 * n / 3; ++i) x[i] = 1.0;  // DC
        x[2 * n / 3 + 10] = -1.0;
        std::vector<TimedEvent> ev;
        std::mt19937 rng(17);
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int k = 0; k < 300; ++k)
            ev.push_back({static_cast<long>(k * n / 300), static_cast<int>(u(rng) * kNumParams) % kNumParams, u(rng)});
        for (int k = 0; k < 10; ++k) ev.push_back({static_cast<long>(k * n / 10), kDepth, k % 2 ? 0.0 : 1.0});
        std::stable_sort(ev.begin(), ev.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.sample < b.sample; });
        DrownerEngine e;
        e.prepare(sr, 2048);
        const Buffer y = run(e, Buffer{x, x}, 1000, ev);
        const double pk = fxtest::peak(y[0]);
        FX_CHECK(fxtest::allFinite(y) && pk < 8.0, "sr %.0f finite, peak %.2f", sr, pk);
        const auto m = e.meters();
        FX_CHECK(std::isfinite(m.cutoffHz) && m.cutoffHz >= 20.0f && m.cutoffHz <= 0.5 * sr, "sr %.0f meter cutoff %.1f", sr, m.cutoffHz);
    }
}

void testBlockSizeInvariance() {
    std::printf("block-size invariance\n");
    const double sr = 48000.0;
    const auto l = fxtest::makeVocal(sr, 3.0, 140.0, 31), r = fxtest::makeVocal(sr, 3.0, 140.0, 32);
    std::vector<TimedEvent> ev{at(100, kDepth, 80), at(20000, kWobble, 100), at(30001, kResonance, 90), at(40000, kReverb, 70),
                               at(50000, kDepth, 20), at(60000, kBody, 6), at(70007, kMix, 60), at(80000, kOutput, -6),
                               at(90000, kDepth, 100), at(91000, kDiveTime, 0.5), at(100000, kDepth, 0), at(120000, kDepth, 60)};
    for (int precision = 0; precision < 2; ++precision) {
        Buffer ref;
        for (int block : {1, 37, 512, 4096}) {
            DrownerEngine e;
            e.prepare(sr, 4096);
            const Buffer y = precision == 0 ? run<double>(e, Buffer{l, r}, block, ev) : run<float>(e, Buffer{l, r}, block, ev);
            if (ref.empty()) {
                ref = y;
                continue;
            }
            const double d = std::max(fxtest::maxAbsDiff(ref[0], y[0]), fxtest::maxAbsDiff(ref[1], y[1]));
            FX_CHECK(d <= 1e-6, "%s block %d differs by %.3g", precision ? "float" : "double", block, d);
        }
    }
}

void testAutomationClickFree() {
    std::printf("automation sweep click-free\n");
    const double sr = 48000.0;
    const std::size_t n = static_cast<std::size_t>(sr * 6.0);
    const auto x = mixOf({fxtest::sine(110.0, 0.3, sr, n), fxtest::sine(220.0, 0.15, sr, n, 0.3)});
    const double inCurv = fxtest::maxCurvature(x), inStep = fxtest::maxStep(x);
    for (double dive : {0.01, 0.05}) {
        auto e = makeEngine(sr, {{kDiveTime, dive}, {kReturnTime, 0.2}, {kResonance, 60}});
        std::vector<TimedEvent> ev;
        // Stepped "drawn" automation (10 ms steps) up and down, then hard jumps.
        for (int k = 0; k <= 100; ++k) ev.push_back({static_cast<long>(0.2 * sr + k * 0.01 * sr), kDepth, k / 100.0});
        for (int k = 0; k <= 100; ++k) ev.push_back({static_cast<long>(1.4 * sr + k * 0.01 * sr), kDepth, 1.0 - k / 100.0});
        for (int k = 0; k < 12; ++k) ev.push_back({static_cast<long>(2.6 * sr + k * 0.25 * sr), kDepth, k % 2 ? 0.0 : 1.0});
        for (int k = 0; k < 8; ++k) ev.push_back({static_cast<long>(2.7 * sr + k * 0.33 * sr), kMix, k % 2 ? 0.2 : 1.0});
        std::sort(ev.begin(), ev.end(), [](const TimedEvent& a, const TimedEvent& b) { return a.sample < b.sample; });
        const Buffer y = run(e, Buffer{x, x}, 512, ev);
        const double curv = std::max(fxtest::maxCurvature(y[0]), fxtest::maxCurvature(y[1]));
        const double step = std::max(fxtest::maxStep(y[0]), fxtest::maxStep(y[1]));
        std::printf("  dive %.2f: curvature %.2fx input, step %.2fx input\n", dive, curv / inCurv, step / inStep);
        FX_CHECK(curv < 6.0 * inCurv, "dive %.2f: max curvature %.3g vs input %.3g", dive, curv, inCurv);
        FX_CHECK(step < 3.5 * inStep, "dive %.2f: max step %.3g vs input %.3g", dive, step, inStep);
    }
}

// ------------------------------------------------------------------------------------------
void render(const std::string& dir) {
    const double sr = 48000.0;
    const auto v = fxtest::makeVocal(sr, 10.0, 140.0, 21);
    const Buffer in{v, v};
    fxtest::writeWav(dir + "/drowner_00_dry.wav", in, sr);
    {   // Classic drop: dive at 2 s, underwater until 5 s, slow return (2 s).
        auto e = makeEngine(sr, {});
        fxtest::writeWav(dir + "/drowner_01_drop_automation.wav", run(e, in, 512, {at(96000, kDepth, 100), at(240000, kDepth, 0)}), sr);
    }
    {
        auto e = makeEngine(sr, {{kResonance, 100}, {kWobble, 100}, {kReverb, 60}});
        fxtest::writeWav(dir + "/drowner_02_full_res_wobble.wav", run(e, in, 512, {at(0, kDepth, 100)}), sr);
    }
    {
        auto e = makeEngine(sr, {});
        fxtest::writeWav(dir + "/drowner_03_half.wav", run(e, in, 512, {at(0, kDepth, 50)}), sr);
    }
    {   // Slow sweep on noise to visualize the filter trajectory.
        const auto nz = fxtest::noise(0.25, static_cast<std::size_t>(sr * 10.0), 77);
        auto e = makeEngine(sr, {{kDiveTime, 2.0}, {kReturnTime, 3.0}, {kReverb, 0}, {kWobble, 35}});
        fxtest::writeWav(dir + "/drowner_04_noise_sweep.wav", run(e, Buffer{nz, nz}, 512, {at(48000, kDepth, 100), at(288000, kDepth, 0)}), sr);
    }
    std::printf("rendered drowner demos to %s\n", dir.c_str());
}

} // namespace

int main(int argc, char** argv) {
    testMappings();
    testDepthZeroNull();
    testDepthOneAttenuation();
    testResonanceStable();
    testTiming();
    testReverbTail();
    testReturnToDry();
    testBypass();
    testSampleRatesFinite();
    testBlockSizeInvariance();
    testAutomationClickFree();
    if (argc >= 3 && std::string(argv[1]) == "--render") render(argv[2]);
    return fxtest::finish("test_drowner");
}
