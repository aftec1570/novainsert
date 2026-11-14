// StateMachine (plan Task 17, F1 §13 + §15): the instance state with an allowed-transition table, the last-known-good rule
// for Stale (a previous Locked under the same key) and a bounded transition history.
// Thread contract: apply/setKey/history/lastKnownGoodValid – message thread only. state()/flags()/setFlags() – any thread,
// lock- and allocation-free (atomics), so processBlock may publish the transport/render flags and read the state.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "engine/types.hpp"

namespace ni::engine
{
struct Transition
{
    State from, to;
    Reason reason;
    uint32_t generation;
    int64_t timestampMs;
    std::string diag;
};

class StateMachine
{
public:
    static constexpr std::size_t kHistoryMax = 256;
    using Row = std::pair<State, State>;

    struct Flags { bool bypass, disabled, playing, recording, rendering; };

    // F1 §13 + §15. Locked only from ApplyingLatency; Stale only from Measuring (failed re-measure) or ApplyingLatency
    // (PDC timeout, §9 Q10) and only with a valid last-known-good; Stale/Failed → ApplyingLatency is the one PDC retry at next play (Q10); Idle → ApplyingLatency is the
    // state-restore path (a matching snapshot is applied without a probe, plan Task 22);
    // Idle (release/deselect) and UnsupportedSampleRate (D-SR-1) are reachable from every state.
    static std::span<const Row> allowedTable() noexcept
    {
        using S = State;
        static constexpr Row rows[] = {
            { S::Idle, S::Queued }, { S::Idle, S::ApplyingLatency }, { S::Idle, S::Unavailable }, { S::Idle, S::UnsupportedHost }, { S::Idle, S::UnsupportedSampleRate },
            { S::Queued, S::Measuring }, { S::Queued, S::Idle }, { S::Queued, S::Unavailable }, { S::Queued, S::UnsupportedHost },
            { S::Queued, S::UnsupportedSampleRate },
            { S::Measuring, S::ApplyingLatency }, { S::Measuring, S::Queued }, { S::Measuring, S::Failed }, { S::Measuring, S::Stale },
            { S::Measuring, S::Idle }, { S::Measuring, S::Unavailable }, { S::Measuring, S::UnsupportedHost },
            { S::Measuring, S::UnsupportedSampleRate },
            { S::ApplyingLatency, S::Locked }, { S::ApplyingLatency, S::Stale }, { S::ApplyingLatency, S::Failed },
            { S::ApplyingLatency, S::Queued }, { S::ApplyingLatency, S::Idle }, { S::ApplyingLatency, S::Unavailable },
            { S::ApplyingLatency, S::UnsupportedHost }, { S::ApplyingLatency, S::UnsupportedSampleRate },
            { S::Locked, S::Queued }, { S::Locked, S::Idle }, { S::Locked, S::Unavailable }, { S::Locked, S::UnsupportedHost },
            { S::Locked, S::UnsupportedSampleRate },
            { S::Stale, S::Queued }, { S::Stale, S::ApplyingLatency }, { S::Stale, S::Idle }, { S::Stale, S::Unavailable },
            { S::Stale, S::UnsupportedHost }, { S::Stale, S::UnsupportedSampleRate },
            { S::Failed, S::Queued }, { S::Failed, S::ApplyingLatency }, { S::Failed, S::Idle }, { S::Failed, S::Unavailable }, { S::Failed, S::UnsupportedHost },
            { S::Failed, S::UnsupportedSampleRate },
            { S::Unavailable, S::Queued }, { S::Unavailable, S::Idle }, { S::Unavailable, S::UnsupportedHost },
            { S::Unavailable, S::UnsupportedSampleRate },
            { S::UnsupportedHost, S::Queued }, { S::UnsupportedHost, S::Idle }, { S::UnsupportedHost, S::Unavailable },
            { S::UnsupportedHost, S::UnsupportedSampleRate },
            { S::UnsupportedSampleRate, S::Idle }, { S::UnsupportedSampleRate, S::Queued }, { S::UnsupportedSampleRate, S::Unavailable },
        };
        return rows;
    }

