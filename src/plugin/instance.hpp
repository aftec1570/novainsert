// Instance – the realtime side of one NovaInsert instance (plan Task 11): owns the SlotLoopEngine, is the HookDispatcher
// client for its hardware pair and feeds the engine from processBlock. JUCE-free so it is unit tested directly.
//   message thread: configure(pair, routing, B) / release() – pair ownership, routing, latency, hook registration.
//   processBlock:   process() – in place; false when inactive (the caller applies the Insert-silence / Effect-dry fallback).
//   hook thread:    onPre/onPost – pick the pair's channels out of REAPER's full channel arrays, convert double <-> float
//                   through preallocated scratch, write the pair's outputs by replacement (the pair is owned exclusively).
// configure() applies a provisional physLoop = 2B + loopExtra (R = physLoop + 2B rounded up to whole blocks, as REAPER primes
// PDC in whole blocks, F0.3; physLoop_origin=CONFIGURED) until applyResult() installs the Measurer's locked value (Insert) or
// the driver round-trip (Effect, 2026-11-04 spec).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include "engine/pdc_observer.hpp"
#include "engine/slot_loop.hpp"
#include "host/hook_dispatcher.hpp"
#include "host/pair_registry.hpp"

namespace ni::plugin
{

class Instance final : public host::IHookClient
{
public:
    // R = ceil((phys + 2B) / B) · B: whole blocks, so the host PDC aligns the returned slot (F0.4 §7).
    static int32_t reportedFor(int32_t phys, int B) noexcept { return (phys + 2 * B + B - 1) / B * B; }
    static constexpr int kFixedPhysBlocks = 2;       // provisional loop before the first lock: 2B (BlackHole, F0.4 §7)

    explicit Instance(host::HookDispatcher& hooks);  // message thread; sizes the engine for kLMax and the scratch for kBMax
    ~Instance() override;                            // unregisters from the hook
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // Message thread. Pair owned: channel map, routing (the widths follow the pair), fixed latency, hook registration.
    // False (and nothing changed) for a block outside 1..kBMax or a negative channel the pair needs.
    bool configure(const host::Pair& pair, const engine::Routing& routing, int hwBlock, int loopExtra = 0);   // loopExtra < 0 rejected
    void release();                                  // pair released / host unusable: unregister, process() returns false
    // Message thread (Task 16). The Measurer's locked physLoop replaces the provisional one: R = physLoop + 2B rounded up to
    // whole blocks, a new segment starts. False (nothing changed) while inactive or outside [0, L_max − 2B].
    bool applyMeasured(int32_t physLoop);
    bool active() const noexcept { return active_.load(std::memory_order_acquire); }
    int32_t physLoop() const noexcept { return physLoop_.load(std::memory_order_relaxed); }
    int32_t reportedLatency() const noexcept { return reported_.load(std::memory_order_relaxed); }
    uint64_t sendFrames() const noexcept { return sendFrames_.load(std::memory_order_relaxed); }   // frames handed to the loop while playing
    engine::SlotLoopEngine& engine() noexcept { return engine_; }
    // Task 19: the PDC transition observer this instance feeds from the RT side (process() while playing with the hook cycle
    // index; one onCycle per engine POST). begin/onTransport/poll stay on the message thread (PdcReporter).
    engine::HostTransitionObserver& pdcObserver() noexcept { return pdcObs_; }

    // processBlock thread. io[0..nch) in place (nch 1 or 2).
    bool process(float* const* io, int nch, int n, int64_t playPos, bool playing) noexcept;
    // Task 21: steady-clock ms of the last process() call (any thread; -1 = never) – RenderArbiter "involved" (F1 §10).
    int64_t lastProcessedMs() const noexcept { return lastProcessedMs_.load(std::memory_order_relaxed); }
    static int64_t steadyMs() noexcept;   // same clock as host::Coordinator::steadyMs

    // hook thread (host::IHookClient)
    void onPre(int64_t cyclePos, int len, const ReaSample* const* in, int nIn) noexcept override;
    void onPost(int64_t cyclePos, int len, ReaSample* const* out, int nOut) noexcept override;

private:
    struct Map { int16_t out[2], in[2]; bool sendStereo, retStereo; };
    static uint64_t pack(const Map& m) noexcept;
    static Map unpack(uint64_t v) noexcept;

    host::HookDispatcher& hooks_;
    engine::SlotLoopEngine engine_;
    int slot_ = -1;                                  // hook client slot, message thread
    int hwBlock_ = 0;                                // configured block, message thread
    int32_t segmentGen_ = 0;                         // processBlock thread
    std::atomic<int32_t> configGen_ { 0 };           // bumped by configure(): a new configuration starts a new segment
    std::atomic<bool> active_ { false };
    std::atomic<bool> preSeen_ { false };              // a PRE reached the engine since activation (fresh preBase_)
    std::atomic<uint64_t> map_ { 0 };
    std::atomic<int32_t> physLoop_ { 0 }, reported_ { 0 };
    std::atomic<uint64_t> sendFrames_ { 0 };
    std::unique_ptr<float[]> inScratch_, outScratch_;   // 2 × kBMax each, hook thread only
    engine::HostTransitionObserver pdcObs_;
    alignas(64) std::atomic<int64_t> lastProcessedMs_ { -1 };   // written every processBlock: off the line the hook thread reads (active_/map_)
    std::atomic<int64_t> hookCycles_ { 0 };          // PRE count: the cycle index of processBlock calls (observer)
    int32_t lastStable_ = 0;                         // hook thread: engine stableCycles at the previous POST
};

} // namespace ni::plugin
