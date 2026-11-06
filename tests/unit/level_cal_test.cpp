// Level CAL maths (VU GUI plan Task 1, spec 2026-11-02 §2/§7): +4 dBu send trim, return unity per channel, LINK with range
// clamping, the 1 kHz reference tone and the loop-gain measurement over the captured return.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "engine/level_cal.hpp"

using namespace ni::engine;
using Catch::Matchers::WithinAbs;

namespace
{
float rmsDb(const float* x, int n)
{
    double s = 0;
    for (int i = 0; i < n; ++i) s += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(10.0 * std::log10(s / n));
}
}

TEST_CASE("level_cal_send_trim_for_plus4")
{
    CHECK_THAT(cal::sendTrimForPlus4(24.0f, -20.0f), WithinAbs(0.0, 1e-4));
    CHECK_THAT(cal::sendTrimForPlus4(24.0f, -18.0f), WithinAbs(-2.0, 1e-4));
    CHECK_THAT(cal::sendTrimForPlus4(20.2f, -20.0f), WithinAbs(3.8, 1e-4));
}

TEST_CASE("level_cal_return_trim_eff")
{
    CHECK_THAT(cal::returnTrimEff(0.0f, -1.5f), WithinAbs(1.5, 1e-5));
    CHECK_THAT(cal::returnTrimEff(3.8f, -0.3f), WithinAbs(-3.5, 1e-5));
}

