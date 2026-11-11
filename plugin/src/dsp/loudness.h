#pragma once

// ITU-R BS.1770-4 / EBU R128 loudness and true-peak metering (SDK-independent, header-only).
//
// - K-weighting (pre-filter high shelf + RLB high-pass) designed analytically for any sample rate
//   (matches the BS.1770 48 kHz reference coefficients).
// - Momentary (400 ms), short-term (3 s) and gated integrated loudness (absolute gate -70 LUFS,
//   relative gate -10 LU) using 100 ms sub-blocks (75 % overlapping 400 ms gating blocks).
//   The integrated measurement uses a fixed-size energy histogram (0.02 LU bins), so it runs
//   forever without allocation.
// - True peak via 4x polyphase FIR interpolation (Kaiser windowed sinc, 49 taps).
// Channel weights: channels 0/1 (L/R or mono) = 1.0. Only the first two channels are measured.

#include "dsp_common.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sonora::dsp {

constexpr double kLufsFloor = -120.0;

inline double powerToLufs(double meanSquare) {
    return meanSquare > 1e-15 ? -0.691 + 10.0 * std::log10(meanSquare) : kLufsFloor;
}

struct KWeighting {
    FastBiquad pre, rlb;

    static Biquad preFilter(double sr) {
        const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(kPi * f0 / sr);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        Biquad b;
        b.b0 = (vh + vb * k / q + k * k) / a0;
        b.b1 = 2.0 * (k * k - vh) / a0;
        b.b2 = (vh - vb * k / q + k * k) / a0;
        b.a1 = 2.0 * (k * k - 1.0) / a0;
        b.a2 = (1.0 - k / q + k * k) / a0;
        return b;
    }
    static Biquad rlbFilter(double sr) {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(kPi * f0 / sr);
        const double a0 = 1.0 + k / q + k * k;
        Biquad b;
        b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
        b.a1 = 2.0 * (k * k - 1.0) / a0;
        b.a2 = (1.0 - k / q + k * k) / a0;
        return b;
    }
    void prepare(double sr) {
        pre.setCoefficients(preFilter(sr));
        rlb.setCoefficients(rlbFilter(sr));
        reset();
    }
    void reset() { pre.reset(); rlb.reset(); }
    double process(double x) { return rlb.process(pre.process(x)); }
    void flush() { pre.flush(); rlb.flush(); }
};

// 4x polyphase interpolator used for true-peak detection (meter and limiter sidechain).
// pushAndPeak(x) returns max |.| of the four interpolated sub-samples that lie in
// [n - kLatency, n - kLatency + 1) (phase 0 is the exact delayed input sample).
// BS.1770-4 style 4x true-peak interpolator (48-tap Kaiser-windowed sinc, 12 taps per phase)
// for a stereo pair. pushAndPeak() returns the largest |sample| among the original sample and
// the three interpolated points of both channels, delayed by kLatency base-rate samples.
class TruePeakInterpolator4x {
public:
    static constexpr int kFactor = 4;
    static constexpr int kHalfTaps = 24;               // filter centre at 4x rate
    static constexpr int kLength = 2 * kHalfTaps + 1;  // 49 taps (the last is zero)
    static constexpr int kTaps = 12;                   // taps per interpolated phase
    static constexpr int kLatency = kHalfTaps / kFactor; // 6 base-rate samples

