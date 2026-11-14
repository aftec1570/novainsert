// ProbeGenerator (plan Task 12, F1 §8 "Probe"): exponential sine sweep 40 Hz–18 kHz, 1.0 s, 10 ms raised-cosine edges,
// 100 ms pre-silence, post-silence L_max/fs + 0.5 s; deterministic, sample peak exact, band-limited, matched inverse filter.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include "engine/probe.hpp"

using ni::engine::ProbeGenerator;
using ni::engine::ProbeSpec;

namespace
{
constexpr double kPi = 3.14159265358979323846;

void fft(std::vector<std::complex<double>>& a)
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
        const std::complex<double> w(std::cos(-2 * kPi / static_cast<double>(len)), std::sin(-2 * kPi / static_cast<double>(len)));
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
}

std::vector<float> rendered(const ProbeGenerator& g, float peakDb)
{
    std::vector<float> x(static_cast<size_t>(g.length()), 123.0f);
    g.render(x.data(), peakDb);
    return x;
}
} // namespace

TEST_CASE("probe_length_follows_the_spec_formula")
{
    const ProbeGenerator g { ProbeSpec {} };
    // 48000 × (0.1 + 1.0 + 65536/48000 + 0.5) = 4800 + 48000 + 65536 + 24000
    REQUIRE(g.length() == 142336);
    REQUIRE(g.sweepStart() == 4800);
    REQUIRE(g.sweepLength() == 48000);
}

TEST_CASE("probe_render_is_deterministic_and_bit_identical")
{
    const ProbeGenerator a { ProbeSpec {} }, b { ProbeSpec {} };
    const auto x = rendered(a, -36.0f), y = rendered(a, -36.0f), z = rendered(b, -36.0f);
    REQUIRE(x == y);
    REQUIRE(x == z);
}

TEST_CASE("probe_sample_peak_matches_the_requested_level")
{
    const ProbeGenerator g { ProbeSpec {} };
    for (const float db : { -36.0f, -24.0f, -12.0f })
    {
        const auto x = rendered(g, db);
        float pk = 0.0f;
        for (const float v : x) pk = std::max(pk, std::fabs(v));
        CAPTURE(db, pk);
        REQUIRE(std::fabs(20.0 * std::log10(static_cast<double>(pk)) - db) <= 0.01);
    }
}

TEST_CASE("probe_pre_and_post_regions_are_exactly_zero")
{
    const ProbeGenerator g { ProbeSpec {} };
    const auto x = rendered(g, -36.0f);
    const auto s = static_cast<size_t>(g.sweepStart()), e = s + static_cast<size_t>(g.sweepLength());
    for (size_t i = 0; i < s; ++i) REQUIRE(x[i] == 0.0f);
    for (size_t i = e; i < x.size(); ++i) REQUIRE(x[i] == 0.0f);
    REQUIRE(x[s + 1000] != 0.0f);
}

TEST_CASE("probe_edges_follow_a_raised_cosine_envelope")
{
    const ProbeGenerator g { ProbeSpec {} };
    const double a = std::pow(10.0, -36.0 / 20.0);
    const auto x = rendered(g, -36.0f);
    const auto s = static_cast<size_t>(g.sweepStart()), L = static_cast<size_t>(g.sweepLength());
    constexpr size_t E = 480;
    bool sawNearEnvelope = false;
    for (size_t k = 0; k < E; ++k)
    {
        const double env = a * 0.5 * (1.0 - std::cos(kPi * static_cast<double>(k) / static_cast<double>(E)));
        CAPTURE(k);
        REQUIRE(std::fabs(x[s + k]) <= env + 1e-7);
        REQUIRE(std::fabs(x[s + L - 1 - k]) <= env + 1e-7);
        sawNearEnvelope |= k > E / 2 && std::fabs(x[s + k]) > 0.5 * env;
    }
    REQUIRE(sawNearEnvelope);
}

