#pragma once

// Test/offline utilities for SONORA Master (SDK-free): WAV I/O, a deterministic synthetic rap test
// mix, metrics (LUFS, true peak, band RMS, crest, aliasing) and an engine render helper.
// Not real-time code: allocation is fine here.

#include "loudness.h"
#include "master_engine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sonora::test {

using sonora::dsp::kPi;

struct Audio {
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> ch;
    int channels() const { return static_cast<int>(ch.size()); }
    int frames() const { return ch.empty() ? 0 : static_cast<int>(ch[0].size()); }
    void resize(int channels, int frames) {
        ch.assign(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    }
    std::vector<const float*> ptrs() const {
        std::vector<const float*> p;
        for (const auto& c : ch) p.push_back(c.data());
        return p;
    }
    std::vector<float*> ptrs() {
        std::vector<float*> p;
        for (auto& c : ch) p.push_back(c.data());
        return p;
    }
};

// ------------------------------------------------------------------------------------------------
// WAV I/O (PCM 16/24/32-bit int, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE)

inline bool readWav(const std::string& path, Audio& out, std::string* error = nullptr) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open file");
    std::vector<unsigned char> data;
    {
        unsigned char buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
    }
    auto u16 = [&](std::size_t o) { return static_cast<unsigned>(data[o] | (data[o + 1] << 8)); };
    auto u32 = [&](std::size_t o) {
        return static_cast<std::uint32_t>(data[o]) | (static_cast<std::uint32_t>(data[o + 1]) << 8)
            | (static_cast<std::uint32_t>(data[o + 2]) << 16) | (static_cast<std::uint32_t>(data[o + 3]) << 24);
    };
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        return fail("not a RIFF/WAVE file");
    std::size_t pos = 12;
    unsigned format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    const unsigned char* pcm = nullptr;
    std::size_t pcmBytes = 0;
    while (pos + 8 <= data.size()) {
        const std::uint32_t size = u32(pos + 4);
        const std::size_t body = pos + 8;
        if (std::memcmp(&data[pos], "fmt ", 4) == 0 && body + 16 <= data.size()) {
            format = u16(body);
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
            if (format == 0xFFFE && size >= 40 && body + 26 <= data.size()) format = u16(body + 24);
        } else if (std::memcmp(&data[pos], "data", 4) == 0) {
            pcm = &data[body];
            pcmBytes = std::min<std::size_t>(size, data.size() - body);
        }
        pos = body + size + (size & 1u);
    }
    if (!pcm || channels == 0 || rate == 0) return fail("missing fmt or data chunk");
    const bool isFloat = format == 3;
    if (!(format == 1 || isFloat)) return fail("unsupported WAV format");
    if (!((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (isFloat && (bits == 32 || bits == 64))))
        return fail("unsupported bit depth");
    const std::size_t bytesPer = bits / 8;
    const int frames = static_cast<int>(pcmBytes / (bytesPer * channels));
    out.sampleRate = rate;
    out.resize(static_cast<int>(channels), frames);
    for (int i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            const unsigned char* p = pcm + (static_cast<std::size_t>(i) * channels + c) * bytesPer;
            double v = 0.0;
            if (isFloat && bits == 32) { float x; std::memcpy(&x, p, 4); v = x; }
            else if (isFloat) { double x; std::memcpy(&x, p, 8); v = x; }
            else if (bits == 16) v = static_cast<std::int16_t>(p[0] | (p[1] << 8)) / 32768.0;
            else if (bits == 24) {
                std::int32_t x = p[0] | (p[1] << 8) | (p[2] << 16);
                if (x & 0x800000) x -= 0x1000000;
                v = x / 8388608.0;
            } else {
                std::int32_t x;
                std::memcpy(&x, p, 4);
                v = x / 2147483648.0;
            }
            out.ch[c][static_cast<std::size_t>(i)] = static_cast<float>(v);
        }
    }
    return true;
}

