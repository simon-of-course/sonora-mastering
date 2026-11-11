#pragma once

// Shared, SDK-independent DSP helpers for all SONORA plugins.
// Header-only so the engines can be unit-tested and rendered offline without the VST3 SDK.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define SONORA_DSP_HAS_MXCSR 1
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define SONORA_DSP_HAS_SSE2 1
#endif

namespace sonora::dsp {

constexpr double kPi = 3.14159265358979323846;

inline double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain, double floorDb = -120.0) {
    return gain > 1e-12 ? std::max(floorDb, 20.0 * std::log10(gain)) : floorDb;
}
inline double clamp01(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double sanitize(double value) {
    return std::isfinite(value) && std::abs(value) > 1e-30 ? value : 0.0;
}

// One-pole coefficient for a time constant in seconds (0 -> immediate).
inline double timeCoefficient(double seconds, double sampleRate) {
    return seconds > 0.0 && sampleRate > 0.0 ? std::exp(-1.0 / (seconds * sampleRate)) : 0.0;
}

struct Smoother {
    double value = 0.0;
    double coefficient = 0.0;
    void setTime(double seconds, double sampleRate) { coefficient = timeCoefficient(seconds, sampleRate); }
    void reset(double v) { value = v; }
    double next(double target) {
        value = target + (value - target) * coefficient;
        return value;
    }
};

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }
    void setCoefficients(const Biquad& other) {
        b0 = other.b0; b1 = other.b1; b2 = other.b2; a1 = other.a1; a2 = other.a2;
    }
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = sanitize(b1 * x - a1 * y + z2);
        z2 = sanitize(b2 * x - a2 * y);
        return y;
    }

    // RBJ Audio EQ Cookbook designs. Frequencies are clamped below Nyquist.
    static Biquad identity() { return {}; }

    static Biquad make(double b0, double b1, double b2, double a0, double a1, double a2) {
        Biquad q;
        q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
        return q;
    }
    static double safeFrequency(double f, double sr) { return std::clamp(f, 1.0, sr * 0.49); }

    static Biquad lowPass(double sr, double f, double q = 0.7071) {
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
        return make((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a);
    }
    static Biquad highPass(double sr, double f, double q = 0.7071) {
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
        return make((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a);
    }
    static Biquad bandPass(double sr, double f, double q) {
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
        return make(a, 0.0, -a, 1 + a, -2 * c, 1 - a);
    }
    static Biquad peak(double sr, double f, double q, double gainDb) {
        if (std::abs(gainDb) < 1e-4) return identity();
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), a = std::sin(w) / (2.0 * q);
        return make(1 + a * A, -2 * c, 1 - a * A, 1 + a / A, -2 * c, 1 - a / A);
    }
    static Biquad lowShelf(double sr, double f, double gainDb, double slope = 1.0) {
        if (std::abs(gainDb) < 1e-4) return identity();
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), s = std::sin(w);
        const double alpha = s / 2.0 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2);
        const double k = 2 * std::sqrt(A) * alpha;
        return make(A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c),
                    A * ((A + 1) - (A - 1) * c - k), (A + 1) + (A - 1) * c + k,
                    -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k);
    }
    static Biquad highShelf(double sr, double f, double gainDb, double slope = 1.0) {
        if (std::abs(gainDb) < 1e-4) return identity();
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w = 2.0 * kPi * safeFrequency(f, sr) / sr, c = std::cos(w), s = std::sin(w);
        const double alpha = s / 2.0 * std::sqrt((A + 1 / A) * (1 / slope - 1) + 2);
        const double k = 2 * std::sqrt(A) * alpha;
        return make(A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c),
                    A * ((A + 1) + (A - 1) * c - k), (A + 1) - (A - 1) * c + k,
                    2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k);
    }
};

// Topology-preserving-transform state-variable filter (Zavalishin). Stable under fast
// cutoff modulation, so it is preferred for automated/modulated filters.
struct SvfTpt {
    double g = 0.0, k = 1.4142, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    double ic1 = 0.0, ic2 = 0.0;