TEST_CASE("level_cal_link_follows")
{
    auto r = cal::linkFollow(0, 0, 3, 0, true, -24, 12);
    CHECK_THAT(r.sendDb, WithinAbs(3, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-3, 1e-5));
    r = cal::linkFollow(0, 0, 3, 0, false, -24, 12);
    CHECK_THAT(r.sendDb, WithinAbs(3, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(0, 1e-5));
    r = cal::linkFollow(0, 0, 3, 1, true, -24, 12);   // both moved: no link
    CHECK_THAT(r.sendDb, WithinAbs(3, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(1, 1e-5));
    r = cal::linkFollow(0, 0, 0, -2, true, -24, 12);  // return moved: send follows
    CHECK_THAT(r.sendDb, WithinAbs(2, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-2, 1e-5));
}

TEST_CASE("level_cal_link_clamps_both_sides")
{
    auto r = cal::linkFollow(10, -22, 12, -22, true, -24, 12);
    CHECK_THAT(r.sendDb, WithinAbs(12, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
    r = cal::linkFollow(11, -23, 12, -23, true, -24, 12);
    CHECK_THAT(r.sendDb, WithinAbs(12, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
    r = cal::linkFollow(12, -24, 12, -24, true, -24, 12);
    CHECK_THAT(r.sendDb, WithinAbs(12, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
    r = cal::linkFollow(5, -24, 7, -24, true, -24, 12);   // return at its edge: send stops too
    CHECK_THAT(r.sendDb, WithinAbs(5, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
    r = cal::linkFollow(5, -20, 9, -20, true, -24, 12);   // partial: send limited to +4 − (−20 − −24) = 9 → 9, −24
    CHECK_THAT(r.sendDb, WithinAbs(9, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
    r = cal::linkFollow(5, -22, 9, -22, true, -24, 12);   // only 2 dB room on the return
    CHECK_THAT(r.sendDb, WithinAbs(7, 1e-5)); CHECK_THAT(r.returnDb, WithinAbs(-24, 1e-5));
}

TEST_CASE("level_cal_tone")
{
    // AES17 (user 2026-11-05 "set it to hit the HW unit at exactly +4 dBu"): a −20 dBFS tone peaks at −20 dBFS, so on a converter
    // aligned to +24 dBu at 0 dBFS (full-scale sine) it leaves at exactly +4 dBu. Its plain RMS is 3.01 dB lower.
    std::vector<float> t(4800);
    cal::makeTone(t.data(), 4800, -20.0f);
    float pk = 0.0f;
    for (float x : t) pk = std::max(pk, std::fabs(x));
    CHECK_THAT(20.0f * std::log10(pk), WithinAbs(-20.0, 0.01));
    CHECK_THAT(rmsDb(t.data(), 4800), WithinAbs(-23.01, 0.01));
    CHECK_THAT(20.0f * std::log10(pk) + 24.0f, WithinAbs(4.0, 0.01));   // dBu at the converter output, 24 dBu alignment
    CHECK_THAT(t[0], WithinAbs(t[48], 1e-6));
    CHECK_THAT(t[4799 - 47], WithinAbs(t[4799 - 47 - 48], 1e-6));
}

TEST_CASE("level_cal_measure_loop_gain")
{
    const int delay = 300, n = delay + cal::kSettleSamples + cal::kWindowSamples;
    std::vector<float> tone(4800), l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
    cal::makeTone(tone.data(), 4800, -20.0f);
    const float gl = std::pow(10.0f, -1.5f / 20), gr = std::pow(10.0f, 0.7f / 20);
    for (int i = delay; i < n; ++i) { l[size_t(i)] = tone[size_t((i - delay) % 4800)] * gl; r[size_t(i)] = tone[size_t((i - delay) % 4800)] * gr; }
    const float sent[2] = { -20.0f, -20.0f };
    auto g = cal::measureLoopGain(l.data(), r.data(), n, delay + cal::kSettleSamples, cal::kWindowSamples, sent, true);
    CHECK_THAT(g.gainDb[0], WithinAbs(-1.5, 0.05));
    CHECK_THAT(g.gainDb[1], WithinAbs(0.7, 0.05));
    CHECK_FALSE(g.clipped);
    CHECK_FALSE(g.noSignal);

    SECTION("a sample at 0.99 in the window clips")
    {
        l[size_t(delay + cal::kSettleSamples + 1000)] = 0.99f;
        CHECK(cal::measureLoopGain(l.data(), r.data(), n, delay + cal::kSettleSamples, cal::kWindowSamples, sent, true).clipped);
    }
    SECTION("silence is no signal")
    {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        CHECK(cal::measureLoopGain(l.data(), r.data(), n, delay + cal::kSettleSamples, cal::kWindowSamples, sent, true).noSignal);
    }
    SECTION("mono measures L only")
    {
        std::fill(r.begin(), r.end(), 0.0f);
        auto m = cal::measureLoopGain(l.data(), r.data(), n, delay + cal::kSettleSamples, cal::kWindowSamples, sent, false);
        CHECK_THAT(m.gainDb[0], WithinAbs(-1.5, 0.05));
        CHECK(m.gainDb[1] == 0.0f);
        CHECK_FALSE(m.noSignal);
    }
}

TEST_CASE("level_cal_link_ignores_normalisation_round_off")
{
    auto r = cal::linkFollow(3.0f, -3.0f, 3.0f + 5e-5f, -3.0f, true, -24, 12);   // not a move
    CHECK_THAT(r.returnDb, WithinAbs(-3.0, 1e-7));
    r = cal::linkFollow(3.0f, -3.0f, 3.001f, -3.0f, true, -24, 12);
    CHECK_THAT(r.returnDb, WithinAbs(-3.001, 1e-5));
}

// User 2026-11-06: "With the link enabled turning the input down made the input knob glitch on low settings" — the knob was
// dragged past the point where the other trim hits its range edge, then pulled back by LINK every timer tick (and the audio
// briefly ran at the over-dragged gain). The knob itself must stop where LINK stops.
TEST_CASE("level_cal_link_limit_stops_the_dragged_trim_where_the_other_hits_its_edge")
{
    // linkLimit(attempted, self now, other now, link, lo, hi): the sum self + other is what LINK keeps
    CHECK_THAT(cal::linkLimit(-20, 0, 0, true, -24, 12), WithinAbs(-12, 1e-5));    // send down, return 0 → return +12 is the edge
    CHECK_THAT(cal::linkLimit(-10, 0, 0, true, -24, 12), WithinAbs(-10, 1e-5));    // inside the room: untouched
    CHECK_THAT(cal::linkLimit(-20, 0, 6, true, -24, 12), WithinAbs(-6, 1e-5));     // return already +6: only 6 dB of room
    CHECK_THAT(cal::linkLimit(-20, 0, 0, false, -24, 12), WithinAbs(-20, 1e-5));   // LINK off: only the own range
    CHECK_THAT(cal::linkLimit(-30, 0, 0, false, -24, 12), WithinAbs(-24, 1e-5));
    CHECK_THAT(cal::linkLimit(12, 8, -20, true, -24, 12), WithinAbs(12, 1e-5));    // up: return −20 has 4 dB room, send 8 → 12
    CHECK_THAT(cal::linkLimit(12, 8, -22, true, -24, 12), WithinAbs(10, 1e-5));    // up: return −22 has 2 dB room
    CHECK_THAT(cal::linkLimit(-30, -24, 12, true, -24, 12), WithinAbs(-24, 1e-5)); // both at their edges: held, never inverted
}