// bits: 16, 24 (PCM, TPDF-free rounding with clipping) or 32 (IEEE float).
inline bool writeWav(const std::string& path, const Audio& a, int bits = 24) {
    if (!(bits == 16 || bits == 24 || bits == 32) || a.channels() == 0) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned channels = static_cast<unsigned>(a.channels());
    const std::uint32_t frames = static_cast<std::uint32_t>(a.frames());
    const unsigned bytesPer = static_cast<unsigned>(bits / 8);
    const std::uint32_t dataBytes = frames * channels * bytesPer;
    std::vector<unsigned char> h;
    auto put16 = [&](unsigned v) { h.push_back(v & 0xFF); h.push_back((v >> 8) & 0xFF); };
    auto put32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) h.push_back((v >> (8 * k)) & 0xFF); };
    auto tag = [&](const char* t) { h.insert(h.end(), t, t + 4); };
    tag("RIFF"); put32(36 + dataBytes); tag("WAVE");
    tag("fmt "); put32(16); put16(bits == 32 ? 3 : 1); put16(channels);
    put32(static_cast<std::uint32_t>(std::lround(a.sampleRate)));
    put32(static_cast<std::uint32_t>(std::lround(a.sampleRate)) * channels * bytesPer);
    put16(channels * bytesPer); put16(static_cast<unsigned>(bits));
    tag("data"); put32(dataBytes);
    std::fwrite(h.data(), 1, h.size(), f);
    std::vector<unsigned char> body(dataBytes);
    std::size_t o = 0;
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            const double v = a.ch[c][i];
            if (bits == 32) {
                const float x = static_cast<float>(v);
                std::memcpy(&body[o], &x, 4);
            } else if (bits == 16) {
                const auto x = static_cast<std::int32_t>(std::lround(std::clamp(v, -1.0, 32767.0 / 32768.0) * 32768.0));
                body[o] = x & 0xFF; body[o + 1] = (x >> 8) & 0xFF;
            } else {
                const auto x = static_cast<std::int32_t>(std::lround(std::clamp(v, -1.0, 8388607.0 / 8388608.0) * 8388608.0));
                body[o] = x & 0xFF; body[o + 1] = (x >> 8) & 0xFF; body[o + 2] = (x >> 16) & 0xFF;
            }
            o += bytesPer;
        }
    }
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return true;
}

// ------------------------------------------------------------------------------------------------
// Metrics

inline double lufsIntegrated(const Audio& a) {
    auto p = a.ptrs();
    return dsp::measureIntegratedLufs(p.data(), a.channels(), a.frames(), a.sampleRate);
}

inline double truePeakDb(const Audio& a) {
    auto p = a.ptrs();
    return dsp::measureTruePeakDb(p.data(), a.channels(), a.frames());
}

inline double samplePeakDb(const Audio& a) {
    double pk = 0.0;
    for (const auto& c : a.ch) for (float v : c) pk = std::max(pk, std::abs(static_cast<double>(v)));
    return dsp::gainToDb(pk);
}

// Independent high-precision true peak (16x, 32 taps per phase Kaiser sinc) for verification.
inline double referenceTruePeakDb(const Audio& a) {
    constexpr int kOs = 16, kHalf = 16; // +-16 input samples
    // taps[p][k]: weight of input sample j = i - kHalf + 1 + k for the value at time i + p/kOs.
    std::vector<std::vector<double>> taps(kOs, std::vector<double>(2 * kHalf));
    for (int p = 0; p < kOs; ++p) {
        double sum = 0.0;
        for (int k = 0; k < 2 * kHalf; ++k) {
            const double t = static_cast<double>(p) / kOs + kHalf - 1 - k;
            const double r = t / (kHalf + 0.5);
            const double win = std::abs(r) < 1.0 ? dsp::besselI0(9.0 * std::sqrt(1.0 - r * r)) / dsp::besselI0(9.0) : 0.0;
            taps[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)] = dsp::sincPi(t) * win;
            sum += taps[static_cast<std::size_t>(p)][static_cast<std::size_t>(k)];
        }
        for (auto& v : taps[static_cast<std::size_t>(p)]) v /= sum;
    }
    double pk = 0.0;
    for (const auto& c : a.ch) {
        const int n = static_cast<int>(c.size());
        for (int i = 0; i < n; ++i) {
            pk = std::max(pk, std::abs(static_cast<double>(c[static_cast<std::size_t>(i)])));
            // Interpolate only where the full kernel support lies inside the signal: a hard cut at
            // the file edges is a step whose Gibbs overshoot does not exist in the real signal.
            if (i - kHalf + 1 < 0 || i + kHalf >= n) continue;
            for (int p = 1; p < kOs; ++p) {
                double y = 0.0;
                const auto& tp = taps[static_cast<std::size_t>(p)];
                for (int k = 0; k < 2 * kHalf; ++k) {
                    const int idx = i - kHalf + 1 + k;
                    if (idx >= 0 && idx < n) y += tp[static_cast<std::size_t>(k)] * c[static_cast<std::size_t>(idx)];
                }
                pk = std::max(pk, std::abs(y));
            }
        }
    }
    return dsp::gainToDb(pk);
}

