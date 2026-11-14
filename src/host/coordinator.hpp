// Coordinator – process-wide singleton shared by every NovaInsert instance (F1 §1, plan Task 6).
//   acquire(): first caller constructs it and installs the single hardware hook; the last shared_ptr release
//   uninstalls the hook and destroys it. One shared_ptr per instance; construction/destruction on the message
//   thread only; a std::mutex guards acquire (never called from RT).
//   The HookDispatcher itself is process-static and never destroyed (S1 review M8: a straggler hook callback
//   after Audio_RegHardwareHook(false) then reads a valid, idle dispatcher); it is installed/uninstalled here.
//   tick(nowMs): the message-thread pump (every instance's 50 ms timer calls it; cheap, idempotent): drains the
//   HookDispatcher host events to every listener, ticks the Scheduler, rechecks HostSettings every 500 ms.
//   RenderArbiter (Task 20/21): the one per-process render arbiter; preflight sees every registered instance view, tick()
//   drives its 250 ms dialog/project restore; refusal messages go to the diagnostics and the REAPER console.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include "host/hook_dispatcher.hpp"
#include "host/host_settings.hpp"
#include "host/pair_registry.hpp"
#include "host/render_arbiter.hpp"
#include "host/scheduler.hpp"
#include "host/reaper_api.hpp"

namespace ni::host
{

class Coordinator
{
public:
    static std::shared_ptr<Coordinator> acquire(const ReaperApi& api);
    ~Coordinator();

    HookDispatcher& hooks() noexcept;
    PairRegistry& pairs() noexcept { return pairs_; }   // message thread
    Scheduler& scheduler() noexcept { return scheduler_; }   // message thread; tick() from the Coordinator timer (Task 9)
    HostSettings& hostSettings() noexcept { return hostSettings_; }   // message thread
    const ReaperApi& api() const noexcept { return api_; }
    // Message-thread events delivered by tick(): host events (every listener), pair promotion and measurement start
    // (the named instance only), HostSettings violation (text = track name; the named instance only).
    struct Event { enum class Kind { Host, Promoted, MeasureStart, Violation, ViolationCleared } kind; engine::Reason reason; uint32_t generation; std::string text; };
    void addListener(int instanceId, std::function<void(const Event&)> onEvent);   // message thread
    void removeListener(int instanceId);
    void tick(int64_t nowMs);                    // message thread
    static constexpr int64_t kRecheckMs = 500;
    uint32_t nextGeneration() noexcept;          // strictly increasing, never 0 (0 = "never measured"); any thread
    int instanceCount() const;                   // live shared_ptr holders (= instances), message thread
    RenderArbiter& render() noexcept { return *arbiter_; }   // message thread
    // Message thread: an instance's current render view (state, mode, last processBlock stamp, track) for the preflight.
    void setRenderView(int instanceId, std::function<RenderArbiter::InstanceView()> view);
    void clearRenderView(int instanceId);
    static int64_t steadyMs() noexcept;          // the clock of Instance::lastProcessedMs and the arbiter
    static constexpr int64_t kRenderTickMs = 250;

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

private:
    explicit Coordinator(const ReaperApi& api);
    ReaperApi api_;
    PairRegistry pairs_;
    Scheduler scheduler_;
    HostSettings hostSettings_;
    std::vector<std::pair<int, std::function<void(const Event&)>>> listeners_;
    void refreshChannelCounts();   // GetNumAudioInputs/Outputs -> HookDispatcher::setChannelCounts
    void deliver(int instanceId, const Event& e);   // -1 = everyone
    int64_t lastRecheckMs_ = 0, lastRenderTickMs_ = 0;
    std::vector<std::pair<int, std::function<RenderArbiter::InstanceView()>>> renderViews_;
    std::unique_ptr<RenderArbiter> arbiter_;
    std::atomic<uint32_t> generation_ { 0 };
};

} // namespace ni::host
