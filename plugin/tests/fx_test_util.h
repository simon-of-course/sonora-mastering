#pragma once

// Shared helpers for the SONORA Shutter/Drowner unit tests (SDK-free, plain g++).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "dsp_common.h"

namespace fxtest {

using Buffer = std::vector<std::vector<double>>;  // [channel][sample]

inline int& failureCount() {
    static int failures = 0;
    return failures;
}
inline int& checkCount() {
    static int checks = 0;
    return checks;
}

#define FX_CHECK(cond, ...)                                                       \
    do {                                                                          \
        ++fxtest::checkCount();                                                   \
        if (!(cond)) {                                                            \
            ++fxtest::failureCount();                                             \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);       \
            std::printf(__VA_ARGS__);                                             \
            std::printf("\n");                                                    \
        }                                                                         \
    } while (0)

inline int finish(const char* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, checkCount(), failureCount());
    return failureCount() == 0 ? 0 : 1;
}

inline double toDb(double v) { return 20.0 * std::log10(std::max(v, 1e-300)); }

inline double rms(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = SIZE_MAX) {
    end = std::min(end, x.size());
    if (end <= begin) return 0.0;
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}
inline double peak(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = SIZE_MAX) {
    end = std::min(end, x.size());
    double p = 0.0;
    for (std::size_t i = begin; i < end; ++i) p = std::max(p, std::abs(x[i]));
    return p;
}
inline double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b,
                         std::size_t begin = 0, std::size_t end = SIZE_MAX) {
    end = std::min({end, a.size(), b.size()});
    double d = 0.0;
    for (std::size_t i = begin; i < end; ++i) d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}
// Largest |x[n] - x[n-1]| in [begin, end).
inline double maxStep(const std::vector<double>& x, std::size_t begin = 1, std::size_t end = SIZE_MAX) {
    end = std::min(end, x.size());
    double d = 0.0;
    for (std::size_t i = std::max<std::size_t>(begin, 1); i < end; ++i) d = std::max(d, std::abs(x[i] - x[i - 1]));
    return d;
}
// Largest |second difference|, a sensitive click detector for smooth signals.
inline double maxCurvature(const std::vector<double>& x, std::size_t begin = 2, std::size_t end = SIZE_MAX) {
    end = std::min(end, x.size());
    double d = 0.0;
    for (std::size_t i = std::max<std::size_t>(begin, 2); i < end; ++i)
        d = std::max(d, std::abs(x[i] - 2.0 * x[i - 1] + x[i - 2]));
    return d;
}
// Null depth of b against a in dB relative to a's RMS (-300 for an exact match).
inline double nullDb(const std::vector<double>& a, const std::vector<double>& b) {
    std::vector<double> d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
    const double r = rms(d);
    return r <= 0.0 ? -300.0 : toDb(r / std::max(rms(a), 1e-300));
}
inline bool allFinite(const Buffer& b) {
    for (const auto& ch : b)
        for (double v : ch)
            if (!std::isfinite(v)) return false;
    return true;
}
// Amplitude of a sinusoid at freq in [begin, end) (Hann-windowed Goertzel, amplitude-corrected).
inline double toneAmplitude(const std::vector<double>& x, double freq, double sr, std::size_t begin, std::size_t end) {
    end = std::min(end, x.size());
    const std::size_t n = end - begin;
    double re = 0.0, im = 0.0, wsum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * sonora::dsp::kPi * (i + 0.5) / n);
        const double ph = 2.0 * sonora::dsp::kPi * freq * static_cast<double>(begin + i) / sr;
        re += w * x[begin + i] * std::cos(ph);
        im -= w * x[begin + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

inline std::vector<double> sine(double freq, double amp, double sr, std::size_t n, double phase = 0.0) {
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) x[i] = amp * std::sin(2.0 * sonora::dsp::kPi * freq * i / sr + phase);
    return x;
}
inline std::vector<double> noise(double amp, std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> x(n);
    for (auto& v : x) v = amp * dist(rng);
    return x;
}

