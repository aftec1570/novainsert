// VU needle ballistics (VU GUI plan Task 2, spec §5, IEC 60268-17): ζ 0.81, ωn 13.4 rad/s → 99 % in 300 ms ±10 %, overshoot
// 1–1.5 %; fixed 1 kHz integration independent of the frame rate; elastic bounce (0.25) at the stop pins.
#include <catch2/catch_test_macros.hpp>
#include <numbers>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "gui/vu_needle.hpp"

using namespace ni::gui;
using Catch::Matchers::WithinAbs;

TEST_CASE("vu_needle_step_response")
{
    VuNeedle n;
    const float target = vuToPosition(0.0f);
    double t = 0, reached = -1; float maxPos = 0;
    while (t < 1.0)
    {
        n.advance(1.0 / 60.0, 1.0f);
        t += 1.0 / 60.0;
        maxPos = std::max(maxPos, n.position());
        if (reached < 0 && n.position() >= 0.99f * target) reached = t;
    }
    // first crossing is seen at 60 Hz frame granularity (±17 ms)
    CHECK(reached >= 0.270 - 1.0 / 60.0);
    CHECK(reached <= 0.330 + 1.0 / 60.0);
    const float over = (maxPos - target) / target;
    CHECK(over >= 0.010f);
    CHECK(over <= 0.015f);
}

TEST_CASE("vu_needle_frame_rate_independent")
{
    VuNeedle a, b;
    for (int i = 0; i < 15; ++i) a.advance(1.0 / 30.0, 0.7f);
    for (int i = 0; i < 72; ++i) b.advance(1.0 / 144.0, 0.7f);
    CHECK_THAT(a.position(), WithinAbs(b.position(), 1e-4));
}

TEST_CASE("vu_needle_pin_bounce")
{
    VuNeedle n;
    float prevV = 0; bool hit = false;
    for (int i = 0; i < 1000; ++i)
    {
        n.advance(0.001, 4.0f);
        CHECK(n.position() <= 1.05f + 1e-6f);
        if (! hit && n.position() >= 1.05f - 1e-6f)
        {
            hit = true;
            CHECK(n.velocity() <= 0.0f);
            CHECK(std::fabs(n.velocity()) <= 0.25f * std::fabs(prevV) + 1e-4f);
        }
        prevV = n.velocity();
    }
    CHECK(hit);
}

TEST_CASE("vu_needle_drive_of_reference_sine")
{
    const float amp = std::pow(10.0f, -20.0f / 20.0f);   // AES17: the −20 dBFS reference sine peaks at −20 dBFS
    const float rect = 2.0f * amp / std::numbers::pi_v<float>;
    CHECK_THAT(needleDrive(rect, -20.0f), WithinAbs(1.0, 0.01));
    CHECK_THAT(positionToVu(vuToPosition(-7.0f)), WithinAbs(-7.0, 1e-4));
    CHECK_THAT(vuToPosition(3.0f), WithinAbs(1.0, 1e-6));
}
