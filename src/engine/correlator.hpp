// NovaInsert correlator and lock evaluator (F1 §7 lock conditions, §8 measurement; plan Task 13).
// Correlator: matched correlation of a captured probe return with the probe's inverse filter by FFT convolution;
// corr[D] = (x ⊛ inverse)[sweepStart + L − 1 + D] for D ∈ [0, lMax]. Main peak = argmax |corr|, main lobe ±kW samples;
// PBR = |main| / RMS(|corr|) outside the lobe over the full range; alt peak = largest local maximum outside the lobe.
// All buffers are allocated in the constructor; analyse() is allocation-free but uses mutable scratch, so one caller at a time
// (the measurement worker thread). evaluateLock() applies the conditions in the order ledgered for Task 13 (generation, then
// per-probe signal quality, then lag agreement), so a noise-only capture reports LowSnr/NoReturn rather than LagMismatch.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#include "engine/probe.hpp"
#include "engine/types.hpp"

namespace ni::engine
{

struct CorrResult
{
    int32_t lag;              // samples, integer main-peak position in [0, lMax]
    double mainAmp;           // signed correlation at lag (1.0 = unity loop gain)
    bool negative;            // polarity inverted (accepted, flagged)
    double fractional;        // lag + parabolic sub-sample offset (diagnostic only)
    double pbrDb;             // peak to background: |main| over RMS outside ±kW, whole range
    double altPeakRelDb;      // largest local maximum outside ±kW relative to |main|
    int32_t altLag;
    double returnPeakDbfs, returnRmsDbfs, preSilenceRmsDbfs, snrDb;
    bool clipped;             // return sample peak ≥ −0.1 dBFS
};

struct LockInputs { CorrResult probe[3]; uint32_t genStart, genNow; int32_t hwBlock; };
struct LockVerdict { bool locked; Reason reason; int32_t lag; };

inline constexpr double kLockMinReturnDbfs = -60.0, kLockMinSnrDb = 20.0, kLockMinPbrDb = 20.0, kLockAltRejectDb = -12.0;
inline constexpr double kClipDbfs = -0.1;

class Correlator
{
public:
    static constexpr int32_t kW = 16;

    explicit Correlator(const ProbeGenerator& probe, int32_t lMax = kLMax)
        : pre_(probe.sweepStart()), len_(probe.sweepLength()),
          lMax_(std::min(lMax, probe.length() - probe.sweepStart() - probe.sweepLength()))   // ≤ the probe's own wait (click: 0.45 s)
    {
        const auto& inv = probe.inverseFilter();
        size_t n = 1;
        while (n < static_cast<size_t>(probe.length()) + inv.size()) n <<= 1;
        n_ = n;
        constexpr double kPi = 3.14159265358979323846;
        tw_.resize(n_ / 2);
        for (size_t k = 0; k < tw_.size(); ++k)
            tw_[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n_));
        h_.assign(n_, {});
        for (size_t k = 0; k < inv.size(); ++k) h_[k] = inv[k];
        fft(h_.data(), false);
        buf_.resize(n_);
        c_.resize(static_cast<size_t>(lMax_) + 1);
    }