inline double rmsDb(const Audio& a, int start = 0, int end = -1) {
    if (end < 0) end = a.frames();
    double s = 0.0;
    std::size_t n = 0;
    for (const auto& c : a.ch) {
        for (int i = start; i < end; ++i) { const double v = c[static_cast<std::size_t>(i)]; s += v * v; ++n; }
    }
    return n ? 10.0 * std::log10(s / static_cast<double>(n) + 1e-30) : -300.0;
}

// Peak-to-RMS ratio (dB) of the channel-summed signal.
inline double crestDb(const Audio& a) { return samplePeakDb(a) - rmsDb(a); }

// Band RMS (dB) using a 4th-order Butterworth band (2x HP biquad + 2x LP biquad).
inline double bandRmsDb(const Audio& a, double fLo, double fHi, int start = 0, int end = -1) {
    if (end < 0) end = a.frames();
    double s = 0.0;
    std::size_t n = 0;
    for (const auto& c : a.ch) {
        dsp::Biquad h1 = dsp::Biquad::highPass(a.sampleRate, fLo), h2 = h1;
        dsp::Biquad l1 = dsp::Biquad::lowPass(a.sampleRate, fHi), l2 = l1;
        for (int i = 0; i < end; ++i) {
            const double v = l2.process(l1.process(h2.process(h1.process(c[static_cast<std::size_t>(i)]))));
            if (i >= start) { s += v * v; ++n; }
        }
    }
    return n ? 10.0 * std::log10(s / static_cast<double>(n) + 1e-30) : -300.0;
}

inline double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t offsetB = 0) {
    double m = 0.0;
    for (std::size_t i = 0; i + offsetB < b.size() && i < a.size(); ++i)
        m = std::max(m, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i + offsetB])));
    return m;
}

inline bool allFinite(const Audio& a) {
    for (const auto& c : a.ch) for (float v : c) if (!std::isfinite(v)) return false;
    return true;
}

inline double dcOffset(const Audio& a) {
    double m = 0.0;
    for (const auto& c : a.ch) {
        double s = 0.0;
        for (float v : c) s += v;
        m = std::max(m, std::abs(s / std::max<std::size_t>(1, c.size())));
    }
    return m;
}

// In-place iterative radix-2 FFT.
inline void fft(std::vector<std::complex<double>>& x) {
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Power spectrum (Blackman-Harris 4-term window) of signal[start, start+n), n power of two.
inline std::vector<double> powerSpectrum(const std::vector<float>& s, std::size_t start, std::size_t n) {
    std::vector<std::complex<double>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        x[i] = (start + i < s.size() ? s[start + i] : 0.0f) * w;
    }
    fft(x);
    std::vector<double> p(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) p[k] = std::norm(x[k]);
    return p;
}

// Aliasing measurement: energy of all non-harmonic components in [fLo, fHi] relative to the
// fundamental (dBc). Harmonics k*f0 (k >= 1) +- 6 bins are excluded.
inline double aliasingDbc(const std::vector<float>& s, double sr, double f0, std::size_t start,
                          std::size_t n = 65536, double fLo = 20.0, double fHi = 20000.0) {
    const auto p = powerSpectrum(s, start, n);
    const double binHz = sr / static_cast<double>(n);
    const int guard = 6;
    double fund = 0.0, other = 0.0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        const double h = f / f0;
        const double nearest = std::round(h);
        const bool harmonic = nearest >= 1.0 && std::abs(f - nearest * f0) <= guard * binHz;
        if (nearest == 1.0 && harmonic) { fund += p[k]; continue; }
        if (f < fLo || f > fHi || harmonic) continue;
        other += p[k];
    }
    return 10.0 * std::log10((other + 1e-30) / (fund + 1e-30));
}