TEST_CASE("probe_spectrum_is_band_limited")
{
    // Above the band: ≥ 60 dB down (brief). Below the band the brief's "< 30 Hz ≥ 60 dB down" is unattainable with the spec's
    // own probe (F1 §8: 40 Hz start, 10 ms edges – the edge is < half a 40 Hz cycle and the sweep's skirt at 30 Hz is only
    // 0.4 octave away; even 100 ms edges reach −34 dB). Ruled: pin the spec design's measured low skirt instead (−15 dB < 30 Hz,
    // −26.5 dB < 10 Hz, DC −29.9 dB relative to the in-band maximum) with ~1.5 dB margin.
    const ProbeGenerator g { ProbeSpec {} };
    const auto x = rendered(g, -36.0f);
    std::vector<std::complex<double>> a(262144);
    for (size_t i = 0; i < x.size(); ++i) a[i] = x[i];
    fft(a);
    const double df = 48000.0 / static_cast<double>(a.size());
    double inBand = 0.0, below30 = 0.0, below10 = 0.0, above = 0.0;
    for (size_t k = 0; k < a.size() / 2; ++k)
    {
        const double f = static_cast<double>(k) * df, m = std::abs(a[k]);
        if (f < 10.0) below10 = std::max(below10, m);
        if (f < 30.0) below30 = std::max(below30, m);
        else if (f > 20000.0) above = std::max(above, m);
        else inBand = std::max(inBand, m);
    }
    const double dc = 20 * std::log10(std::abs(a[0]) / inBand), b30 = 20 * std::log10(below30 / inBand),
                 b10 = 20 * std::log10(below10 / inBand), ab = 20 * std::log10(above / inBand);
    CAPTURE(dc, b30, b10, ab);
    REQUIRE(ab <= -60.0);
    REQUIRE(b30 <= -13.5);
    REQUIRE(b10 <= -25.0);
    REQUIRE(dc <= -28.5);
}

TEST_CASE("probe_inverse_filter_compresses_the_sweep_to_one_sharp_peak")
{
    const ProbeGenerator g { ProbeSpec {} };
    const auto x = rendered(g, 0.0f);
    const auto& inv = g.inverseFilter();
    REQUIRE(static_cast<int32_t>(inv.size()) == g.sweepLength());
    const auto s = static_cast<size_t>(g.sweepStart()), L = inv.size();
    // correlation-by-convolution around the expected lag only (sweep end aligned with the inverse filter's start)
    auto conv = [&](size_t n) { double acc = 0; for (size_t k = 0; k < L; ++k) acc += static_cast<double>(x[s + n - k]) * static_cast<double>(inv[k]); return acc; };
    const double peak = conv(L - 1);
    REQUIRE(std::fabs(peak - 1.0) < 1e-3);
    for (const size_t off : { size_t { 3 }, size_t { 16 }, size_t { 20 }, size_t { 50 }, size_t { 200 }, size_t { 2000 } })
    {
        CAPTURE(off);
        REQUIRE(std::fabs(conv(L - 1 - off)) < 0.1);   // sharp lobe: 0.07 at ±3, ≤ 0.03 from ±10 (1/f-weighted inverse: 0.87 at ±16)
        REQUIRE(std::fabs(conv(L - 1 + off)) < 0.1);
    }
}

TEST_CASE("probe_click_is_one_sample_at_the_level_with_an_identity_inverse")
{
    const ProbeGenerator g { ProbeSpec::click() };
    REQUIRE(g.sweepLength() == 1);
    REQUIRE(g.sweepStart() == 960);                  // 20 ms of exact silence before the click (noise floor)
    REQUIRE(g.length() == 960 + 1 + 21600);          // the longest wait: 0.45 s (a loop beyond that fails safely)
    const auto x = rendered(g, -6.0f);
    for (int32_t i = 0; i < g.length(); ++i)
        if (i == g.sweepStart()) REQUIRE(std::fabs(x[static_cast<size_t>(i)] - std::pow(10.0f, -6.0f / 20.0f)) < 1e-6f);
        else REQUIRE(x[static_cast<size_t>(i)] == 0.0f);
    REQUIRE(g.inverseFilter().size() == 1);
    REQUIRE(g.inverseFilter()[0] == 1.0f);
}