    void reset() { ic1 = ic2 = 0.0; }
    void set(double sr, double cutoff, double q) {
        const double f = std::clamp(cutoff, 5.0, sr * 0.49);
        g = std::tan(kPi * f / sr);
        k = 1.0 / std::max(q, 0.05);
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    struct Out { double low, band, high; };
    Out process(double x) {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = sanitize(2.0 * v1 - ic1);
        ic2 = sanitize(2.0 * v2 - ic2);
        return {v2, v1, x - k * v1 - v2};
    }
};

// Linkwitz-Riley 4th-order crossover (two cascaded Butterworth sections per path).
// low + high sums to an allpass response (flat magnitude).
struct LinkwitzRiley {
    std::array<Biquad, 2> lp{}, hp{};
    void set(double sr, double f) {
        const auto low = Biquad::lowPass(sr, f, 0.7071);
        const auto high = Biquad::highPass(sr, f, 0.7071);
        for (auto& s : lp) s.setCoefficients(low);
        for (auto& s : hp) s.setCoefficients(high);
    }
    void reset() { for (auto& s : lp) s.reset(); for (auto& s : hp) s.reset(); }
    void process(double x, double& low, double& high) {
        low = lp[1].process(lp[0].process(x));
        high = hp[1].process(hp[0].process(x));
    }
};

// ---------------------------------------------------------------------------------------------
// Additions for v0.2 (Master-DSP): window design helpers and a real-time safe delay line.

inline double sincPi(double x) {
    if (std::abs(x) < 1e-12) return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

// Zeroth-order modified Bessel function of the first kind (power series).
inline double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

// Kaiser window value for tap n of a length-N window (n in [0, N-1]).
inline double kaiserWindow(int n, int length, double beta) {
    if (length <= 1) return 1.0;
    const double r = 2.0 * n / static_cast<double>(length - 1) - 1.0;
    return besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
}

// Fixed-capacity delay line. Allocate with prepare() (not real-time), then push/read per sample.
struct DelayLine {
    std::vector<double> buffer;
    int mask = 0, writePos = 0;
    void prepare(int maxDelay) {
        int size = 1;
        while (size < maxDelay + 2) size <<= 1;
        buffer.assign(static_cast<std::size_t>(size), 0.0);
        mask = size - 1;
        writePos = 0;
    }
    void reset() { std::fill(buffer.begin(), buffer.end(), 0.0); writePos = 0; }
    void push(double x) {
        writePos = (writePos + 1) & mask;
        buffer[static_cast<std::size_t>(writePos)] = x;
    }
    // delay 0 = most recently pushed sample.
    double read(int delay) const {
        return buffer[static_cast<std::size_t>((writePos - delay) & mask)];
    }
};

// Biquad variant for hot per-sample paths: same coefficients/designs as Biquad, but without the
// per-sample sanitize(). Call flush() periodically (e.g. at control rate) to clear denormal or
// non-finite state.
struct FastBiquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    FastBiquad() = default;
    FastBiquad(const Biquad& q) : b0(q.b0), b1(q.b1), b2(q.b2), a1(q.a1), a2(q.a2), z1(q.z1), z2(q.z2) {}
    void reset() { z1 = z2 = 0.0; }
    void setCoefficients(const Biquad& q) { b0 = q.b0; b1 = q.b1; b2 = q.b2; a1 = q.a1; a2 = q.a2; }
    double process(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void flush() { z1 = sanitize(z1); z2 = sanitize(z2); }
};

// Linkwitz-Riley 4th-order crossover built from FastBiquads (see LinkwitzRiley).
struct FastLinkwitzRiley {
    std::array<FastBiquad, 2> lp{}, hp{};
    void set(double sr, double f) {
        const auto low = Biquad::lowPass(sr, f, 0.7071);
        const auto high = Biquad::highPass(sr, f, 0.7071);
        for (auto& s : lp) s.setCoefficients(low);
        for (auto& s : hp) s.setCoefficients(high);
    }
    void reset() { for (auto& s : lp) s.reset(); for (auto& s : hp) s.reset(); }
    void flush() { for (auto& s : lp) s.flush(); for (auto& s : hp) s.flush(); }
    void process(double x, double& low, double& high) {
        low = lp[1].process(lp[0].process(x));
        high = hp[1].process(hp[0].process(x));
    }
};

// Second-order allpass with the same poles as a Butterworth (Q 0.7071) section at f: equals the
// summed response (low + high) of a LinkwitzRiley crossover at f, for phase-matching parallel paths.
inline Biquad allPassForCrossover(double sr, double f) {
    const double w = 2.0 * kPi * Biquad::safeFrequency(f, sr) / sr, c = std::cos(w), a = std::sin(w) / (2.0 * 0.7071);
    return Biquad::make(1.0 - a, -2.0 * c, 1.0 + a, 1.0 + a, -2.0 * c, 1.0 - a);
}

// Sets FTZ|DAZ for the current scope (x86 only; no-op elsewhere) and restores the previous mode.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() {
#ifdef SONORA_DSP_HAS_MXCSR
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040u);
#endif
    }
    ~ScopedFlushDenormals() {
#ifdef SONORA_DSP_HAS_MXCSR
        _mm_setcsr(saved_);
#endif
    }
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

