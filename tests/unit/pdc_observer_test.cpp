// HostTransitionObserver (plan Task 18, F1 §9 Q9/Q10): catch-up event seen in processBlock (burst on increase, stall or
// position step on decrease, first play segment when stopped), then 2 stable hardware cycles; window max(500 ms, 8 cycles).
#include <catch2/catch_test_macros.hpp>
#include "engine/pdc_observer.hpp"
#include "rt/alloc_guard.hpp"

using namespace ni::engine;
using Phase = HostTransitionObserver::Phase;

namespace
{
// One normal hardware cycle: one processBlock of B samples continuing at pos.
void cycle(HostTransitionObserver& o, int B, int64_t& pos, int64_t& cyc)
{
    o.onProcessBlock(B, pos, cyc);
    pos += B;
    ++cyc;
}
} // namespace

TEST_CASE("observer_increase_detected_by_burst_then_two_stable_cycles")
{
    HostTransitionObserver o;
    const int B = 256;
    int64_t pos = 0, cyc = 0;
    for (int i = 0; i < 4; ++i) cycle(o, B, pos, cyc);
    o.begin(1024, 1024 + 600, B, true, 0);   // ΔR = 600 → ⌈600/256⌉ = 3 extra calls
    REQUIRE(o.poll(1) == Phase::LatencyChangeRequested);
    cycle(o, B, pos, cyc);
    o.onCycle(true, true);
    REQUIRE(o.poll(2) == Phase::LatencyChangeRequested);   // nothing observed yet: a stable cycle before the event is ignored
    for (int k = 0; k < 4; ++k) { o.onProcessBlock(B, pos, cyc); pos += B; }   // 1 normal + 3 extra calls in one cycle
    ++cyc;
    REQUIRE(o.poll(3) == Phase::HostTransitionObserved);
    o.onCycle(true, true);
    REQUIRE(o.poll(4) == Phase::HostTransitionObserved);
    o.onCycle(false, true);   // a mismatching cycle restarts the count
    o.onCycle(true, true);
    REQUIRE(o.poll(5) == Phase::HostTransitionObserved);
    o.onCycle(true, true);
    REQUIRE(o.poll(6) == Phase::StableLatencyAccepted);
}

TEST_CASE("observer_increase_with_too_few_extra_calls_is_not_a_burst")
{
    HostTransitionObserver o;
    const int B = 256;
    int64_t pos = 0, cyc = 0;
    cycle(o, B, pos, cyc);
    o.begin(1024, 1024 + 600, B, true, 0);
    for (int k = 0; k < 3; ++k) { o.onProcessBlock(B, pos, cyc); pos += B; }   // only 2 extra
    ++cyc;
    REQUIRE(o.poll(1) == Phase::LatencyChangeRequested);
}

TEST_CASE("observer_decrease_detected_by_stall")
{
    HostTransitionObserver o;
    const int B = 128;
    int64_t pos = 0, cyc = 0;
    for (int i = 0; i < 3; ++i) cycle(o, B, pos, cyc);
    o.begin(2048, 1024, B, true, 0);
    ++cyc;   // one hook cycle without a processBlock call
    o.onProcessBlock(B, pos, cyc);
    REQUIRE(o.poll(1) == Phase::HostTransitionObserved);
}

TEST_CASE("observer_decrease_detected_by_position_step")
{
    HostTransitionObserver o;
    const int B = 128;
    int64_t pos = 1000, cyc = 0;
    for (int i = 0; i < 3; ++i) cycle(o, B, pos, cyc);
    o.begin(2048, 1024, B, true, 0);
    o.onProcessBlock(B, pos - 1024, cyc);   // playhead rewound by ΔR
    REQUIRE(o.poll(1) == Phase::HostTransitionObserved);
}

TEST_CASE("observer_stopped_transport_window_starts_at_next_play")
{
    HostTransitionObserver o;
    o.begin(0, 512, 256, false, 0);
    REQUIRE(o.poll(100000) == Phase::LatencyChangeRequested);   // no window while stopped
    o.onTransport(true, 200000);   // the first play segment after the call is the transition
    REQUIRE(o.poll(200000) == Phase::HostTransitionObserved);
    REQUIRE(o.poll(200499) == Phase::HostTransitionObserved);
    REQUIRE(o.poll(200501) == Phase::TimedOut);
}

TEST_CASE("observer_window_is_max_500ms_or_8_cycles")
{
    HostTransitionObserver o;
    o.begin(0, 4096, 2048, true, 0);
    REQUIRE(o.windowMs() == 500);   // 8 × 42.7 ms = 341 ms
    o.begin(0, 8192, 4096, true, 0);
    REQUIRE(o.windowMs() == 683);   // 8 × 85.3 ms, rounded up
    REQUIRE(o.poll(683) == Phase::LatencyChangeRequested);
    REQUIRE(o.poll(684) == Phase::TimedOut);
}

TEST_CASE("observer_timeout_is_sticky_until_a_new_begin")
{
    HostTransitionObserver o;
    const int B = 256;
    int64_t pos = 0, cyc = 0;
    cycle(o, B, pos, cyc);
    o.begin(0, 256, B, true, 0);
    REQUIRE(o.poll(501) == Phase::TimedOut);
    for (int k = 0; k < 2; ++k) { o.onProcessBlock(B, pos, cyc); pos += B; }   // late burst
    ++cyc;
    o.onCycle(true, true);
    o.onCycle(true, true);
    o.onCycle(true, true);
    REQUIRE(o.poll(600) == Phase::TimedOut);
    o.begin(256, 512, B, false, 700);
    REQUIRE(o.poll(701) == Phase::LatencyChangeRequested);
}

TEST_CASE("observer_unchanged_r_while_running_needs_only_two_stable_cycles")
{
    // A measurement that confirms the provisional R: the host has no transition to make, so the call is the event
    // (otherwise the window would always expire into Failed(PdcNotObserved) for a correct latency).
    HostTransitionObserver o;
    const int B = 64;
    int64_t pos = 0, cyc = 0;
    for (int i = 0; i < 3; ++i) cycle(o, B, pos, cyc);
    o.begin(512, 512, B, true, 0);
    REQUIRE(o.poll(1) == Phase::HostTransitionObserved);
    o.onCycle(true, true);
    o.onCycle(true, true);
    REQUIRE(o.poll(2) == Phase::StableLatencyAccepted);
}

TEST_CASE("observer_rt_calls_are_allocation_free")
{
    HostTransitionObserver o;
    o.begin(0, 256, 256, true, 0);
    AllocGuard g;
    REQUIRE_NO_ALLOC(o.onProcessBlock(256, 0, 0));
    REQUIRE_NO_ALLOC(o.onCycle(true, true));
    REQUIRE_NO_ALLOC(o.poll(1));
}
