// Correlator + evaluateLock (plan Task 13, F1 §7 lock conditions, §8 measurement): matched correlation of a captured probe
// return against the probe's inverse filter; lag search [0, L_max], main lobe ±16, alt-peak scan outside it; negative controls.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <random>
#include <vector>
#include "engine/correlator.hpp"
#include "../rt/alloc_guard.hpp"

using namespace ni::engine;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kProbeDbfs = -12.0f;

const ProbeGenerator& probe()
{
    static const ProbeGenerator g { ProbeSpec {} };
    return g;
}

const Correlator& corr()
{
    static const Correlator c { probe() };
    return c;
}

// The probe as it would come back through a loop of D samples, gain gDb, optional polarity flip.
std::vector<float> delayed(int32_t D, double gDb = 0.0, bool invert = false)
{
    const auto& g = probe();
    std::vector<float> clean(static_cast<size_t>(g.length())), x(clean.size(), 0.0f);
    g.render(clean.data(), kProbeDbfs);
    const double k = std::pow(10.0, gDb / 20.0) * (invert ? -1.0 : 1.0);
    for (size_t n = 0; n + static_cast<size_t>(D) < x.size(); ++n) x[n + static_cast<size_t>(D)] = static_cast<float>(clean[n] * k);
    return x;
}

void addNoise(std::vector<float>& x, double rmsDbfs, unsigned seed = 7)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, std::pow(10.0, rmsDbfs / 20.0));
    for (auto& v : x) v = static_cast<float>(v + nd(rng));
}

void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = (inverse ? 2 : -2) * kPi / static_cast<double>(len);
        const std::complex<double> w(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> wk(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k, wk *= w)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= static_cast<double>(n);
}

// Independent reference: corr[D] = (x ⊛ inverse)[sweepStart + L − 1 + D], D ∈ [0, kLMax].
std::vector<double> referenceCorr(const std::vector<float>& x)
{
    const auto& g = probe();
    const auto& inv = g.inverseFilter();
    size_t N = 1;
    while (N < x.size() + inv.size()) N <<= 1;
    std::vector<std::complex<double>> a(N), b(N);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    for (size_t i = 0; i < inv.size(); ++i) b[i] = inv[i];
    fft(a, false); fft(b, false);
    for (size_t i = 0; i < N; ++i) a[i] *= b[i];
    fft(a, true);
    std::vector<double> c(static_cast<size_t>(kLMax) + 1);
    const size_t base = static_cast<size_t>(g.sweepStart() + g.sweepLength() - 1);
    for (size_t d = 0; d < c.size(); ++d) c[d] = a[base + d].real();
    return c;
}

CorrResult goodProbe(int32_t lag)
{
    CorrResult r {};
    r.lag = lag; r.mainAmp = 0.25; r.fractional = lag; r.pbrDb = 60.0; r.altPeakRelDb = -40.0; r.altLag = lag + 500;
    r.returnPeakDbfs = -12.0; r.returnRmsDbfs = -15.0; r.preSilenceRmsDbfs = -100.0; r.snrDb = 85.0;
    return r;
}

LockInputs three(const CorrResult& a, const CorrResult& b, const CorrResult& c, int32_t B = 64)
{
    LockInputs in {};
    in.probe[0] = a; in.probe[1] = b; in.probe[2] = c;
    in.genStart = in.genNow = 5; in.hwBlock = B;
    return in;
}

} // namespace

TEST_CASE("correlator_lag_exact")
{
    for (int32_t D : { 0, 1, 17, 64, 1000, 4097, kLMax - 4 * 64 })
    {
        CAPTURE(D);
        const auto x = delayed(D, -3.0);
        const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
        REQUIRE(r.lag == D);
        REQUIRE_FALSE(r.negative);
        REQUIRE(r.pbrDb >= 20.0);
        REQUIRE_FALSE(r.clipped);
        REQUIRE(evaluateLock(three(r, r, r)).locked);
    }
}

TEST_CASE("correlator_negative_polarity_flagged_lag_unchanged")
{
    const auto x = delayed(777, 0.0, true);
    const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
    REQUIRE(r.lag == 777);
    REQUIRE(r.negative);
    REQUIRE(r.mainAmp < 0.0);
    const auto v = evaluateLock(three(r, r, r));
    REQUIRE(v.locked);
    REQUIRE(v.lag == 777);
}