// ------------------------------------------------------------------------------------------------
// Deterministic synthetic rap test mix (~140 BPM, trap-style): 808 with glides + saturation, punchy
// kick, half-time clap/snare, hats with rolls, a stereo pad, and a formant "rap vocal" with
// sibilants (6-9 kHz). Mix normalised to -14 LUFS-I.

struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    std::uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double uni() { return (next() >> 8) * (1.0 / 16777216.0); }
    double bip() { return uni() * 2.0 - 1.0; }
};

struct RapMix {
    Audio mix;   // stereo beat + vocal
    Audio beat;  // stereo beat only
    Audio vocal; // mono vocal stem (sidechain source)
    double gain = 1.0;
};

inline RapMix generateRapMix(double sr = 48000.0, double seconds = 12.0, std::uint32_t seed = 7,
                             double targetLufs = -14.0) {
    const int n = static_cast<int>(std::lround(sr * seconds));
    std::vector<double> bl(static_cast<std::size_t>(n)), br(static_cast<std::size_t>(n)), vo(static_cast<std::size_t>(n));
    Rng rng(seed);
    const double beat = 60.0 / 140.0, six = beat / 4.0, bar = beat * 4.0;
    const int bars = static_cast<int>(std::ceil(seconds / bar)) + 1;
    auto at = [&](double t) { return static_cast<int>(std::lround(t * sr)); };

    // --- 808 ---
    {
        const double roots[4] = {55.0, 55.0, 43.65, 49.0};
        struct Note { double t, f, len, glideFrom; };
        std::vector<Note> notes;
        for (int b = 0; b < bars; ++b) {
            const double f = roots[b % 4];
            const double t0 = b * bar;
            notes.push_back({t0, f, 6 * six, 0.0});
            notes.push_back({t0 + 6 * six, f, 4 * six, 0.0});
            notes.push_back({t0 + 10 * six, f * (b % 2 ? 1.5 : 1.335), 6 * six, f}); // glide up from root
        }
        double phase = 0.0;
        for (const auto& nt : notes) {
            const int s0 = at(nt.t), s1 = std::min(n, at(nt.t + nt.len));
            for (int i = s0; i < s1 && i < n; ++i) {
                if (i < 0) continue;
                const double t = (i - s0) / sr;
                double f = nt.f * (1.0 + 0.6 * std::exp(-t / 0.012)); // attack pitch drop
                if (nt.glideFrom > 0.0) f = nt.f + (nt.glideFrom - nt.f) * std::exp(-t / 0.05) + (f - nt.f);
                phase += 2.0 * kPi * f / sr;
                const double env = (1.0 - std::exp(-t / 0.002)) * std::exp(-t / 0.9)
                    * std::min(1.0, (s1 - i) / (0.006 * sr));
                const double v = std::tanh(2.2 * std::sin(phase)) / std::tanh(2.2) * env * 0.42;
                bl[static_cast<std::size_t>(i)] += v;
                br[static_cast<std::size_t>(i)] += v;
            }
        }
    }
    // --- kick ---
    {
        const int pattern[] = {0, 7, 10};
        for (int b = 0; b < bars; ++b) {
            for (int p : pattern) {
                if (p == 7 && b % 2 == 0) continue;
                const int s0 = at(b * bar + p * six);
                double ph = 0.0;
                for (int i = s0; i < std::min(n, s0 + at(0.35)); ++i) {
                    const double t = (i - s0) / sr;
                    const double f = 48.0 + 130.0 * std::exp(-t / 0.028);
                    ph += 2.0 * kPi * f / sr;
                    double v = std::sin(ph) * std::exp(-t / 0.11) * 0.65;
                    v += rng.bip() * std::exp(-t / 0.0015) * 0.25; // click
                    bl[static_cast<std::size_t>(i)] += v;
                    br[static_cast<std::size_t>(i)] += v;
                }
            }
        }
    }
    // --- clap / snare on beat 3 (half time) ---
    {
        dsp::Biquad bpL = dsp::Biquad::bandPass(sr, 1500.0, 0.9), bpR = bpL;
        dsp::Biquad hpL = dsp::Biquad::highPass(sr, 400.0), hpR = hpL;
        for (int b = 0; b < bars; ++b) {
            const int s0 = at(b * bar + 8 * six);
            double ph = 0.0;
            for (int i = s0; i < std::min(n, s0 + at(0.3)); ++i) {
                const double t = (i - s0) / sr;
                double e = std::exp(-t / 0.09);
                for (int k = 0; k < 3; ++k) { const double tk = t - k * 0.011; if (tk >= 0 && tk < 0.011) e += 0.8 * std::exp(-tk / 0.003); }
                const double nC = rng.bip(), nL = rng.bip(), nR = rng.bip();
                const double l = bpL.process(hpL.process(0.75 * nC + 0.25 * nL)) * e * 0.95;
                const double r = bpR.process(hpR.process(0.75 * nC + 0.25 * nR)) * e * 0.95;
                ph += 2.0 * kPi * 190.0 / sr;
                const double tone = std::sin(ph) * std::exp(-t / 0.04) * 0.22;
                bl[static_cast<std::size_t>(i)] += l + tone;
                br[static_cast<std::size_t>(i)] += r + tone;
            }
        }
    }
    // --- hats with rolls ---
    {
        dsp::Biquad h1 = dsp::Biquad::highPass(sr, 7000.0), h2 = h1;
        std::vector<std::pair<double, double>> hits; // time, velocity
        for (int b = 0; b < bars; ++b) {
            for (int e = 0; e < 8; ++e) {
                const double t = b * bar + e * 2 * six;
                if (b % 2 == 1 && e >= 6) continue; // roll replaces last beat
                hits.push_back({t, e % 2 ? 0.6 : 1.0});
            }
            if (b % 2 == 1) {
                for (int k = 0; k < 12; ++k) hits.push_back({b * bar + 3 * beat + k * beat / 12.0, 0.45 + 0.04 * k});
            }
        }
        std::vector<double> hat(static_cast<std::size_t>(n), 0.0);
        for (const auto& h : hits) {
            const int s0 = at(h.first);
            for (int i = s0; i < std::min(n, s0 + at(0.08)); ++i) {
                const double t = (i - s0) / sr;
                hat[static_cast<std::size_t>(i)] += rng.bip() * std::exp(-t / 0.018) * h.second;
            }
        }
        for (int i = 0; i < n; ++i) {
            const double v = h2.process(h1.process(hat[static_cast<std::size_t>(i)])) * 0.30;
            bl[static_cast<std::size_t>(i)] += v * 0.75;
            br[static_cast<std::size_t>(i)] += v * 1.0;
        }
    }
    // --- pad (A minor), detuned per side for width ---
    {
        const double chord[3] = {220.0, 261.63, 329.63};
        dsp::Biquad lpL = dsp::Biquad::lowPass(sr, 2200.0), lpR = lpL;
        std::array<double, 6> ph{};
        for (int i = 0; i < n; ++i) {
            double l = 0.0, r = 0.0;
            for (int k = 0; k < 3; ++k) {
                ph[static_cast<std::size_t>(k)] += 2.0 * kPi * chord[k] * 0.998 / sr;
                ph[static_cast<std::size_t>(k + 3)] += 2.0 * kPi * chord[k] * 1.002 / sr;
                for (int hrm = 1; hrm <= 6; ++hrm) {
                    l += std::sin(hrm * ph[static_cast<std::size_t>(k)]) / hrm;
                    r += std::sin(hrm * ph[static_cast<std::size_t>(k + 3)]) / hrm;
                }
            }
            const double sw = 0.6 + 0.4 * std::sin(2.0 * kPi * i / sr / (bar * 2.0));
            bl[static_cast<std::size_t>(i)] += lpL.process(l) * 0.016 * sw;
            br[static_cast<std::size_t>(i)] += lpR.process(r) * 0.016 * sw;
        }
    }
    // --- rap vocal: glottal pulse train -> formants, syllables on 16ths, sibilants 6-9 kHz ---
    {
        const double formants[5][3] = {{800, 1200, 2600}, {500, 1900, 2600}, {300, 2300, 3000}, {500, 900, 2500}, {350, 800, 2400}};
        std::array<dsp::Biquad, 3> fm{};
        dsp::Biquad sh1 = dsp::Biquad::highPass(sr, 6000.0), sh2 = sh1, sl1 = dsp::Biquad::lowPass(sr, 9000.0);
        dsp::Biquad presence = dsp::Biquad::peak(sr, 3000.0, 1.0, 4.0);
        double ph = 0.0;
        const double start = bar; // vocal enters after one bar
        int lastSyl = -1;
        bool sib = false;
        double sylF0 = 150.0;
        int vowel = 0;
        bool voiced = false;
        for (int i = 0; i < n; ++i) {
            const double t = i / sr;
            double v = 0.0;
            if (t >= start) {
                const int syl = static_cast<int>((t - start) / six);
                if (syl != lastSyl) {
                    lastSyl = syl;
                    voiced = (syl % 16 != 15) && (rng.uni() < 0.82);
                    sib = voiced && rng.uni() < 0.28;
                    vowel = static_cast<int>(rng.uni() * 5.0) % 5;
                    sylF0 = 135.0 + 40.0 * rng.uni();
                    for (int k = 0; k < 3; ++k)
                        fm[static_cast<std::size_t>(k)].setCoefficients(dsp::Biquad::bandPass(sr, formants[vowel][k], 6.0 + 2.0 * k));
                }
                const double ts = t - start - syl * six;
                if (voiced) {
                    const double f0 = sylF0 * (1.0 + 0.03 * std::sin(2.0 * kPi * 5.5 * t)) * (1.0 - 0.08 * ts / six);
                    ph += f0 / sr;
                    ph -= std::floor(ph);
                    const double pulse = std::exp(-ph * 12.0) - 1.0 / 12.0; // glottal-ish pulse
                    double formant = 0.0;
                    const double g[3] = {1.0, 0.6, 0.35};
                    for (int k = 0; k < 3; ++k) formant += g[k] * fm[static_cast<std::size_t>(k)].process(pulse);
                    const double sylEnv = std::min(1.0, ts / 0.008) * std::exp(-ts / 0.09) * (sib ? std::min(1.0, std::max(0.0, (ts - 0.035) / 0.01)) : 1.0);
                    v += presence.process(formant) * sylEnv * 1.0;
                    if (sib && ts < 0.05) {
                        const double sEnv = std::min(1.0, ts / 0.006) * std::exp(-ts / 0.03);
                        v += sl1.process(sh2.process(sh1.process(rng.bip()))) * sEnv * 0.55;
                    }
                }
            }
            vo[static_cast<std::size_t>(i)] = v * 1.1;
        }
    }

    RapMix out;
    out.mix.sampleRate = out.beat.sampleRate = out.vocal.sampleRate = sr;
    out.mix.resize(2, n);
    out.beat.resize(2, n);
    out.vocal.resize(1, n);
    for (int i = 0; i < n; ++i) {
        const auto k = static_cast<std::size_t>(i);
        out.beat.ch[0][k] = static_cast<float>(bl[k]);
        out.beat.ch[1][k] = static_cast<float>(br[k]);
        out.vocal.ch[0][k] = static_cast<float>(vo[k]);
        out.mix.ch[0][k] = static_cast<float>(bl[k] + vo[k]);
        out.mix.ch[1][k] = static_cast<float>(br[k] + vo[k]);
    }
    const double l = lufsIntegrated(out.mix);
    const double g = dsp::dbToGain(targetLufs - l);
    for (Audio* a : {&out.mix, &out.beat, &out.vocal})
        for (auto& c : a->ch) for (auto& v : c) v = static_cast<float>(v * g);
    out.gain = g;
    return out;
}

