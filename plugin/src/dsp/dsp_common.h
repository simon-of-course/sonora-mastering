#pragma once

// Shared, SDK-independent DSP helpers for all SONORA plugins.
// Header-only so the engines can be unit-tested and rendered offline without the VST3 SDK.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

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

} // namespace sonora::dsp