TEST_CASE("correlator_fractional_estimate_for_0_3_sample_delay")
{
    // Band-limited fractional delay of 1000.3 samples: Hann-windowed sinc interpolation of the clean probe, in double.
    const auto& g = probe();
    std::vector<float> clean(static_cast<size_t>(g.length())), x(clean.size(), 0.0f);
    g.render(clean.data(), kProbeDbfs);
    const double delay = 1000.3;
    constexpr int H = 64;
    const int32_t lo = g.sweepStart() - H, hi = g.sweepStart() + g.sweepLength() + H;
    for (int32_t n = static_cast<int32_t>(delay) + lo; n < static_cast<int32_t>(delay) + hi + 1; ++n)
    {
        const double t = n - delay;
        const auto c = static_cast<int32_t>(std::floor(t));
        double acc = 0.0;
        for (int32_t k = c - H + 1; k <= c + H; ++k)
        {
            if (k < 0 || k >= g.length()) continue;
            const double u = t - k;
            const double s = std::fabs(u) < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u);
            const double w = 0.5 * (1.0 + std::cos(kPi * u / (H + 1)));
            acc += clean[static_cast<size_t>(k)] * s * w;
        }
        x[static_cast<size_t>(n)] = static_cast<float>(acc);
    }
    const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
    REQUIRE(r.lag == 1000);
    REQUIRE(std::fabs(r.fractional - delay) < 0.1);
}

TEST_CASE("correlator_pbr_is_rms_outside_pm16_over_full_range")
{
    auto x = delayed(2345, -6.0);
    addNoise(x, -50.0);
    const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
    const auto c = referenceCorr(x);
    size_t m = 0;
    for (size_t d = 1; d < c.size(); ++d) if (std::fabs(c[d]) > std::fabs(c[m])) m = d;
    REQUIRE(m == 2345);
    double s = 0.0; size_t cnt = 0;
    for (size_t d = 0; d < c.size(); ++d)
        if (d + 16 < m || d > m + 16) { s += c[d] * c[d]; ++cnt; }
    const double ref = 20.0 * std::log10(std::fabs(c[m]) / std::sqrt(s / static_cast<double>(cnt)));
    CAPTURE(ref, r.pbrDb);
    REQUIRE(std::fabs(r.pbrDb - ref) < 0.1);
    REQUIRE(std::fabs(r.mainAmp - c[m]) < 1e-3 * std::fabs(c[m]));
}

TEST_CASE("correlator_negative_controls")
{
    SECTION("a: no return -> NoReturn")
    {
        const std::vector<float> x(static_cast<size_t>(probe().length()), 0.0f);
        const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
        REQUIRE(r.returnPeakDbfs < -60.0);
        const auto v = evaluateLock(three(r, r, r));
        REQUIRE_FALSE(v.locked);
        REQUIRE(v.reason == Reason::NoReturn);
    }
    SECTION("b: noise only -> LowSnr or NoReturn")
    {
        for (unsigned seed : { 1u, 2u, 3u })
        {
            std::vector<float> x(static_cast<size_t>(probe().length()), 0.0f);
            addNoise(x, -30.0, seed);
            const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
            const auto v = evaluateLock(three(r, r, r));
            REQUIRE_FALSE(v.locked);
            REQUIRE((v.reason == Reason::LowSnr || v.reason == Reason::NoReturn));
        }
    }
    SECTION("c: echo at +100 samples, -6 dB -> AmbiguousPeak")
    {
        auto x = delayed(500, -3.0);
        const auto e = delayed(600, -9.0);
        for (size_t n = 0; n < x.size(); ++n) x[n] += e[n];
        const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
        REQUIRE(r.lag == 500);
        REQUIRE(r.altPeakRelDb >= -12.0);
        REQUIRE(std::abs(r.altLag - 600) <= 1);
        const auto v = evaluateLock(three(r, r, r));
        REQUIRE_FALSE(v.locked);
        REQUIRE(v.reason == Reason::AmbiguousPeak);
    }
    SECTION("d: -50 dB attenuated return (peak below -60 dBFS) -> LowSnr or NoReturn")
    {
        auto x = delayed(300, -50.0);
        addNoise(x, -90.0);
        const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
        REQUIRE(r.returnPeakDbfs < -60.0);
        const auto v = evaluateLock(three(r, r, r));
        REQUIRE_FALSE(v.locked);
        REQUIRE((v.reason == Reason::LowSnr || v.reason == Reason::NoReturn));
    }
    SECTION("e: one sample at 1.01 -> Clipped")
    {
        auto x = delayed(300, 0.0);
        x[static_cast<size_t>(probe().sweepStart() + 300 + 20000)] = 1.01f;
        const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
        REQUIRE(r.clipped);
        const auto v = evaluateLock(three(r, r, r));
        REQUIRE_FALSE(v.locked);
        REQUIRE(v.reason == Reason::Clipped);
    }
}

