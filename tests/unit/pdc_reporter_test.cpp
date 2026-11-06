// PdcReporter (plan Task 18, F1 §9): setLatencySamples once per request on the message thread, deferred to stop while the
// transport runs; Locked only after the observer accepts; timeout → Stale (prior lock, same key) or Failed(PdcNotObserved),
// R kept, one automatic retry of the transition on the next play; R > L_max → Failed(OutOfRange) without a host call.
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "plugin/pdc_reporter.hpp"

using namespace ni::engine;
using ni::plugin::PdcReporter;

namespace
{
struct Rig
{
    std::vector<int32_t> calls;
    HostTransitionObserver obs;
    StateMachine sm;
    PdcReporter rep { [this](int32_t r) { calls.push_back(r); }, obs, sm };

    void toMeasuring() { sm.setKey(1); REQUIRE(sm.apply(State::Queued, Reason::None, 1, 0)); REQUIRE(sm.apply(State::Measuring, Reason::None, 1, 0)); }
    void lockVia(int32_t R, int64_t t)   // apply while stopped, then play: first segment observed + 2 stable cycles
    {
        rep.request(R, true, 1, 256, t);
        rep.onTransport(true, t + 1);
        obs.onCycle(true, true);
        obs.onCycle(true, true);
        rep.tick(t + 2);
        REQUIRE(sm.state() == State::Locked);
        rep.onTransport(false, t + 3);
    }
};
} // namespace

TEST_CASE("reporter_defers_while_playing_and_applies_at_stop")
{
    Rig g;
    g.toMeasuring();
    g.rep.onTransport(true, 0);
    g.rep.request(1024, true, 1, 256, 10);
    REQUIRE(g.calls.empty());
    REQUIRE(g.sm.state() == State::ApplyingLatency);
    g.rep.tick(5000);   // no window while the change is only pending
    REQUIRE(g.sm.state() == State::ApplyingLatency);
    g.rep.onTransport(false, 6000);
    REQUIRE(g.calls == std::vector<int32_t> { 1024 });
    REQUIRE(g.rep.reported() == 1024);
}

TEST_CASE("reporter_applies_immediately_when_stopped_and_locks_after_acceptance")
{
    Rig g;
    g.toMeasuring();
    g.lockVia(512, 0);
    REQUIRE(g.calls == std::vector<int32_t> { 512 });
}

TEST_CASE("reporter_timeout_without_prior_lock_is_failed_pdc_not_observed_with_one_retry")
{
    Rig g;
    g.toMeasuring();
    g.rep.onTransport(true, 0);
    g.rep.request(768, false, 1, 256, 0);   // live re-lock opted in
    REQUIRE(g.calls == std::vector<int32_t> { 768 });
    g.rep.tick(501);
    REQUIRE(g.sm.state() == State::Failed);
    REQUIRE(g.sm.history().back().reason == Reason::PdcNotObserved);
    REQUIRE(g.rep.reported() == 768);   // R kept, never flipped back
    g.rep.onTransport(false, 600);
    g.rep.onTransport(true, 700);   // the one automatic retry: the next play segment is the transition
    REQUIRE(g.sm.state() == State::ApplyingLatency);
    g.obs.onCycle(true, true);
    g.obs.onCycle(true, true);
    g.rep.tick(701);
    REQUIRE(g.sm.state() == State::Locked);
    REQUIRE(g.calls.size() == 1);   // never a second setLatencySamples for one request
}

TEST_CASE("reporter_timeout_with_prior_lock_is_stale_and_retries_only_once")
{
    Rig g;
    g.toMeasuring();
    g.lockVia(512, 0);
    REQUIRE(g.sm.apply(State::Queued, Reason::RoutingChange, 2, 10));
    REQUIRE(g.sm.apply(State::Measuring, Reason::RoutingChange, 2, 10));
    g.rep.onTransport(true, 20);
    g.rep.request(768, false, 2, 256, 20);
    g.rep.tick(521);
    REQUIRE(g.sm.state() == State::Stale);
    REQUIRE(g.rep.reported() == 768);
    g.rep.onTransport(false, 600);
    g.rep.onTransport(true, 700);
    REQUIRE(g.sm.state() == State::ApplyingLatency);
    g.rep.tick(1201);   // the retry times out too
    REQUIRE(g.sm.state() == State::Stale);
    g.rep.onTransport(false, 1300);
    g.rep.onTransport(true, 1400);
    REQUIRE(g.sm.state() == State::Stale);   // no second retry
    REQUIRE(g.calls == std::vector<int32_t> { 512, 768 });
}

TEST_CASE("reporter_caps_at_lmax_failed_out_of_range")
{
    Rig g;
    g.toMeasuring();
    g.rep.request(kLMax + 1, true, 1, 256, 0);
    REQUIRE(g.calls.empty());
    REQUIRE(g.sm.state() == State::Failed);
    REQUIRE(g.sm.history().back().reason == Reason::OutOfRange);
}

// Task 19 (processor wiring): the processor keeps its own state, so the reporter also drives a plain state sink; a new
// measurement request or a released loop cancels an outstanding transition; latency set outside the reporter (provisional
// configure, release → 0) is noted as the base of the next transition.
TEST_CASE("reporter_drives_a_function_state_sink")
{
    struct Call { State s; Reason r; };
    std::vector<Call> applied;
    std::vector<int32_t> calls;
    bool lkg = false;
    HostTransitionObserver obs;
    PdcReporter rep { [&](int32_t r) { calls.push_back(r); }, obs,
                      [&](State s, Reason r, uint32_t, int64_t, const char*) { applied.push_back({ s, r }); return true; },
                      [&] { return lkg; } };
    rep.request(512, true, 1, 256, 0);
    REQUIRE(calls == std::vector<int32_t> { 512 });
    REQUIRE(applied.size() == 1);
    REQUIRE(applied[0].s == State::ApplyingLatency);
    rep.onTransport(true, 1);
    obs.onCycle(true, true); obs.onCycle(true, true);
    rep.tick(2);
    REQUIRE(applied.back().s == State::Locked);
    rep.onTransport(false, 3);
    lkg = true;
    rep.request(768, true, 2, 256, 10);
    rep.onTransport(true, 11);
    rep.tick(11 + 600);   // no stable cycles inside the 500 ms window
    REQUIRE(applied.back().s == State::Stale);
    REQUIRE(applied.back().r == Reason::PdcNotObserved);
}