// Rap-vocal-like test signal: syllables on a 16th grid with rests, PolyBLEP-saw glottal source
// (f0 ≈ 110–170 Hz with phrase intonation and vibrato) through vowel formants, plus breath noise,
// "s" sibilants (5–10 kHz noise) and plosive bursts. Peak normalized to `peakLevel`.
inline std::vector<double> makeVocal(double sr, double seconds, double bpm = 140.0, std::uint32_t seed = 7,
                                     double peakLevel = 0.5) {
    using sonora::dsp::Biquad;
    const std::size_t n = static_cast<std::size_t>(seconds * sr);
    std::vector<double> out(n, 0.0);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<double> gauss(0.0, 1.0);

    struct Vowel { double f1, f2, f3; };
    const Vowel vowels[] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
    const double sixteenth = 60.0 / bpm / 4.0;
    // Flow pattern per bar (1 = syllable start, 0 = rest/hold), repeated with variations.
    const int pattern[16] = {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0};

    Biquad f1, f2, f3, sHp, sLp;
    double phase = 0.0;
    double env = 0.0, envTarget = 0.0;
    double sEnv = 0.0, burst = 0.0;
    double f0 = 140.0;
    std::size_t nextOnset = 0;
    int stepIndex = 0;
    std::size_t sylEnd = 0;
    const double attack = std::exp(-1.0 / (0.004 * sr));
    const double release = std::exp(-1.0 / (0.025 * sr));
    const double sDecay = std::exp(-1.0 / (0.03 * sr));
    sHp.setCoefficients(Biquad::highPass(sr, 5000.0, 0.7));
    sLp.setCoefficients(Biquad::lowPass(sr, std::min(10000.0, 0.45 * sr), 0.7));

    for (std::size_t i = 0; i < n; ++i) {
        if (i >= nextOnset) {
            const int bar = stepIndex / 16;
            const int pos = stepIndex % 16;
            bool start = pattern[pos] == 1;
            if (bar % 4 == 3 && pos >= 12) start = false;  // breath at the end of each phrase
            if (start && uni(rng) < 0.12) start = false;
            if (start) {
                const Vowel& v = vowels[static_cast<std::size_t>(uni(rng) * 4.999)];
                f1.setCoefficients(Biquad::bandPass(sr, v.f1, 5.0));
                f2.setCoefficients(Biquad::bandPass(sr, v.f2, 8.0));
                f3.setCoefficients(Biquad::bandPass(sr, v.f3, 10.0));
                const double phrasePos = (stepIndex % 64) / 64.0;
                f0 = (165.0 - 50.0 * phrasePos) * (1.0 + 0.04 * gauss(rng));
                envTarget = 0.7 + 0.3 * uni(rng);
                const double len = sixteenth * (pos % 2 == 0 ? 1.6 : 0.85) * (0.8 + 0.3 * uni(rng));
                sylEnd = i + static_cast<std::size_t>(len * sr);
                const double r = uni(rng);
                if (r < 0.25) sEnv = 0.9;        // "s"
                else if (r < 0.55) burst = 1.0;  // plosive
            }
            ++stepIndex;
            nextOnset = static_cast<std::size_t>(stepIndex * sixteenth * sr);
        }
        if (i >= sylEnd) envTarget = 0.0;
        env = envTarget + (env - envTarget) * (envTarget > env ? attack : release);

        const double vib = 1.0 + 0.012 * std::sin(2.0 * sonora::dsp::kPi * 5.5 * i / sr);
        const double dt = f0 * vib / sr;
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        double saw = 2.0 * phase - 1.0;
        // PolyBLEP
        if (phase < dt) { const double t = phase / dt; saw -= t + t - t * t - 1.0; }
        else if (phase > 1.0 - dt) { const double t = (phase - 1.0) / dt; saw -= t * t + t + t + 1.0; }
        const double src = -saw + 0.05 * gauss(rng);
        const double voiced = 1.0 * f1.process(src) + 0.55 * f2.process(src) + 0.3 * f3.process(src) + 0.08 * src;

        const double hiss = sLp.process(sHp.process(gauss(rng)));
        sEnv *= sDecay;
        burst *= std::exp(-1.0 / (0.006 * sr));
        out[i] = env * voiced + 0.35 * sEnv * hiss + 0.25 * burst * gauss(rng) * env;
    }
    const double p = peak(out);
    if (p > 0.0)
        for (auto& v : out) v *= peakLevel / p;
    return out;
}

