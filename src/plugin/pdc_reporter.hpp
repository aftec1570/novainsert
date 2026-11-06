// PdcReporter (plan Task 18, F1 §9): reports a measured R to the host once per request and drives ApplyingLatency → Locked
// through the HostTransitionObserver. Deferred to stop/pause while the transport runs (DD default) unless the caller opts
// into live re-lock; timeout → Stale (last-known-good, same key) or Failed(PdcNotObserved), R kept; one automatic retry of
// the transition on the next play after a stop. Message thread only.
#pragma once
#include <cstdint>
#include <functional>
#include "engine/pdc_observer.hpp"
#include "engine/state_machine.hpp"

namespace ni::plugin
{
class PdcReporter
{
public:
    using SetLatency = std::function<void (int32_t)>;   // juce::AudioProcessor::setLatencySamples in the product
    // State sink (Task 19): the processor keeps its own state; the engine StateMachine is the reference sink of the unit tests.
    using ApplyState = std::function<bool (ni::engine::State, ni::engine::Reason, uint32_t gen, int64_t nowMs, const char* diag)>;
    using LastKnownGood = std::function<bool()>;        // a previous valid lock for the current calibration key

    PdcReporter (SetLatency set, ni::engine::HostTransitionObserver& obs, ApplyState apply, LastKnownGood lkg)
        : set_ (std::move (set)), obs_ (obs), applyState_ (std::move (apply)), lkg_ (std::move (lkg)) {}
    PdcReporter (SetLatency set, ni::engine::HostTransitionObserver& obs, ni::engine::StateMachine& sm)
        : PdcReporter (std::move (set), obs,
                       [&sm] (ni::engine::State s, ni::engine::Reason r, uint32_t g, int64_t t, const char* d) { return sm.apply (s, r, g, t, d); },
                       [&sm] { return sm.lastKnownGoodValid(); }) {}

    // `why`: the reason ApplyingLatency carries (None for a measured result, StateRestore for a project snapshot, Task 22).
    void request (int32_t R, bool deferToStop, uint32_t gen, int hwBlock, int64_t nowMs, ni::engine::Reason why = ni::engine::Reason::None);
    // The product's call: `playingNow` is the transport read at the call (the timer's onTransport may lag a result, S5 I1).
    void request (int32_t R, bool deferToStop, bool playingNow, uint32_t gen, int hwBlock, int64_t nowMs,
                  ni::engine::Reason why = ni::engine::Reason::None);
    void onTransport (bool playing, int64_t nowMs);
    void tick (int64_t nowMs);
    void cancel() noexcept;                       // a new request / released loop: drop the pending or awaited transition
    void noteReported (int32_t R) noexcept { reported_ = R; }   // latency the processor set itself (provisional, release → 0)
    int32_t reported() const noexcept { return reported_; }
    bool pending() const noexcept { return pendingR_ >= 0; }
    // F1 §9 Q10 output mute after PdcNotObserved: held through the pending retry, lifted on Locked, cancel/new request, or at
    // the first stop once no retry is left (S5 I2: never a permanent mute).
    bool muted() const noexcept { return muted_; }

private:
    void apply (int32_t R, int64_t nowMs);

    SetLatency set_;
    ni::engine::HostTransitionObserver& obs_;
    ApplyState applyState_;
    LastKnownGood lkg_;
    int32_t reported_ = 0, pendingR_ = -1;
    uint32_t gen_ = 0;
    int block_ = 0;
    bool playing_ = false, awaiting_ = false, timedOut_ = false, retryLeft_ = false, stoppedAfterTimeout_ = false, muted_ = false;
};
} // namespace ni::plugin