private:
#ifdef SONORA_DSP_HAS_MXCSR
    unsigned int saved_ = 0;
#endif
};

// Two doubles processed together (stereo pair). SSE2 when available, scalar otherwise.
struct F2 {
#ifdef SONORA_DSP_HAS_SSE2
    __m128d v;
    static F2 load(const double* p) { return {_mm_loadu_pd(p)}; }
    static F2 make(double l, double r) { return {_mm_set_pd(r, l)}; }
    static F2 splat(double a) { return {_mm_set1_pd(a)}; }
    void store(double* p) const { _mm_storeu_pd(p, v); }
    double l() const { return _mm_cvtsd_f64(v); }
    double r() const { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }
    friend F2 operator+(F2 a, F2 b) { return {_mm_add_pd(a.v, b.v)}; }
    friend F2 operator-(F2 a, F2 b) { return {_mm_sub_pd(a.v, b.v)}; }
    friend F2 operator*(F2 a, F2 b) { return {_mm_mul_pd(a.v, b.v)}; }
    static F2 absv(F2 a) { return {_mm_andnot_pd(_mm_set1_pd(-0.0), a.v)}; }
    static F2 maxv(F2 a, F2 b) { return {_mm_max_pd(a.v, b.v)}; }
#else
    double a0, a1;
    static F2 load(const double* p) { return {p[0], p[1]}; }
    static F2 make(double l, double r) { return {l, r}; }
    static F2 splat(double a) { return {a, a}; }
    void store(double* p) const { p[0] = a0; p[1] = a1; }
    double l() const { return a0; }
    double r() const { return a1; }
    friend F2 operator+(F2 a, F2 b) { return {a.a0 + b.a0, a.a1 + b.a1}; }
    friend F2 operator-(F2 a, F2 b) { return {a.a0 - b.a0, a.a1 - b.a1}; }
    friend F2 operator*(F2 a, F2 b) { return {a.a0 * b.a0, a.a1 * b.a1}; }
    static F2 absv(F2 a) { return {std::abs(a.a0), std::abs(a.a1)}; }
    static F2 maxv(F2 a, F2 b) { return {std::max(a.a0, b.a0), std::max(a.a1, b.a1)}; }
#endif
    double hmax() const { return std::max(l(), r()); }
};

// FastBiquad for a stereo pair (same coefficients on both lanes). Call flush() periodically.
struct FastBiquad2 {
    F2 b0 = F2::splat(1.0), b1 = F2::splat(0.0), b2 = F2::splat(0.0), a1 = F2::splat(0.0), a2 = F2::splat(0.0);
    F2 z1 = F2::splat(0.0), z2 = F2::splat(0.0);

    void reset() { z1 = z2 = F2::splat(0.0); }
    void setCoefficients(const Biquad& q) {
        b0 = F2::splat(q.b0); b1 = F2::splat(q.b1); b2 = F2::splat(q.b2);
        a1 = F2::splat(q.a1); a2 = F2::splat(q.a2);
    }
    F2 process(F2 x) {
        const F2 y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void flush() {
        z1 = F2::make(sanitize(z1.l()), sanitize(z1.r()));
        z2 = F2::make(sanitize(z2.l()), sanitize(z2.r()));
    }
};

} // namespace sonora::dsp