TEST_CASE("reporter_cancel_drops_a_pending_change_and_never_locks")
{
    Rig g;
    g.toMeasuring();
    g.rep.onTransport(true, 0);
    g.rep.request(1024, true, 1, 256, 10);   // deferred: playing
    g.rep.cancel();
    REQUIRE_FALSE(g.rep.pending());
    g.rep.onTransport(false, 100);
    REQUIRE(g.calls.empty());
    // an applied transition that is cancelled before acceptance does not lock later
    g.rep.request(512, true, 1, 256, 200);   // stopped: applied now
    g.rep.cancel();
    g.rep.onTransport(true, 201);
    g.obs.onCycle(true, true); g.obs.onCycle(true, true);
    g.rep.tick(202);
    REQUIRE(g.sm.state() == State::ApplyingLatency);
}

TEST_CASE("reporter_note_reported_is_the_base_of_the_next_transition")
{
    Rig g;
    g.toMeasuring();
    g.rep.noteReported(448);                 // provisional R set by the processor at configure
    REQUIRE(g.rep.reported() == 448);
    g.rep.onTransport(true, 0);
    g.rep.request(480, false, 1, 32, 10);    // live re-lock: ΔR = +32 at B = 32 needs one extra block in a cycle
    g.obs.onProcessBlock(32, 0, 1);
    g.obs.onProcessBlock(32, 32, 1);         // a second call inside the same hook cycle: the catch-up
    g.obs.onCycle(true, true); g.obs.onCycle(true, true);
    g.rep.tick(20);
    REQUIRE(g.sm.state() == State::Locked);  // with base 0 (ΔR = 480) two blocks would not count as the catch-up
}

// S5 review I1: the processor learns the transport on its timer, after a measurement result may already have arrived; the
// request carries the transport state read at the call so a play edge inside that tick still defers the change.
TEST_CASE("reporter_request_with_current_transport_defers_without_a_prior_transport_edge")
{
    Rig g;
    g.toMeasuring();
    g.rep.request(1024, true, true, 1, 256, 10);   // playing now; onTransport never called
    REQUIRE(g.calls.empty());
    REQUIRE(g.rep.pending());
    g.rep.onTransport(false, 20);
    REQUIRE(g.calls == std::vector<int32_t> { 1024 });
}

// S5 review I2: the PdcNotObserved mute lasts until the transport stops; it holds through a pending retry and is lifted at the
// first stop once no retry is left (never a permanent mute), on Locked, and on cancel.
TEST_CASE("reporter_mute_holds_through_the_retry_and_lifts_at_stop_when_none_is_left")
{
    Rig g;
    g.toMeasuring();
    g.rep.onTransport(true, 0);
    g.rep.request(768, false, 1, 256, 0);
    REQUIRE_FALSE(g.rep.muted());
    g.rep.tick(501);
    REQUIRE(g.rep.muted());
    g.rep.onTransport(false, 600);
    REQUIRE(g.rep.muted());           // the retry is still to come
    g.rep.onTransport(true, 700);
    REQUIRE(g.rep.muted());
    g.rep.tick(1201);                 // the retry times out too
    REQUIRE(g.sm.state() == State::Failed);
    REQUIRE(g.rep.muted());           // muted until the user stops
    g.rep.onTransport(false, 1300);
    REQUIRE_FALSE(g.rep.muted());     // no retry left: never muted for good
}

TEST_CASE("reporter_mute_lifts_on_lock_and_on_cancel")
{
    Rig g;
    g.toMeasuring();
    g.rep.onTransport(true, 0);
    g.rep.request(768, false, 1, 256, 0);
    g.rep.tick(501);
    REQUIRE(g.rep.muted());
    g.rep.onTransport(false, 600);
    g.rep.onTransport(true, 700);
    g.obs.onCycle(true, true);
    g.obs.onCycle(true, true);
    g.rep.tick(701);
    REQUIRE(g.sm.state() == State::Locked);
    REQUIRE_FALSE(g.rep.muted());

    Rig h;
    h.toMeasuring();
    h.rep.onTransport(true, 0);
    h.rep.request(768, false, 1, 256, 0);
    h.rep.tick(501);
    REQUIRE(h.rep.muted());
    h.rep.cancel();
    REQUIRE_FALSE(h.rep.muted());
}

TEST_CASE("reporter_request_carries_the_callers_reason_into_applying_latency")
{
    // Task 22: a project reopened with a matching calibration snapshot reports R without a probe — ApplyingLatency(StateRestore)
    std::vector<Reason> seen;
    HostTransitionObserver obs;
    PdcReporter rep ([] (int32_t) {}, obs,
                     [&] (State s, Reason r, uint32_t, int64_t, const char*) { if (s == State::ApplyingLatency) seen.push_back (r); return true; },
                     [] { return false; });
    rep.request (1024, true, false, 7, 256, 0, Reason::StateRestore);
    REQUIRE (seen == std::vector<Reason> { Reason::StateRestore });
    rep.request (512, true, false, 8, 256, 10);
    REQUIRE (seen == std::vector<Reason> { Reason::StateRestore, Reason::None });
}