    TruePeakInterpolator4x() {
        std::array<double, kLength> h{};
        for (int j = 0; j < kLength; ++j) {
            h[static_cast<std::size_t>(j)] = sincPi((j - kHalfTaps) / static_cast<double>(kFactor))
                * kaiserWindow(j, kLength, 7.5);
        }
        std::array<std::array<double, kTaps>, 3> c{};
        for (int p = 1; p < kFactor; ++p) {
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k) sum += h[static_cast<std::size_t>(kFactor * k + p)];
            // Normalise each phase to unity DC gain.
            for (int k = 0; k < kTaps; ++k) {
                c[static_cast<std::size_t>(p - 1)][static_cast<std::size_t>(k)] = h[static_cast<std::size_t>(kFactor * k + p)] / sum;
            }
        }
        // Phase 2 is symmetric and phases 1/3 are mirror images, so the filter folds onto
        // s_k = x_k + x_{11-k} and d_k = x_k - x_{11-k}:
        //   y1 + y3 = 2 sum13_k s_k,  y1 - y3 = 2 diff13_k d_k,  y2 = mid2_k s_k.
        for (std::size_t k = 0; k < 6; ++k) {
            sum13_[k] = 0.5 * (c[0][k] + c[0][11 - k]);
            diff13_[k] = 0.5 * (c[0][k] - c[0][11 - k]);
            mid2_[k] = c[1][k];
        }
        reset();
    }
    void reset() { history_.fill(0.0); pos_ = 0; }

    double pushAndPeak(double x) { return pushAndPeak(x, 0.0); }

    double pushAndPeak(double l, double r) {
        pos_ = pos_ == 0 ? kTaps - 1 : pos_ - 1;
        const F2 in = F2::make(l, r);
        in.store(&history_[static_cast<std::size_t>(2 * pos_)]);
        in.store(&history_[static_cast<std::size_t>(2 * (pos_ + kTaps))]);
        const double* hist = &history_[static_cast<std::size_t>(2 * pos_)]; // pair k = x[n-k]
        F2 sum13 = F2::splat(0.0), diff13 = F2::splat(0.0), y2 = F2::splat(0.0);
        for (int k = 0; k < 6; ++k) {
            const F2 a = F2::load(hist + 2 * k), b = F2::load(hist + 2 * (11 - k));
            const F2 s = a + b;
            sum13 = sum13 + F2::splat(sum13_[static_cast<std::size_t>(k)]) * s;
            diff13 = diff13 + F2::splat(diff13_[static_cast<std::size_t>(k)]) * (a - b);
            y2 = y2 + F2::splat(mid2_[static_cast<std::size_t>(k)]) * s;
        }
        F2 peak = F2::absv(F2::load(hist + 2 * kLatency));
        peak = F2::maxv(peak, F2::absv(sum13 + diff13));
        peak = F2::maxv(peak, F2::absv(sum13 - diff13));
        peak = F2::maxv(peak, F2::absv(y2));
        return peak.hmax();
    }

private:
    std::array<double, 6> sum13_{}, diff13_{}, mid2_{};
    std::array<double, 4 * kTaps> history_{};
    int pos_ = 0;
};

class TruePeakMeter {
public:
    void reset() {
        interp_.reset();
        maxPeak_ = 0.0;
        blockPeak_ = 0.0;
    }
    // Returns the stereo-max true peak (linear) of this sample.
    double processFrame(const double* x, int channels) {
        const double p = channels >= 2 ? interp_.pushAndPeak(x[0], x[1]) : interp_.pushAndPeak(x[0]);
        maxPeak_ = std::max(maxPeak_, p);
        blockPeak_ = std::max(blockPeak_, p);
        return p;
    }
    double maxTruePeak() const { return maxPeak_; }
    double maxTruePeakDb() const { return gainToDb(maxPeak_); }
    double takeBlockPeak() { const double p = blockPeak_; blockPeak_ = 0.0; return p; }
    void resetHold() { maxPeak_ = 0.0; }

private:
    TruePeakInterpolator4x interp_{};
    double maxPeak_ = 0.0, blockPeak_ = 0.0;
};

class LoudnessMeter {
public:
    static constexpr int kShortTermBlocks = 30; // 3 s of 100 ms sub-blocks
    static constexpr int kMomentaryBlocks = 4;  // 400 ms
    static constexpr double kHistMinLufs = -70.0;
    static constexpr double kHistMaxLufs = 10.0;
    static constexpr double kBinWidth = 0.02;
    static constexpr int kBins = static_cast<int>((kHistMaxLufs - kHistMinLufs) / kBinWidth);

    void prepare(double sampleRate) {
        sampleRate_ = sampleRate;
        blockLength_ = std::max(1, static_cast<int>(std::lround(sampleRate * 0.1)));
        for (auto& k : kw_) k.prepare(sampleRate);
        histEnergy_.assign(static_cast<std::size_t>(kBins), 0.0);
        histCount_.assign(static_cast<std::size_t>(kBins), 0);
        reset();
    }
    void reset() {
        for (auto& k : kw_) k.reset();
        ring_.fill(0.0);
        ringPos_ = 0;
        acc_ = 0.0;
        counter_ = 0;
        momentary_ = shortTerm_ = 0.0;
        resetIntegrated();
    }
    void resetIntegrated() {
        std::fill(histEnergy_.begin(), histEnergy_.end(), 0.0);
        std::fill(histCount_.begin(), histCount_.end(), 0);
        blocksSeen_ = 0;
        integratedLufs_ = kLufsFloor;
        gatedBlocks_ = 0;
    }

    void processFrame(const double* x, int channels) {
        double e = 0.0;
        for (int c = 0; c < std::min(channels, 2); ++c) {
            const double y = kw_[static_cast<std::size_t>(c)].process(x[c]);
            e += y * y;
        }
        acc_ += e;
        if (++counter_ >= blockLength_) {
            for (auto& k : kw_) k.flush();
            finishSubBlock();
        }
    }