    static bool allowed (State from, State to) noexcept
    {
        for (const Row& r : allowedTable())
            if (r.first == from && r.second == to) return true;
        return false;
    }

    State state() const noexcept { return state_.load (std::memory_order_acquire); }

    // Returns false and records nothing when (state, to) is not in the table or Stale lacks a last-known-good.
    bool apply (State to, Reason reason, uint32_t gen, int64_t nowMs, std::string diag = {})
    {
        const State from = state();
        if (! allowed (from, to)) return false;
        if (to == State::Stale && ! lastKnownGoodValid()) return false;
        if (to == State::Locked) { lkgKey_ = key_; lkgValid_ = true; }
        if (history_.size() == kHistoryMax) history_.erase (history_.begin());
        history_.push_back ({ from, to, reason, gen, nowMs, std::move (diag) });
        state_.store (to, std::memory_order_release);
        return true;
    }

    const std::vector<Transition>& history() const noexcept { return history_; }

    // The routing/device key the current value belongs to (F1 §8 calibration key); a new key voids last-known-good.
    void setKey (uint64_t key) noexcept { key_ = key; }
    bool lastKnownGoodValid() const noexcept { return lkgValid_ && lkgKey_ == key_; }

    void setFlags (Flags f) noexcept
    {
        flags_.store (static_cast<uint8_t> ((f.bypass ? 1 : 0) | (f.disabled ? 2 : 0) | (f.playing ? 4 : 0)
                                            | (f.recording ? 8 : 0) | (f.rendering ? 16 : 0)),
                      std::memory_order_release);
    }
    Flags flags() const noexcept
    {
        const uint8_t b = flags_.load (std::memory_order_acquire);
        return { (b & 1) != 0, (b & 2) != 0, (b & 4) != 0, (b & 8) != 0, (b & 16) != 0 };
    }

private:
    std::atomic<State> state_ { State::Idle };
    std::atomic<uint8_t> flags_ { 0 };
    std::vector<Transition> history_;
    uint64_t key_ = 0, lkgKey_ = 0;
    bool lkgValid_ = false;
};
// F1 §15 auto-recover + §7 record abort: the reason the host poll (evaluateHostState) queues a usable instance with, or None.
// An instance whose probe a recording cut (heldForRecording) is re-queued by the record path with Reason::Recording once the
// transport stops recording; the poll must not re-queue it earlier under another label (f3_4 Apollo record_abort, 2026-11-01).
// Once another path has re-queued the instance (it left Idle), a leftover hold must not mask a poll-only change such as IoChange.
// An Idle instance whose first measurement waits for the tick that applies its preset defaults (deferredFirst) is left alone:
// that request follows within one tick, and a poll request before it would schedule the same measurement twice (Apollo preset
// bench, 2026-11-05: SCHED gen 8 + 9 on a fresh instance given its preset by parameter).
inline Reason hostPollRequeue(State s, Reason pending, Reason trigger, bool heldForRecording, bool deferredFirst = false) noexcept
{
    if (s == State::Idle && (heldForRecording || deferredFirst)) return Reason::None;   // a hold / a deferral only means anything while still Idle (S4 review I1)
    if (pending != Reason::None) return pending;
    if (s == State::Idle) return trigger != Reason::None ? trigger : Reason::ProjectLoad;
    return Reason::None;
}

// Device parameter -> none (F1 §5): the loop is released, so every measurement state ends in Idle; the host-blocked states
// describe the host, not the device, and stay (f3_5 ap_pdc_reset: a lock survived the reset before this).
inline State deviceClearedState(State s) noexcept
{
    switch (s)
    {
        case State::Queued: case State::Measuring: case State::ApplyingLatency: case State::Locked: case State::Stale: case State::Failed:
            return State::Idle;
        case State::Idle: case State::Unavailable: case State::UnsupportedHost: case State::UnsupportedSampleRate:
            return s;
    }
    return s;
}

} // namespace ni::engine
