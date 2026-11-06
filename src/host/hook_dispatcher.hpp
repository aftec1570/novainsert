// HookDispatcher — the single REAPER Audio_RegHardwareHook registration shared by every NovaInsert instance
// (F1 §1 Coordinator/HookDispatcher, §14 RT-safety, §22 row 1 dropout detector).
//   hook thread: onAudioBuffer() — no allocation, no locks, no waits. Fans out to client slots, measures the
//                inter-cycle span, detects buffer-size changes and null-buffer (device lost) cycles, and pushes
//                host events into a bounded ring.
//   message thread: install/uninstall, registerClient/unregisterClient, setExpectedBlock, drainEvents().
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include "engine/rings.hpp"
#include "engine/types.hpp"
#include "host/reaper_api.hpp"

namespace ni::host
{

class IHookClient
{
public:
    virtual ~IHookClient() = default;
    // Hook thread. `in`/`out` are REAPER's hardware channel buffers for this cycle (valid only during the call).
    virtual void onPre(int64_t cyclePos, int len, const ReaSample* const* in, int nIn) noexcept = 0;
    virtual void onPost(int64_t cyclePos, int len, ReaSample* const* out, int nOut) noexcept = 0;
};

class HookDispatcher
{
public:
    static constexpr int kMaxClients = engine::kMaxInstances;   // 20
    static constexpr int kMaxChannels = 128;                    // hardware channels gathered per cycle
    static constexpr int kNullCyclesToLost = 2;
    // ...and the null run must also have lasted this long: REAPER's own re-prepare at the end of an online render hands
    // the hook a few null cycles while the device keeps running (f3_6 rn_rpt), which is not a loss (Task 21 ruling).
    static constexpr uint64_t kLostMinNs = 500'000'000;
    static constexpr double kDropoutSpanFactor = 1.5;

    using ClockFn = uint64_t (*)() noexcept;                    // monotonic nanoseconds

    explicit HookDispatcher(ClockFn clock = nullptr);           // nullptr -> CLOCK_MONOTONIC_RAW
    ~HookDispatcher();
    HookDispatcher(const HookDispatcher&) = delete;
    HookDispatcher& operator=(const HookDispatcher&) = delete;

    bool install(ReaperApi& api);
    void uninstall();
    bool installed() const noexcept { return installed_; }

    int registerClient(IHookClient* c);                         // slot 0..19, or -1 when full
    void unregisterClient(int slot);                            // returns only after any in-flight cycle has finished

    struct Stats
    {
        uint64_t cycles = 0;
        int lastLen = 0;
        int nullBufferStreak = 0;
        bool deviceLost = false;                                // DeviceLost reported and not yet returned
        int lastNullRun = 0;                                    // length in cycles of the last finished null run
        int inputChannels = 0;                                  // reg_.input_nch as last seen by the hook
        int outputChannels = 0;                                 // reg_.output_nch as last seen by the hook
        int firstNullChannel = -1;                              // channel whose GetBuffer was null in the last pre cycle, -1 = none
        uint64_t dropouts = 0;
        double lastSpanUs = 0.0;
    };
    Stats stats() const noexcept;
    // Message thread: the hook's verdict, or a null run (>= kNullCyclesToLost cycles) older than kLostMinNs whose callbacks
    // stopped, so the hook never reached the mark itself (S6 review I2). Recovery: the next good cycle ends the run.
    bool deviceLostNow() const noexcept;
    void setExpectedBlock(int B);   // device BSIZE (DeviceKey.bufferSize): drives BufferChange reporting only
    // Host-side channel counts (GetNumAudioInputs/Outputs, message thread). REAPER 7.83 leaves the register's
    // input_nch/output_nch at 0 (F0.2), so these drive the cycle whenever the register says 0.
    void setChannelCounts(int inputs, int outputs);

    // Called on the message thread (drainEvents) for BufferChange / DeviceLost / DeviceReturned.
    std::function<void(engine::Reason)> onHostEvent;
    int drainEvents();                                          // message thread; returns the number dispatched

    static void onAudioBuffer(bool isPost, int len, double srate, audio_hook_register_t* reg);

private:
    void cycle(bool isPost, int len, double srate, audio_hook_register_t* reg) noexcept;
    void pushEvent(engine::Reason r) noexcept;

    ClockFn clock_;
    ReaperApi* api_ = nullptr;
    audio_hook_register_t reg_ {};
    bool installed_ = false;

    std::atomic<IHookClient*> clients_[kMaxClients] {};
    std::atomic<bool> inCycle_ { false };

    std::atomic<int> expectedBlock_ { 0 };
    int lastReportedLen_ = 0;                                   // hook thread only
    int64_t cyclePos_ = 0;                                      // hook thread only
    uint64_t lastPreNs_ = 0;                                    // hook thread only
    std::atomic<uint64_t> nullSinceNs_ { 0 };                   // written by the hook thread: pre time of the run's first null
    std::atomic<bool> lost_ { false };                          // written by the hook thread only
    std::atomic<int> lastNullRun_ { 0 };
    ReaSample* bufs_[kMaxChannels] {};                          // hook thread only

    std::atomic<uint64_t> cycles_ { 0 };
    std::atomic<int> lastLen_ { 0 };
    std::atomic<int> nullStreak_ { 0 };
    std::atomic<int> hostInputs_ { 0 };                         // setChannelCounts
    std::atomic<int> hostOutputs_ { 0 };
    std::atomic<int> inputCh_ { 0 };                            // effective counts of the last pre cycle
    std::atomic<int> outputCh_ { 0 };
    std::atomic<int> firstNullCh_ { -1 };
    std::atomic<uint64_t> dropouts_ { 0 };
    std::atomic<double> lastSpanUs_ { 0.0 };

    engine::Mpsc<engine::Reason, 256> events_;
    std::atomic<uint64_t> eventOverflow_ { 0 };
};

} // namespace ni::host
