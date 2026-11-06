// BARS meter view (user request 2026-11-05: switch between the VU meters and ReaInsert-style level bars, SEND above RETURN,
// dBu scale above and dBFS scale below each group). Pure scale maths and the peak-bar fall, JUCE-free.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "gui/bar_scale.hpp"

using namespace ni::gui;
using Catch::Matchers::WithinAbs;

TEST_CASE("bar_position_is_linear_from_minus_60_to_0_dbfs_and_clamps")
{
    CHECK_THAT(barPosition(-60.0f), WithinAbs(0.0, 1e-6));
    CHECK_THAT(barPosition(0.0f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(barPosition(-30.0f), WithinAbs(0.5, 1e-6));
    CHECK_THAT(barPosition(-120.0f), WithinAbs(0.0, 1e-6));   // meter floor: an empty bar
    CHECK_THAT(barPosition(3.0f), WithinAbs(1.0, 1e-6));      // over full scale: a full bar (the CLIP lamp says the rest)
}

TEST_CASE("dbu_ticks_follow_the_converter_alignment_and_stay_on_the_scale")
{
    // Apollo +4 / 24 dBu: +4 dBu sits at −20 dBFS (the SEND 0 VU), +24 dBu at 0 dBFS, −30 dBu at −54 dBFS
    const auto t24 = dbuTicks(24.0f);
    REQUIRE(t24.size() == 10);   // −30 −24 −18 −12 −6 0 +4 +12 +18 +24
    CHECK(t24.front().dbu == -30.0f);
    CHECK_THAT(t24.front().dbfs, WithinAbs(-54.0, 1e-5));
    CHECK(t24.back().dbu == 24.0f);
    CHECK_THAT(t24.back().dbfs, WithinAbs(0.0, 1e-5));
    bool plus4 = false;
    for (const auto& t : t24) if (t.dbu == 4.0f) { plus4 = true; CHECK_THAT(t.dbfs, WithinAbs(-20.0, 1e-5)); CHECK(t.reference); }
    CHECK(plus4);
    // Apollo Twin MkII 20.2 dBu: +24 dBu would be +3.8 dBFS, off the scale; +18 dBu = −2.2 dBFS stays
    const auto t20 = dbuTicks(20.2f);
    CHECK(t20.back().dbu == 18.0f);
    CHECK_THAT(t20.back().dbfs, WithinAbs(-2.2, 1e-4));
    // the dBFS scale is fixed: every 6 dB from −60 to 0
    const auto f = dbfsTicks();
    REQUIRE(f.size() == 11);
    CHECK(f.front() == -60.0f);
    CHECK(f.back() == 0.0f);
}

TEST_CASE("peak_bar_rises_at_once_and_falls_at_the_meter_rate")
{
    BarFall b;
    CHECK(b.shownDb <= kBarFloorDb);
    CHECK_THAT(b.advance(1.0 / 60.0, -10.0f), WithinAbs(-10.0, 1e-5));          // instant rise
    CHECK_THAT(b.advance(0.5, -120.0f), WithinAbs(-10.0 - 0.5 * kBarFallDbPerS, 1e-4));   // falls at the fixed rate
    CHECK_THAT(b.advance(0.1, -12.0f), WithinAbs(-12.0, 1e-5));                // a louder input than the falling bar wins
    for (int i = 0; i < 600; ++i) b.advance(0.1, -120.0f);
    CHECK(b.shownDb >= kBarFloorDb);                                           // never below the floor
    CHECK_THAT(b.advance(5.0, -120.0f), WithinAbs(kBarFloorDb, 1e-5));         // a stalled frame drains at most to the floor
}

// Three bar looks (user 2026-11-06: "All 3 looks cool, make all 3 of them selectable"): the meter view rocker is
// VU | TUBE | LED | TWIN. The stored per-process preference keeps its old meaning: 1 (was BARS) is TUBE.
TEST_CASE("meter_view_choices_map_the_old_bars_preference_to_tube_and_clamp")
{
    CHECK(meterViewFromStored(0) == MeterView::Vu);
    CHECK(meterViewFromStored(1) == MeterView::Tube);   // a session that chose BARS before keeps the bars
    CHECK(meterViewFromStored(2) == MeterView::Led);
    CHECK(meterViewFromStored(3) == MeterView::Twin);
    CHECK(meterViewFromStored(7) == MeterView::Vu);     // unknown: the VU faces
    CHECK(meterViewFromStored(-1) == MeterView::Vu);
    CHECK(isBarView(MeterView::Tube));
    CHECK(isBarView(MeterView::Twin));
    CHECK_FALSE(isBarView(MeterView::Vu));
}

TEST_CASE("led_segment_is_full_under_rms_dimmed_up_to_the_peak_and_lit_at_the_hold")
{
    // segment k spans [−60 + k, −59 + k) dBFS; RMS −20, peak −14, hold −11.2
    CHECK(ledIntensity(-30.0f, -20.0f, -14.0f, -11.2f) == 1.0f);              // under the RMS: full
    CHECK(ledIntensity(-21.0f, -20.0f, -14.0f, -11.2f) == 1.0f);
    CHECK(ledIntensity(-20.0f, -20.0f, -14.0f, -11.2f) == kLedPeakIntensity); // RMS … peak: the dimmer peak band
    CHECK(ledIntensity(-15.0f, -20.0f, -14.0f, -11.2f) == kLedPeakIntensity);
    CHECK(ledIntensity(-14.0f, -20.0f, -14.0f, -11.2f) == 0.0f);              // above the peak: dark
    CHECK(ledIntensity(-12.0f, -20.0f, -14.0f, -11.2f) == 1.0f);              // the hold segment
    CHECK(ledIntensity(-11.0f, -20.0f, -14.0f, -11.2f) == 0.0f);
    CHECK(ledIntensity(-60.0f, -120.0f, -120.0f, -120.0f) == 0.0f);           // silence: nothing lit
}

TEST_CASE("bar_zones_follow_the_scale_green_yellow_orange_red")
{
    CHECK(barZone(-30.0f) == 0);   // working range
    CHECK(barZone(-20.0f) == 1);   // from 0 VU (−20 dBFS reference) up
    CHECK(barZone(-9.0f) == 2);
    CHECK(barZone(-3.0f) == 3);    // the last 3 dB
    CHECK(barZone(-0.5f) == 3);
}
