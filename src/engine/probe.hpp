// NovaInsert measurement probe (F1 §8 "Probe", plan Task 12). Exponential sine sweep (Farina) f0 → f1 over sweepSec with
// raised-cosine edges of edgeSec, preSec of exact silence before and postSec (= L_max/fs + 0.5 s decay guard) after.
// Deterministic: the template is computed once in double at construction (message/worker thread); render() only scales it.
// inverseFilter() is the time-reversed sweep weighted ∝ f(τ) (the reversed signal decays 6 dB/octave over its own time:
// the sweep's spectrum falls as 1/√f, so |H| ∝ √f flattens the product), normalised so that sweep ⊛ inverse peaks at 1.0 at
// lag sweepLength − 1: the matched correlator's single sharp main lobe (F1 §8 lock condition 3; ±3 samples, 70 dB to background).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "engine/types.hpp"

namespace ni::engine
{

struct ProbeSpec
{
    int fs = 48000;
    double f0 = 40.0, f1 = 18000.0;
    double sweepSec = 1.0, edgeSec = 0.010, preSec = 0.100;
    double postSec = static_cast<double>(kLMax) / 48000.0 + 0.5;   // L_max/fs + 0.5 s at the default fs
    bool isClick = false;

    // The latency ping (user 2026-11-05, as ReaInsert's ping): one unit sample after 20 ms of exact silence; the longest
    // wait for its return is 0.45 s (an AD/DA round trip plus outboard is a few ms; the Measurer closes the window as soon as
    // the click is back). Matched filter of a click = the click: the correlation is the captured return itself.
    static ProbeSpec click() noexcept { ProbeSpec s; s.preSec = 0.020; s.postSec = 0.450; s.isClick = true; return s; }
};

class ProbeGenerator
{
public:
    explicit ProbeGenerator(ProbeSpec spec)
        : pre_(static_cast<int32_t>(std::lround(spec.preSec * spec.fs))),
          sweep_(spec.isClick ? 1 : static_cast<int32_t>(std::lround(spec.sweepSec * spec.fs))),
          post_(static_cast<int32_t>(std::lround(spec.postSec * spec.fs)))
    {
        if (spec.isClick)
        {
            sweep_ = 1;
            shape_.assign(1, 1.0);
            inverse_.assign(1, 1.0f);
            return;
        }
        constexpr double kPi = 3.14159265358979323846;
        const double T = static_cast<double>(sweep_) / spec.fs, Lr = std::log(spec.f1 / spec.f0);
        const auto edge = static_cast<int32_t>(std::lround(spec.edgeSec * spec.fs));
        shape_.resize(static_cast<size_t>(sweep_));
        double pk = 0.0;
        for (int32_t n = 0; n < sweep_; ++n)
        {
            const double t = static_cast<double>(n) / spec.fs;
            double v = std::sin(2.0 * kPi * spec.f0 * T / Lr * (std::exp(t / T * Lr) - 1.0));
            const int32_t fromEnd = sweep_ - 1 - n, e = std::min(n, fromEnd);
            if (e < edge) v *= 0.5 * (1.0 - std::cos(kPi * static_cast<double>(e) / static_cast<double>(edge)));
            shape_[static_cast<size_t>(n)] = v;
            pk = std::max(pk, std::fabs(v));
        }
        for (auto& v : shape_) v /= pk;   // unit sample peak
        // inverse: reversed sweep × f(τ)/f0 (τ = original time of the reversed sample), scaled for a unit peak
        std::vector<double> inv(static_cast<size_t>(sweep_));
        double c0 = 0.0;
        for (int32_t k = 0; k < sweep_; ++k)
        {
            const int32_t src = sweep_ - 1 - k;
            const double w = std::exp(static_cast<double>(src) / spec.fs / T * Lr);
            inv[static_cast<size_t>(k)] = shape_[static_cast<size_t>(src)] * w;
            c0 += shape_[static_cast<size_t>(src)] * shape_[static_cast<size_t>(src)] * w;
        }
        inverse_.resize(inv.size());
        for (size_t k = 0; k < inv.size(); ++k) inverse_[k] = static_cast<float>(inv[k] / c0);
    }

    int32_t length() const noexcept { return pre_ + sweep_ + post_; }
    int32_t sweepStart() const noexcept { return pre_; }
    int32_t sweepLength() const noexcept { return sweep_; }

    // dst holds length() samples; sample peak == peakDbfs (the template peak is exactly 1.0).
    void render(float* dst, float peakDbfs) const noexcept
    {
        const double g = std::pow(10.0, static_cast<double>(peakDbfs) / 20.0);
        std::fill(dst, dst + pre_, 0.0f);
        for (int32_t n = 0; n < sweep_; ++n) dst[pre_ + n] = static_cast<float>(shape_[static_cast<size_t>(n)] * g);
        std::fill(dst + pre_ + sweep_, dst + length(), 0.0f);
    }

    const std::vector<float>& inverseFilter() const noexcept { return inverse_; }

private:
    int32_t pre_, sweep_, post_;
    std::vector<double> shape_;
    std::vector<float> inverse_;
};

} // namespace ni::engine