    template <typename T>
    void process(const T* const* channels, int numChannels, int numSamples) {
        double frame[2] = {0.0, 0.0};
        const int ch = std::min(numChannels, 2);
        for (int i = 0; i < numSamples; ++i) {
            for (int c = 0; c < ch; ++c) frame[c] = static_cast<double>(channels[c][i]);
            processFrame(frame, ch);
        }
    }

    double momentaryLufs() const { return powerToLufs(momentary_); }
    double shortTermLufs() const { return powerToLufs(shortTerm_); }
    double integratedLufs() const { return integratedLufs_; }
    std::uint64_t gatedBlockCount() const { return gatedBlocks_; }

private:
    void finishSubBlock() {
        ring_[static_cast<std::size_t>(ringPos_)] = acc_ / static_cast<double>(blockLength_);
        ringPos_ = (ringPos_ + 1) % kShortTermBlocks;
        acc_ = 0.0;
        counter_ = 0;
        ++blocksSeen_;
        double m = 0.0, s = 0.0;
        for (int k = 0; k < kShortTermBlocks; ++k) {
            const double v = ring_[static_cast<std::size_t>((ringPos_ - 1 - k + 2 * kShortTermBlocks) % kShortTermBlocks)];
            if (k < kMomentaryBlocks) m += v;
            s += v;
        }
        momentary_ = m / kMomentaryBlocks;
        shortTerm_ = s / kShortTermBlocks;
        if (blocksSeen_ >= static_cast<std::uint64_t>(kMomentaryBlocks)) {
            const double l = powerToLufs(momentary_);
            if (l > kHistMinLufs) {
                const int bin = std::clamp(static_cast<int>((l - kHistMinLufs) / kBinWidth), 0, kBins - 1);
                histEnergy_[static_cast<std::size_t>(bin)] += momentary_;
                histCount_[static_cast<std::size_t>(bin)] += 1;
                ++gatedBlocks_;
                updateIntegrated();
            }
        }
    }

    void updateIntegrated() {
        double energy = 0.0;
        std::uint64_t count = 0;
        for (int b = 0; b < kBins; ++b) {
            energy += histEnergy_[static_cast<std::size_t>(b)];
            count += histCount_[static_cast<std::size_t>(b)];
        }
        if (count == 0) { integratedLufs_ = kLufsFloor; return; }
        const double relGate = powerToLufs(energy / static_cast<double>(count)) - 10.0;
        energy = 0.0;
        count = 0;
        for (int b = 0; b < kBins; ++b) {
            const double centre = kHistMinLufs + (b + 0.5) * kBinWidth;
            if (centre > relGate) {
                energy += histEnergy_[static_cast<std::size_t>(b)];
                count += histCount_[static_cast<std::size_t>(b)];
            }
        }
        integratedLufs_ = count ? powerToLufs(energy / static_cast<double>(count)) : kLufsFloor;
    }

    double sampleRate_ = 48000.0;
    int blockLength_ = 4800;
    std::array<KWeighting, 2> kw_{};
    std::array<double, kShortTermBlocks> ring_{};
    int ringPos_ = 0;
    double acc_ = 0.0;
    int counter_ = 0;
    std::uint64_t blocksSeen_ = 0, gatedBlocks_ = 0;
    double momentary_ = 0.0, shortTerm_ = 0.0, integratedLufs_ = kLufsFloor;
    std::vector<double> histEnergy_;
    std::vector<std::uint64_t> histCount_;
};

// Offline helpers (allocate; not for the audio thread).
template <typename T>
inline double measureIntegratedLufs(const T* const* channels, int numChannels, int numSamples, double sampleRate) {
    LoudnessMeter m;
    m.prepare(sampleRate);
    m.process(channels, numChannels, numSamples);
    return m.integratedLufs();
}

template <typename T>
inline double measureTruePeakDb(const T* const* channels, int numChannels, int numSamples) {
    TruePeakMeter m;
    m.reset();
    double frame[2] = {0.0, 0.0};
    const int ch = std::min(numChannels, 2);
    for (int i = 0; i < numSamples + TruePeakInterpolator4x::kLatency + 1; ++i) {
        for (int c = 0; c < ch; ++c) frame[c] = i < numSamples ? static_cast<double>(channels[c][i]) : 0.0;
        m.processFrame(frame, ch);
    }
    return m.maxTruePeakDb();
}

} // namespace sonora::dsp
