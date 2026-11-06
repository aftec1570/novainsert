// StateMachine (plan Task 17, F1 §13 + §15): allowed-transition table, last-known-good rule for Stale, bounded history,
// UnsupportedSampleRate from every state, allocation-free state() read.
#include <catch2/catch_test_macros.hpp>
#include <utility>
#include <vector>
#include "engine/state_machine.hpp"
#include "rt/alloc_guard.hpp"

using namespace ni::engine;

namespace
{
// Drive a fresh machine along a path of allowed transitions; every step must be accepted.
void walk(StateMachine& sm, const std::vector<State>& path)
{
    for (State s : path) REQUIRE(sm.apply(s, Reason::None, 1, 0));
}
} // namespace

TEST_CASE("state_machine_every_allowed_row_transitions")
{
    for (const auto& [from, to] : StateMachine::allowedTable())
    {
        StateMachine sm;
        sm.setKey(7);
        // Lock once (so Stale rows have a last-known-good), then reach `from` from Locked along allowed rows.
        walk(sm, { State::Queued, State::Measuring, State::ApplyingLatency, State::Locked });
        std::vector<State> path;
        switch (from)
        {
            case State::Idle: path = { State::Idle }; break;
            case State::Queued: path = { State::Queued }; break;
            case State::Measuring: path = { State::Queued, State::Measuring }; break;
            case State::ApplyingLatency: path = { State::Queued, State::Measuring, State::ApplyingLatency }; break;
            case State::Locked: break;
            case State::Stale: path = { State::Queued, State::Measuring, State::Stale }; break;
            case State::Failed: path = { State::Queued, State::Measuring, State::Failed }; break;
            case State::Unavailable: path = { State::Unavailable }; break;
            case State::UnsupportedHost: path = { State::UnsupportedHost }; break;
            case State::UnsupportedSampleRate: path = { State::UnsupportedSampleRate }; break;
        }
        walk(sm, path);
        INFO(static_cast<int>(from) << " -> " << static_cast<int>(to));
        REQUIRE(sm.state() == from);
        REQUIRE(sm.apply(to, Reason::None, 2, 10));
        REQUIRE(sm.state() == to);
    }
}

TEST_CASE("state_machine_idle_to_locked_rejected")
{
    StateMachine sm;
    REQUIRE_FALSE(sm.apply(State::Locked, Reason::None, 1, 0));
    REQUIRE(sm.state() == State::Idle);
    REQUIRE(sm.history().empty());
}

TEST_CASE("state_machine_locked_only_from_applying_latency")
{
    for (State from : { State::Queued, State::Measuring, State::Stale, State::Failed, State::Unavailable })
        for (const auto& row : StateMachine::allowedTable())
            if (row.first == from) REQUIRE(row.second != State::Locked);
}

TEST_CASE("state_machine_failed_to_stale_rejected")
{
    StateMachine sm;
    sm.setKey(1);
    walk(sm, { State::Queued, State::Measuring, State::ApplyingLatency, State::Locked, State::Queued, State::Measuring, State::Failed });
    REQUIRE(sm.lastKnownGoodValid());
    REQUIRE_FALSE(sm.apply(State::Stale, Reason::NoReturn, 3, 0));
    REQUIRE(sm.state() == State::Failed);
}

TEST_CASE("state_machine_applying_to_stale_needs_prior_lock_under_same_key")
{
    StateMachine fresh;
    fresh.setKey(1);
    walk(fresh, { State::Queued, State::Measuring, State::ApplyingLatency });
    REQUIRE_FALSE(fresh.lastKnownGoodValid());
    REQUIRE_FALSE(fresh.apply(State::Stale, Reason::PdcNotObserved, 1, 0));

    StateMachine same;
    same.setKey(1);
    walk(same, { State::Queued, State::Measuring, State::ApplyingLatency, State::Locked, State::Queued, State::Measuring,
                 State::ApplyingLatency });
    REQUIRE(same.apply(State::Stale, Reason::PdcNotObserved, 2, 0));

    StateMachine other;
    other.setKey(1);
    walk(other, { State::Queued, State::Measuring, State::ApplyingLatency, State::Locked, State::Queued, State::Measuring,
                  State::ApplyingLatency });
    other.setKey(2);   // routing/device key changed: the old value is not last-known-good for the new key
    REQUIRE_FALSE(other.lastKnownGoodValid());
    REQUIRE_FALSE(other.apply(State::Stale, Reason::PdcNotObserved, 2, 0));
}

TEST_CASE("state_machine_history_records_reason_generation_timestamp")
{
    StateMachine sm;
    REQUIRE(sm.apply(State::Queued, Reason::ProjectLoad, 4, 1000, "pos=1"));
    REQUIRE(sm.apply(State::Measuring, Reason::ProjectLoad, 4, 1500));
    REQUIRE(sm.history().size() == 2);
    const Transition& t = sm.history().front();
    REQUIRE(t.from == State::Idle);
    REQUIRE(t.to == State::Queued);
    REQUIRE(t.reason == Reason::ProjectLoad);
    REQUIRE(t.generation == 4);
    REQUIRE(t.timestampMs == 1000);
    REQUIRE(t.diag == "pos=1");
    REQUIRE(sm.history().back().timestampMs == 1500);
}

