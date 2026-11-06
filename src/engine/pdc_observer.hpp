// HostTransitionObserver (plan Task 18, F1 §9 Q9/Q10): follows one PDC change from setLatencySamples to acceptance.
// LatencyChangeRequested → HostTransitionObserved (ΔR > 0: ≥ ⌈ΔR/B⌉ extra blocks inside one hook cycle; ΔR < 0: a stall of
// ≥ 1 cycle or a playhead position step; begun while stopped: the first play segment) → StableLatencyAccepted (2 further
// cycles with every tag matching and contiguous positions). Window max(500 ms, 8 cycles) from the call while the transport
// runs, from the next play when stopped; TimedOut is sticky until the next begin.
// Thread contract: begin/onTransport/poll — message thread. onProcessBlock (processBlock) and onCycle (slot engine status,
// one call per hook cycle) — one RT thread, allocation- and lock-free; RT-only counters reset when the epoch changes.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include "engine/types.hpp"

namespace ni::engine
{
class HostTransitionObserver
{
public:
    enum class Phase : uint8_t { Idle, LatencyChangeRequested, HostTransitionObserved, StableLatencyAccepted, TimedOut };

    void begin (int32_t oldR, int32_t newR, int hwBlock, bool transportRunning, int64_t nowMs) noexcept
    {
        const int32_t d = newR - oldR;
        const int b = std::max (1, hwBlock);
        delta_.store (d, std::memory_order_relaxed);
        block_.store (b, std::memory_order_relaxed);
        need_.store (((d < 0 ? -d : d) + b - 1) / b, std::memory_order_relaxed);
        const int64_t cyclesMs = (8LL * b * 1000 + kSampleRate - 1) / kSampleRate;
        windowMs_ = std::max<int64_t> (500, cyclesMs);
        windowStart_ = transportRunning ? nowMs : -1;
        epoch_.fetch_add (1, std::memory_order_relaxed);
        // ΔR == 0 while running: the host has nothing to catch up, the call itself is the event (a confirmed provisional R).
        phase_.store (d == 0 && transportRunning ? Phase::HostTransitionObserved : Phase::LatencyChangeRequested, std::memory_order_release);
    }

    void onProcessBlock (int n, int64_t playPos, int64_t cycleIndex) noexcept
    {
        syncEpoch();
        const bool sameCycle = havePrev_ && cycleIndex == lastCycle_;
        const bool stalled = havePrev_ && cycleIndex > lastCycle_ + 1;
        const bool posStep = havePrev_ && playPos != lastPos_ + lastN_;
        samplesInCycle_ = sameCycle ? samplesInCycle_ + n : n;
        lastCycle_ = cycleIndex; lastPos_ = playPos; lastN_ = n; havePrev_ = true;

        if (phase_.load (std::memory_order_acquire) != Phase::LatencyChangeRequested) return;
        const int32_t d = delta_.load (std::memory_order_relaxed);
        const int b = block_.load (std::memory_order_relaxed);
        const bool seen = d > 0 ? samplesInCycle_ - b >= static_cast<int64_t> (need_.load (std::memory_order_relaxed)) * b
                                : d < 0 && (stalled || posStep);
        if (seen) advance (Phase::LatencyChangeRequested, Phase::HostTransitionObserved);
    }

    void onCycle (bool allTagsMatch, bool contiguous) noexcept
    {
        syncEpoch();
        if (phase_.load (std::memory_order_acquire) != Phase::HostTransitionObserved) return;
        stable_ = allTagsMatch && contiguous ? stable_ + 1 : 0;
        if (stable_ >= 2) advance (Phase::HostTransitionObserved, Phase::StableLatencyAccepted);
    }

    void onTransport (bool playing, int64_t nowMs) noexcept
    {
        if (! playing || windowStart_ >= 0) return;
        windowStart_ = nowMs;
        advance (Phase::LatencyChangeRequested, Phase::HostTransitionObserved);   // begun stopped: the play segment is the event
    }

    Phase poll (int64_t nowMs) noexcept
    {
        Phase p = phase_.load (std::memory_order_acquire);
        if ((p == Phase::LatencyChangeRequested || p == Phase::HostTransitionObserved) && windowStart_ >= 0
            && nowMs - windowStart_ > windowMs_)
            if (phase_.compare_exchange_strong (p, Phase::TimedOut, std::memory_order_acq_rel)) return Phase::TimedOut;
        return p;
    }

    int64_t windowMs() const noexcept { return windowMs_; }

private:
    void advance (Phase from, Phase to) noexcept { phase_.compare_exchange_strong (from, to, std::memory_order_acq_rel); }
    void syncEpoch() noexcept
    {
        const uint32_t e = epoch_.load (std::memory_order_acquire);
        if (e != seenEpoch_) { seenEpoch_ = e; stable_ = 0; }
    }

    std::atomic<Phase> phase_ { Phase::Idle };
    std::atomic<uint32_t> epoch_ { 0 };
    std::atomic<int32_t> delta_ { 0 }, need_ { 0 };
    std::atomic<int> block_ { 1 };
    int64_t windowMs_ = 500, windowStart_ = -1;   // message thread
    // RT thread only
    uint32_t seenEpoch_ = 0;
    int stable_ = 0;
    bool havePrev_ = false;
    int64_t lastCycle_ = 0, lastPos_ = 0, samplesInCycle_ = 0;
    int lastN_ = 0;
};
} // namespace ni::engine