// --- WAV (24-bit PCM) writer ---------------------------------------------------------------
inline bool writeWav(const std::string& path, const Buffer& channels, double sampleRate) {
    if (channels.empty()) return false;
    const std::uint16_t numCh = static_cast<std::uint16_t>(channels.size());
    const std::uint32_t frames = static_cast<std::uint32_t>(channels[0].size());
    const std::uint32_t sr = static_cast<std::uint32_t>(std::lround(sampleRate));
    const std::uint32_t dataBytes = frames * numCh * 3u;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const auto u32 = [f](std::uint32_t v) { unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)}; std::fwrite(b, 1, 4, f); };
    const auto u16 = [f](std::uint16_t v) { unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)}; std::fwrite(b, 1, 2, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(numCh); u32(sr); u32(sr * numCh * 3u); u16(static_cast<std::uint16_t>(numCh * 3u)); u16(24);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    std::vector<unsigned char> buf(static_cast<std::size_t>(dataBytes));
    std::size_t k = 0;
    for (std::uint32_t i = 0; i < frames; ++i)
        for (std::uint16_t c = 0; c < numCh; ++c) {
            const double v = std::clamp(channels[c][i], -1.0, 1.0);
            const std::int32_t s = static_cast<std::int32_t>(std::lround(v * 8388607.0));
            buf[k++] = static_cast<unsigned char>(s & 0xff);
            buf[k++] = static_cast<unsigned char>((s >> 8) & 0xff);
            buf[k++] = static_cast<unsigned char>((s >> 16) & 0xff);
        }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return true;
}

// --- Block runner ------------------------------------------------------------------------------
// Event at an absolute sample index (converted to per-block offsets by the runner).
struct TimedEvent {
    long sample;
    int id;
    double normalized;
};

// Runs `processBlock(const T* const* in, T* const* out, int channels, int frames, long blockStart,
// const EventT* events, int numEvents)` over `input` in blocks of `blockSize`, converting
// the timed events to block-relative offsets. Sample type T selects 32/64-bit processing.
template <typename T, typename EventT, typename Fn>
Buffer runBlocks(const Buffer& input, int blockSize, const std::vector<TimedEvent>& events, Fn&& processBlock) {
    const int channels = static_cast<int>(input.size());
    const long total = static_cast<long>(input[0].size());
    std::vector<std::vector<T>> in(channels, std::vector<T>(static_cast<std::size_t>(blockSize)));
    std::vector<std::vector<T>> out(channels, std::vector<T>(static_cast<std::size_t>(blockSize)));
    std::vector<const T*> inPtr(static_cast<std::size_t>(channels));
    std::vector<T*> outPtr(static_cast<std::size_t>(channels));
    Buffer result(channels, std::vector<double>(static_cast<std::size_t>(total)));
    std::vector<EventT> blockEvents;
    blockEvents.reserve(events.size());
    std::size_t ev = 0;
    for (long start = 0; start < total; start += blockSize) {
        const int frames = static_cast<int>(std::min<long>(blockSize, total - start));
        for (int c = 0; c < channels; ++c) {
            for (int i = 0; i < frames; ++i)
                in[c][static_cast<std::size_t>(i)] = static_cast<T>(input[c][static_cast<std::size_t>(start + i)]);
            inPtr[static_cast<std::size_t>(c)] = in[c].data();
            outPtr[static_cast<std::size_t>(c)] = out[c].data();
        }
        blockEvents.clear();
        while (ev < events.size() && events[ev].sample < start + frames) {
            blockEvents.push_back(EventT{static_cast<int>(std::max<long>(0, events[ev].sample - start)), events[ev].id,
                                         events[ev].normalized});
            ++ev;
        }
        processBlock(inPtr.data(), outPtr.data(), channels, frames, start, blockEvents.data(),
                     static_cast<int>(blockEvents.size()));
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < frames; ++i)
                result[c][static_cast<std::size_t>(start + i)] = static_cast<double>(out[c][static_cast<std::size_t>(i)]);
    }
    return result;
}

} // namespace fxtest