TEST_CASE("state_machine_history_bounded_to_256_keeps_newest")
{
    StateMachine sm;
    for (int i = 0; i < 300; ++i)
    {
        REQUIRE(sm.apply(State::Queued, Reason::None, static_cast<uint32_t>(i), i));
        REQUIRE(sm.apply(State::Idle, Reason::None, static_cast<uint32_t>(i), i));
    }
    REQUIRE(sm.history().size() == StateMachine::kHistoryMax);
    REQUIRE(StateMachine::kHistoryMax == 256);
    REQUIRE(sm.history().back().generation == 299);
    REQUIRE(sm.history().front().generation == 172);   // 600 transitions, newest 256 kept: index 344 = gen 172
}

TEST_CASE("state_machine_unsupported_sample_rate_reachable_from_every_state")
{
    for (State s : { State::Idle, State::Queued, State::Measuring, State::ApplyingLatency, State::Locked, State::Stale,
                     State::Failed, State::Unavailable, State::UnsupportedHost })
    {
        bool found = false;
        for (const auto& row : StateMachine::allowedTable())
            found = found || (row.first == s && row.second == State::UnsupportedSampleRate);
        INFO(static_cast<int>(s));
        REQUIRE(found);
    }
}

TEST_CASE("state_machine_self_transition_rejected")
{
    StateMachine sm;
    REQUIRE_FALSE(sm.apply(State::Idle, Reason::None, 1, 0));
    REQUIRE(sm.apply(State::Queued, Reason::None, 1, 0));
    REQUIRE_FALSE(sm.apply(State::Queued, Reason::None, 1, 0));
}

TEST_CASE("state_machine_flags_are_orthogonal")
{
    StateMachine sm;
    sm.setFlags({ true, false, true, true, false });
    const auto f = sm.flags();
    REQUIRE(f.bypass);
    REQUIRE_FALSE(f.disabled);
    REQUIRE(f.playing);
    REQUIRE(f.recording);
    REQUIRE_FALSE(f.rendering);
    REQUIRE(sm.state() == State::Idle);
}

TEST_CASE("state_machine_state_and_flags_reads_are_allocation_free")
{
    StateMachine sm;
    REQUIRE(sm.apply(State::Queued, Reason::None, 1, 0));
    AllocGuard g;
    REQUIRE_NO_ALLOC(sm.state());
    REQUIRE_NO_ALLOC(sm.flags());
    REQUIRE_NO_ALLOC(sm.setFlags({ false, false, true, false, false }));
}

// f3_4 Apollo record_abort (2026-11-01): the 500 ms host poll re-queued an instance aborted by recording as Idle(ProjectLoad)
// while the transport was still recording; the record path owns that re-queue (reason Recording, after the stop).
TEST_CASE("host_poll_requeue_leaves_a_recording_abort_to_the_record_path")
{
    REQUIRE(hostPollRequeue(State::Idle, Reason::None, Reason::None, true) == Reason::None);
    REQUIRE(hostPollRequeue(State::Idle, Reason::BufferChange, Reason::None, true) == Reason::None);
    REQUIRE(hostPollRequeue(State::Idle, Reason::None, Reason::None, false) == Reason::ProjectLoad);
    REQUIRE(hostPollRequeue(State::Idle, Reason::None, Reason::DeviceReturned, false) == Reason::DeviceReturned);
    REQUIRE(hostPollRequeue(State::Locked, Reason::DeviceReopen, Reason::None, false) == Reason::DeviceReopen);
    REQUIRE(hostPollRequeue(State::Locked, Reason::None, Reason::None, false) == Reason::None);
}

TEST_CASE("host_poll_requeue_ignores_a_stale_record_hold_once_the_instance_left_idle")
{
    // review S4 I1: a recording cut a probe (hold set), another path re-queued and locked the instance before the record path
    // cleared the hold; a later device change seen only by the poll must still re-measure.
    REQUIRE(hostPollRequeue(State::Locked, Reason::IoChange, Reason::None, true) == Reason::IoChange);
    REQUIRE(hostPollRequeue(State::Locked, Reason::None, Reason::None, true) == Reason::None);
}

TEST_CASE("host poll leaves an Idle instance alone while its first measurement waits for the preset defaults")
{
    // Apollo preset bench 2026-11-05: a fresh instance given its preset by parameter was Idle + owner for one tick before the
    // defaults tick requested the deferred measurement; a poll in that window re-queued it as ProjectLoad, so the first
    // measurement was scheduled twice (SCHED gen 8 and 9). The deferred request covers the poll's auto-recover duty.
    REQUIRE(hostPollRequeue(State::Idle, Reason::None, Reason::None, false, true) == Reason::None);
    REQUIRE(hostPollRequeue(State::Idle, Reason::BufferChange, Reason::None, false, true) == Reason::None);
    REQUIRE(hostPollRequeue(State::Idle, Reason::None, Reason::None, false, false) == Reason::ProjectLoad);
    REQUIRE(hostPollRequeue(State::Locked, Reason::IoChange, Reason::None, false, true) == Reason::IoChange);   // only an Idle wait is covered
}

TEST_CASE("device_cleared_state_drops_every_measurement_state_to_idle")
{
    // f3_5 ap_pdc_reset (Apollo, 2026-11-01): device -> none after a lock left the instance Locked with no loop.
    for (State s : { State::Queued, State::Measuring, State::ApplyingLatency, State::Locked, State::Stale, State::Failed })
        REQUIRE(deviceClearedState(s) == State::Idle);
    // host-blocked states describe the host, not the device: they stay until the host changes
    for (State s : { State::Idle, State::Unavailable, State::UnsupportedHost, State::UnsupportedSampleRate })
        REQUIRE(deviceClearedState(s) == s);
}
