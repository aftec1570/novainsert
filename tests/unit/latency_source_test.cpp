// Latency source per mode (Effect PDC spec 2026-11-04 §2): Insert probes; Effect never probes and takes the driver's own
// round-trip (GetInputOutputLatency in + out) as physLoop; R follows the same margin rule as a measured loop.
#include <catch2/catch_test_macros.hpp>
#include "plugin/latency_source.hpp"

using ni::engine::Mode;
using ni::engine::Reason;
using ni::plugin::latencySource;

TEST_CASE("latency_source_insert_probes", "[latency_source]")
{
    const auto s = latencySource(Mode::Insert, 100, 227, 64);
    CHECK(s.probe);
    CHECK(s.phys == -1);
}

TEST_CASE("latency_source_effect_uses_driver_round_trip_never_probes", "[latency_source]")
{
    const auto s = latencySource(Mode::Effect, 100, 227, 64);
    CHECK_FALSE(s.probe);
    CHECK(s.fail == Reason::None);
    CHECK(s.phys == 327);
}

TEST_CASE("latency_source_effect_zero_driver_round_trip_locks", "[latency_source]")
{
    const auto s = latencySource(Mode::Effect, 0, 0, 64);   // a driver reporting 0/0 still locks: R = 2B by the margin rule
    CHECK_FALSE(s.probe);
    CHECK(s.fail == Reason::None);
    CHECK(s.phys == 0);
}

TEST_CASE("latency_source_effect_negative_driver_value_fails_without_probe", "[latency_source]")
{
    const auto in = latencySource(Mode::Effect, -1, 200, 64);
    CHECK_FALSE(in.probe);
    CHECK(in.fail == Reason::OutOfRange);
    const auto out = latencySource(Mode::Effect, 200, -1, 64);
    CHECK_FALSE(out.probe);
    CHECK(out.fail == Reason::OutOfRange);
}

TEST_CASE("latency_source_effect_driver_out_of_range_fails", "[latency_source]")
{
    const int32_t edge = ni::engine::kLMax - 2 * 64;
    CHECK(latencySource(Mode::Effect, edge, 1, 64).fail == Reason::OutOfRange);
    CHECK(latencySource(Mode::Effect, edge, 0, 64).fail == Reason::None);
    CHECK(latencySource(Mode::Effect, edge, 0, 64).phys == edge);
}