TEST_CASE("lock_range_boundary")
{
    for (int32_t B : { 32, 64, 2048 })
    {
        CAPTURE(B);
        const auto ok = goodProbe(kLMax - 2 * B);
        const auto v = evaluateLock(three(ok, ok, ok, B));
        REQUIRE(v.locked);
        REQUIRE(v.reason == Reason::None);
        REQUIRE(v.lag == kLMax - 2 * B);
        const auto over = goodProbe(kLMax - 2 * B + 1);
        const auto w = evaluateLock(three(over, over, over, B));
        REQUIRE_FALSE(w.locked);
        REQUIRE(w.reason == Reason::OutOfRange);
    }
    const auto neg = goodProbe(-1);
    REQUIRE(evaluateLock(three(neg, neg, neg)).reason == Reason::OutOfRange);
}

TEST_CASE("lock_three_lags_must_be_identical")
{
    const auto v = evaluateLock(three(goodProbe(100), goodProbe(100), goodProbe(101)));
    REQUIRE_FALSE(v.locked);
    REQUIRE(v.reason == Reason::LagMismatch);
}

TEST_CASE("lock_generation_changed_rejects_with_KeyChange")
{
    auto in = three(goodProbe(100), goodProbe(100), goodProbe(100));
    in.genNow = in.genStart + 1;
    const auto v = evaluateLock(in);
    REQUIRE_FALSE(v.locked);
    REQUIRE(v.reason == Reason::KeyChange);
}

TEST_CASE("lock_signal_quality_reasons_per_probe")
{
    auto bad = goodProbe(100);
    bad.pbrDb = 19.9;
    REQUIRE(evaluateLock(three(goodProbe(100), bad, goodProbe(100))).reason == Reason::LowSnr);
    bad = goodProbe(100); bad.snrDb = 19.9;
    REQUIRE(evaluateLock(three(goodProbe(100), goodProbe(100), bad)).reason == Reason::LowSnr);
    bad = goodProbe(100); bad.altPeakRelDb = -12.0;
    REQUIRE(evaluateLock(three(bad, goodProbe(100), goodProbe(100))).reason == Reason::AmbiguousPeak);
    bad = goodProbe(100); bad.altPeakRelDb = -12.1;
    REQUIRE(evaluateLock(three(bad, goodProbe(100), goodProbe(100))).locked);
}

TEST_CASE("correlator_analyse_is_allocation_free")
{
    const auto x = delayed(1234);
    const auto& c = corr();   // construction allocates by design; only analyse() is guarded
    AllocGuard g;
    REQUIRE_NO_ALLOC(c.analyse(x.data(), static_cast<int32_t>(x.size()), 64));
}

TEST_CASE("correlator_analyse_142k_timing_info")
{
    const auto x = delayed(4321);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = corr().analyse(x.data(), static_cast<int32_t>(x.size()), 64);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.lag == 4321);
    WARN("analyse(" << x.size() << " samples) = " << ms << " ms (target < 150 ms, informational)");
}

TEST_CASE("correlator_click_lag_exact_and_background_only_over_the_captured_range")
{
    static const ProbeGenerator g { ProbeSpec::click() };
    static const Correlator c { g };
    const int32_t pre = g.sweepStart();
    for (int32_t D : { 0, 5, 237, 4000, 21000 })
    {
        CAPTURE(D);
        const int32_t n = std::min(g.length(), pre + D + 960);   // the window closes 20 ms after the return
        std::vector<float> x(static_cast<size_t>(n));
        const float sigma = 0.001f;   // -60 dBFS RMS noise; click return 0.5 → expected PBR 20·log10(0.5/0.001) = 54 dB
        uint32_t h = 12345u;
        for (auto& v : x) { h = h * 1664525u + 1013904223u; v = sigma * std::sqrt(3.0f) * (static_cast<float>(h >> 8) / 8388608.0f - 1.0f); }
        x[static_cast<size_t>(pre + D)] = 0.5f;
        const auto r = c.analyse(x.data(), n, 64);
        REQUIRE(r.lag == D);
        REQUIRE(std::fabs(r.pbrDb - 20.0 * std::log10(0.5 / sigma)) < 1.5);   // zero padding past n must not inflate it
        REQUIRE(r.snrDb > 40.0);
        REQUIRE(r.altPeakRelDb < -12.0);
    }
}
