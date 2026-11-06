#include <catch2/catch_test_macros.hpp>
#include "engine/types.hpp"
#include "plugin/fallback.hpp"

// processBlock fallback when the instance did not process (no device, inactive, offline): Insert outputs silence, Effect
// passes the input through unchanged (F1 §5; review 2026-11-04 Important: the guard had no test).
TEST_CASE("fallback: Insert mode silences the block")
{
    float l[4] = { 1, 2, 3, 4 }, r[4] = { 5, 6, 7, 8 };
    float* io[2] = { l, r };
    ni::plugin::applyFallback(ni::engine::Mode::Insert, io, 2, 4);
    for (int i = 0; i < 4; ++i) { REQUIRE(l[i] == 0.0f); REQUIRE(r[i] == 0.0f); }
}

TEST_CASE("fallback: Effect mode passes the block through untouched")
{
    float l[4] = { 1, 2, 3, 4 }, r[4] = { 5, 6, 7, 8 };
    float* io[2] = { l, r };
    ni::plugin::applyFallback(ni::engine::Mode::Effect, io, 2, 4);
    for (int i = 0; i < 4; ++i) { REQUIRE(l[i] == (float) (i + 1)); REQUIRE(r[i] == (float) (i + 5)); }
}

TEST_CASE("fallback: mono Insert clears only the channel it has")
{
    float l[3] = { 1, 2, 3 };
    float* io[2] = { l, nullptr };
    ni::plugin::applyFallback(ni::engine::Mode::Insert, io, 1, 3);
    for (int i = 0; i < 3; ++i) REQUIRE(l[i] == 0.0f);
}