    CorrResult analyse(const float* x, int32_t n, int32_t /*hwBlock*/) const noexcept
    {
        CorrResult r {};
        const size_t m = std::min(static_cast<size_t>(std::max(n, 0)), n_);
        for (size_t i = 0; i < m; ++i) buf_[i] = { static_cast<double>(x[i]), 0.0 };
        std::fill(buf_.begin() + static_cast<std::ptrdiff_t>(m), buf_.end(), std::complex<double> {});
        fft(buf_.data(), false);
        for (size_t i = 0; i < n_; ++i) buf_[i] *= h_[i];
        fft(buf_.data(), true);
        const size_t base = static_cast<size_t>(pre_ + len_ - 1);
        const double scale = 1.0 / static_cast<double>(n_);
        // the lag range ends where the capture ends (a click window closes once the return is in): zero padding past m
        // is not background and must not dilute the PBR
        const size_t range = m > base ? std::min(c_.size(), m - base) : 1;
        for (size_t d = 0; d < range; ++d) c_[d] = buf_[base + d].real() * scale;

        size_t lag = 0;
        for (size_t d = 1; d < range; ++d) if (std::fabs(c_[d]) > std::fabs(c_[lag])) lag = d;
        const double main = c_[lag], amain = std::fabs(main);
        r.lag = static_cast<int32_t>(lag);
        r.mainAmp = main;
        r.negative = main < 0.0;

        const double sgn = r.negative ? -1.0 : 1.0;
        const double y0 = lag > 0 ? sgn * buf_[base + lag - 1].real() * scale : 0.0, y2 = sgn * buf_[base + lag + 1].real() * scale;
        const double den = y0 - 2.0 * amain + y2;
        r.fractional = static_cast<double>(lag) + (den < 0.0 ? 0.5 * (y0 - y2) / den : 0.0);

        double sum = 0.0, alt = 0.0;
        size_t cnt = 0, altLag = 0;
        for (size_t d = 0; d < range; ++d)
        {
            if (d + kW >= lag && d <= lag + kW) continue;
            const double a = std::fabs(c_[d]);
            sum += a * a; ++cnt;
            const bool localMax = (d == 0 || a >= std::fabs(c_[d - 1])) && (d + 1 == range || a >= std::fabs(c_[d + 1]));
            if (localMax && a > alt) { alt = a; altLag = d; }
        }
        const double bg = cnt > 0 ? std::sqrt(sum / static_cast<double>(cnt)) : 0.0;
        r.pbrDb = amain > 0.0 ? db(amain) - db(bg) : 0.0;
        r.altPeakRelDb = amain > 0.0 ? db(alt) - db(amain) : 0.0;
        r.altLag = static_cast<int32_t>(altLag);

        double pk = 0.0;
        for (size_t i = 0; i < m; ++i) pk = std::max(pk, static_cast<double>(std::fabs(x[i])));
        r.returnPeakDbfs = db(pk);
        r.clipped = r.returnPeakDbfs >= kClipDbfs;
        r.returnRmsDbfs = db(rms(x, m, lag + static_cast<size_t>(pre_), static_cast<size_t>(len_)));
        r.preSilenceRmsDbfs = db(rms(x, m, lag, static_cast<size_t>(pre_)));
        r.snrDb = r.returnRmsDbfs - r.preSilenceRmsDbfs;
        return r;
    }

private:
    static double db(double v) noexcept { return 20.0 * std::log10(std::max(v, 1e-15)); }

    static double rms(const float* x, size_t n, size_t from, size_t count) noexcept
    {
        const size_t end = std::min(n, from + count);
        if (from >= end) return 0.0;
        double s = 0.0;
        for (size_t i = from; i < end; ++i) s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / static_cast<double>(end - from));
    }

    // In-place iterative radix-2; inverse uses conjugate twiddles and is left unscaled.
    void fft(std::complex<double>* a, bool inverse) const noexcept
    {
        for (size_t i = 1, j = 0; i < n_; ++i)
        {
            size_t bit = n_ >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n_; len <<= 1)
        {
            const size_t half = len / 2, step = n_ / len;
            for (size_t i = 0; i < n_; i += len)
                for (size_t k = 0; k < half; ++k)
                {
                    const auto w = inverse ? std::conj(tw_[k * step]) : tw_[k * step];
                    const auto u = a[i + k], v = a[i + k + half] * w;
                    a[i + k] = u + v; a[i + k + half] = u - v;
                }
        }
    }

    int32_t pre_, len_, lMax_;
    size_t n_ = 0;
    std::vector<std::complex<double>> tw_, h_;
    mutable std::vector<std::complex<double>> buf_;
    mutable std::vector<double> c_;
};

// F1 §7. Order (Task 13 ruling): 7 generation → per probe 5 return/SNR, 4 clip, 3 PBR, 2/8 ambiguity, 6/9 range → 1 agreement.
inline LockVerdict evaluateLock(const LockInputs& in) noexcept
{
    const auto fail = [](Reason why) { return LockVerdict { false, why, -1 }; };
    if (in.genStart != in.genNow) return fail(Reason::KeyChange);
    for (const auto& p : in.probe)
    {
        if (p.returnPeakDbfs < kLockMinReturnDbfs) return fail(Reason::NoReturn);
        if (p.snrDb < kLockMinSnrDb) return fail(Reason::LowSnr);
        if (p.clipped) return fail(Reason::Clipped);
        if (p.pbrDb < kLockMinPbrDb) return fail(Reason::LowSnr);
        if (p.altPeakRelDb >= kLockAltRejectDb) return fail(Reason::AmbiguousPeak);
        if (p.lag < 0 || p.lag > kLMax || p.lag > kLMax - 2 * in.hwBlock) return fail(Reason::OutOfRange);
    }
    if (in.probe[1].lag != in.probe[0].lag || in.probe[2].lag != in.probe[0].lag) return fail(Reason::LagMismatch);
    return { true, Reason::None, in.probe[0].lag };
}

} // namespace ni::engine
