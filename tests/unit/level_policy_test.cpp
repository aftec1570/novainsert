// LevelPolicy (plan Task 14, F1 §8 "Levels"): start level −36 / −24 dBFS, next level P = min(−12 − sendTrim, T_ret − G_peak,
// current + 12) with T_ret = −12, predicted return ≤ H_ret = −6; no raise on clip, no lock, return < −60, overload, unstable gen.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include "engine/level_policy.hpp"

using namespace ni::engine;
using Catch::Matchers::WithinAbs;

namespace
{
ProbeObs ok(float gainPeakDb, float returnPeakDb = -20.0f)
{
    return { returnPeakDb, gainPeakDb, false, true, false, true };
}
} // namespace

TEST_CASE("level_start_unknown_path_is_minus36")
{
    REQUIRE(LevelPolicy::startLevelDb({ false, -20.0f, 3.0f }) == -36.0f);
}

TEST_CASE("level_start_known_key_with_gain_is_minus24")
{
    REQUIRE(LevelPolicy::startLevelDb({ true, -6.0f, 15.6f }) == -24.0f);
    REQUIRE(LevelPolicy::startLevelDb({ true, -5.0f, 15.6f }) == -36.0f);
    REQUIRE(LevelPolicy::startLevelDb({ true, -20.0f, std::numeric_limits<float>::quiet_NaN() }) == -36.0f);
}

TEST_CASE("level_next_uses_gain_peak_bluey")
{
    const auto n = LevelPolicy::nextLevelDb(-36.0f, ok(15.6f, -20.4f), 0.0f);
    REQUIRE(n.has_value());
    REQUIRE_THAT(*n, WithinAbs(-27.6, 1e-4));
    REQUIRE_THAT(*n + 15.6f, WithinAbs(-12.0, 1e-4));   // predicted return peak = T_ret
}

TEST_CASE("level_next_step_capped_at_plus12")
{
    const auto n = LevelPolicy::nextLevelDb(-36.0f, ok(-30.0f, -66.0f + 6.0f), 0.0f);
    REQUIRE(n.has_value());
    REQUIRE(*n == -24.0f);
}

TEST_CASE("level_next_send_ceiling_includes_send_trim")
{
    const auto n = LevelPolicy::nextLevelDb(-24.0f, ok(-30.0f, -54.0f), 6.0f);
    REQUIRE(n.has_value());
    REQUIRE(*n == -18.0f);
    const auto m = LevelPolicy::nextLevelDb(-24.0f, ok(-30.0f, -54.0f), 0.0f);
    REQUIRE(*m == -12.0f);
}

TEST_CASE("level_no_raise_conditions")
{
    auto o = ok(0.0f);
    o.clipped = true;
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(0.0f, -0.05f);   // return sample-peak ≥ −0.1 dBFS counts as clipping
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(0.0f); o.lockPresent = false;
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(0.0f, -61.0f);
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(0.0f); o.overload = true;
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(0.0f); o.genStable = false;
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
    o = ok(std::numeric_limits<float>::quiet_NaN());
    REQUIRE_FALSE(LevelPolicy::nextLevelDb(-36.0f, o, 0.0f));
}

TEST_CASE("level_predicted_return_never_exceeds_ceiling")
{
    for (float g = -40.0f; g <= 30.0f; g += 0.7f)
        for (float cur : { -36.0f, -24.0f, -18.0f })
            for (float trim : { -6.0f, 0.0f, 6.0f })
                if (const auto n = LevelPolicy::nextLevelDb(cur, ok(g, cur + g), trim))
                {
                    CAPTURE(g, cur, trim, *n);
                    REQUIRE(*n + g <= -6.0f);
                    REQUIRE(*n + trim <= -12.0f + 1e-4f);
                    REQUIRE(*n <= cur + 12.0f);
                }
}
