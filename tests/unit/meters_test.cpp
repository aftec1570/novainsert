// MeterEngine (plan Task 24, R-GUI-1 meter part): peak, 10 s peak hold, 50 ms RMS, 300 ms VU against the device VU
// reference, clip (> 0 dBFS, latched 2 s) separate from near-full-scale (≥ −0.1 dBFS). feed() is RT: allocation-free.
#include <catch2/catch_test_macros.hpp>
#include <numbers>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "engine/meters.hpp"
#include "rt/alloc_guard.hpp"

using namespace ni::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kFs = 48000;
constexpr int kBlock = 256;

// Feed `seconds` of a 1 kHz sine of amplitude a (both channels) in kBlock pieces.
void sine(MeterEngine& m, float a, double seconds, int64_t& phase)
{
    std::vector<float> buf(kBlock);
    for (int64_t done = 0, total = static_cast<int64_t>(seconds * kFs); done < total; done += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
            buf[static_cast<size_t>(i)] = a * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(phase + i) / kFs));
        phase += kBlock;
        m.feed(buf.data(), buf.data(), kBlock);
    }
}
void silence(MeterEngine& m, double seconds)
{
    std::vector<float> z(kBlock, 0.0f);
    for (int64_t done = 0, total = static_cast<int64_t>(seconds * kFs); done < total; done += kBlock) m.feed(z.data(), z.data(), kBlock);
}
void single(MeterEngine& m, float v)
{
    std::vector<float> b(kBlock, 0.0f);
    b[10] = v;
    m.feed(b.data(), b.data(), kBlock);
}
} // namespace

TEST_CASE("meters_minus6_sine_peak_rms_vu")
{
    MeterEngine m;
    m.prepare(kFs);
    int64_t ph = 0;
    sine(m, std::pow(10.0f, -6.0f / 20.0f), 2.0, ph);
    const auto r = m.read(-20.0f);
    for (int c = 0; c < 2; ++c)
    {
        REQUIRE_THAT(r.peakDb[c], WithinAbs(-6.0, 0.05));
        REQUIRE_THAT(r.rmsDb[c], WithinAbs(-6.0, 0.1));   // AES17: a sine's RMS level equals its peak level
        REQUIRE_THAT(r.vu[c], WithinAbs(14.0, 0.1));
        REQUIRE_FALSE(r.clip[c]);
        REQUIRE_FALSE(r.nearFullScale[c]);
    }
}

TEST_CASE("meters_clip_latched_two_seconds")
{
    MeterEngine m;
    m.prepare(kFs);
    single(m, 1.001f);
    silence(m, 1.9);
    REQUIRE(m.read(-20.0f).clip[0]);
    silence(m, 0.2);
    REQUIRE_FALSE(m.read(-20.0f).clip[0]);
}

TEST_CASE("meters_near_full_scale_is_not_clip")
{
    MeterEngine m;
    m.prepare(kFs);
    single(m, 0.99f);
    const auto r = m.read(-20.0f);
    REQUIRE(r.nearFullScale[0]);
    REQUIRE_FALSE(r.clip[0]);
    single(m, 0.98f);   // −0.175 dBFS: below the near-full-scale line, but the 0.99 latch still holds
    REQUIRE(m.read(-20.0f).nearFullScale[0]);
}

TEST_CASE("meters_hold_ten_seconds_then_decays")
{
    MeterEngine m;
    m.prepare(kFs);
    int64_t ph = 0;
    sine(m, 0.5f, 0.1, ph);
    silence(m, 9.8);
    REQUIRE_THAT(m.read(-20.0f).peakHoldDb[0], WithinAbs(-6.02, 0.05));
    REQUIRE(m.read(-20.0f).peakDb[0] < -60.0f);   // the live peak has fallen
    silence(m, 1.0);
    REQUIRE(m.read(-20.0f).peakHoldDb[0] < -7.0f);
    m.resetHold();
    REQUIRE(m.read(-20.0f).peakHoldDb[0] <= -120.0f);
}

TEST_CASE("meters_silence_reads_floor")
{
    MeterEngine m;
    m.prepare(kFs);
    const auto r = m.read(-20.0f);
    REQUIRE(r.peakDb[0] <= -120.0f);
    REQUIRE(r.rmsDb[1] <= -120.0f);
}

TEST_CASE("meters_feed_is_allocation_free")
{
    MeterEngine m;
    m.prepare(kFs);
    std::vector<float> b(kBlock, 0.25f);
    AllocGuard g;
    REQUIRE_NO_ALLOC(m.feed(b.data(), b.data(), kBlock));
    REQUIRE_NO_ALLOC(m.read(-20.0f));
}

TEST_CASE("meters_rectified_average_of_reference_sine")
{
    MeterEngine m; m.prepare(kFs);
    int64_t ph = 0;
    sine(m, 0.1f * std::sqrt(2.0f), 1.0, ph);   // RMS −20 dBFS
    const float want = 0.1f * 2.0f * std::sqrt(2.0f) / std::numbers::pi_v<float>;
    const auto r = m.read(-20.0f);
    CHECK(std::fabs(r.rect[0] - want) < 0.01f * want);
    CHECK(std::fabs(r.rect[1] - want) < 0.01f * want);
}

TEST_CASE("meters_null_right_channel_reads_floor")
{
    MeterEngine m; m.prepare(kFs);
    std::vector<float> x(kBlock, 0.5f);
    m.feed(x.data(), nullptr, kBlock);
    const auto r = m.read(-20.0f);
    CHECK(r.peakDb[0] > -7.0f);
    CHECK(r.peakDb[1] == MeterEngine::kFloorDb);
    CHECK(r.rect[1] == 0.0f);
}

TEST_CASE("meters_feed_gain_scales_the_levels_but_clip_watches_the_raw_sample")
{
    // RETURN meter after the return trim (user 2026-11-05): the shown level follows the trim, CLIP / near-full-scale stay
    // at the converter input, where the overload happens
    MeterEngine m;
    m.prepare(kFs);
    std::vector<float> hot(kBlock, 1.001f);
    m.feed(hot.data(), hot.data(), kBlock, 0.5f, 0.25f);
    const auto r = m.read(-20.0f);
    REQUIRE(std::fabs(r.peakDb[0] - (-6.01f)) < 0.05f);
    REQUIRE(std::fabs(r.peakDb[1] - (-12.03f)) < 0.05f);
    REQUIRE(r.clip[0]); REQUIRE(r.clip[1]);
    REQUIRE(r.nearFullScale[0]); REQUIRE(r.nearFullScale[1]);
    std::vector<float> soft(kBlock, 0.5f);
    MeterEngine n; n.prepare(kFs);
    n.feed(soft.data(), nullptr, kBlock, 2.0f, 1.0f);   // trimmed to full scale: a level, not a clip
    const auto q = n.read(-20.0f);
    REQUIRE(std::fabs(q.peakDb[0]) < 0.05f);
    REQUIRE_FALSE(q.clip[0]); REQUIRE_FALSE(q.nearFullScale[0]);
}