// ------------------------------------------------------------------------------------------------
// Engine render helper: processes `in` (and optional mono/stereo sidechain) in blocks of
// `blockSize`. If compensateLatency, the output is advanced by the engine latency (input is
// padded with zeros at the end) so it is time-aligned with the input.

template <typename T = float>
inline Audio renderEngine(dsp::MasterEngine& e, const Audio& in, const Audio* sidechain = nullptr,
                          int blockSize = 512, bool compensateLatency = true) {
    const int lat = compensateLatency ? e.latencySamples() : 0;
    const int frames = in.frames();
    const int total = frames + lat;
    const int chs = in.channels();
    std::vector<std::vector<T>> ib(static_cast<std::size_t>(chs), std::vector<T>(static_cast<std::size_t>(total), T(0)));
    for (int c = 0; c < chs; ++c)
        for (int i = 0; i < frames; ++i) ib[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<T>(in.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]);
    std::vector<std::vector<T>> ob(static_cast<std::size_t>(chs), std::vector<T>(static_cast<std::size_t>(total), T(0)));
    const int scChs = sidechain ? sidechain->channels() : 0;
    std::vector<std::vector<T>> sb(static_cast<std::size_t>(scChs), std::vector<T>(static_cast<std::size_t>(total), T(0)));
    for (int c = 0; c < scChs; ++c)
        for (int i = 0; i < std::min(frames, sidechain->frames()); ++i)
            sb[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<T>(sidechain->ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]);
    std::vector<const T*> ip(static_cast<std::size_t>(chs));
    std::vector<T*> op(static_cast<std::size_t>(chs));
    std::vector<const T*> sp(static_cast<std::size_t>(std::max(1, scChs)));
    for (int pos = 0; pos < total; pos += blockSize) {
        const int len = std::min(blockSize, total - pos);
        for (int c = 0; c < chs; ++c) {
            ip[static_cast<std::size_t>(c)] = ib[static_cast<std::size_t>(c)].data() + pos;
            op[static_cast<std::size_t>(c)] = ob[static_cast<std::size_t>(c)].data() + pos;
        }
        for (int c = 0; c < scChs; ++c) sp[static_cast<std::size_t>(c)] = sb[static_cast<std::size_t>(c)].data() + pos;
        e.process(ip.data(), op.data(), chs, len, scChs ? sp.data() : nullptr, scChs);
    }
    Audio out;
    out.sampleRate = in.sampleRate;
    out.resize(chs, frames);
    for (int c = 0; c < chs; ++c)
        for (int i = 0; i < frames; ++i)
            out.ch[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<float>(ob[static_cast<std::size_t>(c)][static_cast<std::size_t>(i + lat)]);
    return out;
}

inline int presetIndexByName(const std::string& name) {
    for (int i = 0; i < dsp::master::kNumPresets; ++i) {
        std::string a = dsp::master::kPresets[static_cast<std::size_t>(i)].name, b = name;
        auto low = [](std::string s) { for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
        if (low(a) == low(b)) return i;
    }
    return -1;
}

// Configure engine for a preset at a given strength (plain %), Master mode by default.
inline void applyPreset(dsp::MasterEngine& e, int preset, double strengthPercent, int mode = dsp::master::kModeMaster) {
    e.setParameterPlain(dsp::master::kMode, mode);
    e.setParameterPlain(dsp::master::kPreset, preset);
    e.setParameterPlain(dsp::master::kAmount, strengthPercent);
}

struct MixMetrics {
    double lufs, tp, samplePeak, crest, plr, sub, low, harm, mid, high;
};

inline MixMetrics measureMix(const Audio& a) {
    MixMetrics m{};
    m.lufs = lufsIntegrated(a);
    m.tp = truePeakDb(a);
    m.samplePeak = samplePeakDb(a);
    m.crest = crestDb(a);
    m.plr = m.tp - m.lufs;
    m.sub = bandRmsDb(a, 25.0, 60.0);
    m.low = bandRmsDb(a, 60.0, 100.0);
    m.harm = bandRmsDb(a, 100.0, 300.0);
    m.mid = bandRmsDb(a, 1000.0, 4000.0);
    m.high = bandRmsDb(a, 6000.0, 16000.0);
    return m;
}

} // namespace sonora::test
