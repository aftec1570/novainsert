#include "host/hook_dispatcher.hpp"
#include <chrono>
#include <thread>

namespace ni::host
{
namespace
{
// Hook thread: steady_clock is a single clock_gettime (libc++) / QueryPerformanceCounter (MSVC) call — no allocation.
uint64_t monotonicNs() noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

HookDispatcher::HookDispatcher(ClockFn clock) : clock_(clock != nullptr ? clock : &monotonicNs) {}
HookDispatcher::~HookDispatcher() { uninstall(); }

bool HookDispatcher::install(ReaperApi& api)
{
    if (installed_) return true;
    if (api.Audio_RegHardwareHook == nullptr) return false;
    if (reg_.OnAudioBuffer == nullptr)   // set once: a straggler callback from a previous registration must never see reg_ mid-rewrite
    {
        reg_ = audio_hook_register_t {};
        reg_.OnAudioBuffer = &HookDispatcher::onAudioBuffer;
        reg_.userdata1 = this;
    }
    api_ = &api;
    installed_ = api.Audio_RegHardwareHook(true, &reg_) != 0;
    if (!installed_) api_ = nullptr;
    return installed_;
}

void HookDispatcher::uninstall()
{
    if (!installed_) return;
    api_->Audio_RegHardwareHook(false, &reg_);
    installed_ = false;
    api_ = nullptr;
    while (inCycle_.load(std::memory_order_acquire)) std::this_thread::yield();
}

int HookDispatcher::registerClient(IHookClient* c)
{
    if (c == nullptr) return -1;
    for (int i = 0; i < kMaxClients; ++i)
    {
        IHookClient* expected = nullptr;
        if (clients_[i].compare_exchange_strong(expected, c, std::memory_order_acq_rel)) return i;
    }
    return -1;
}

void HookDispatcher::unregisterClient(int slot)
{
    if (slot < 0 || slot >= kMaxClients) return;
    clients_[slot].store(nullptr, std::memory_order_seq_cst);
    // A cycle that loaded the pointer before the store may still be calling the client: wait for it to finish.
    while (inCycle_.load(std::memory_order_seq_cst)) std::this_thread::yield();
}

HookDispatcher::Stats HookDispatcher::stats() const noexcept
{
    Stats s;
    s.cycles = cycles_.load(std::memory_order_relaxed);
    s.lastLen = lastLen_.load(std::memory_order_relaxed);
    s.nullBufferStreak = nullStreak_.load(std::memory_order_relaxed);
    s.deviceLost = lost_.load(std::memory_order_relaxed);
    s.lastNullRun = lastNullRun_.load(std::memory_order_relaxed);
    s.inputChannels = inputCh_.load(std::memory_order_relaxed);
    s.outputChannels = outputCh_.load(std::memory_order_relaxed);
    s.firstNullChannel = firstNullCh_.load(std::memory_order_relaxed);
    s.nullInputs = nullIn_.load(std::memory_order_relaxed);
    s.nullOutputs = nullOut_.load(std::memory_order_relaxed);
    s.dropouts = dropouts_.load(std::memory_order_relaxed);
    s.lastSpanUs = lastSpanUs_.load(std::memory_order_relaxed);
    return s;
}

bool HookDispatcher::deviceLostNow() const noexcept
{
    if (lost_.load(std::memory_order_relaxed)) return true;
    // acquire pairs with the hook's release store of the streak, which follows its store of this run's nullSinceNs
    if (nullStreak_.load(std::memory_order_acquire) < kNullCyclesToLost) return false;
    const uint64_t since = nullSinceNs_.load(std::memory_order_relaxed);
    const uint64_t now = clock_();
    return since != 0 && now > since && now - since >= kLostMinNs;
}

bool HookDispatcher::channelDelivered(bool output, int ch) const noexcept
{
    const int d = output ? 1 : 0;
    if (ch < 0 || ch >= kMaxChannels || !deliveredSeen_[d].load(std::memory_order_acquire)) return true;
    return (delivered_[d][ch / 64].load(std::memory_order_relaxed) >> (ch % 64)) & 1u;
}

void HookDispatcher::resetDelivered() noexcept
{
    for (auto& s : deliveredSeen_) s.store(false, std::memory_order_release);
}

void HookDispatcher::setExpectedBlock(int B) { expectedBlock_.store(B, std::memory_order_relaxed); }

void HookDispatcher::setChannelCounts(int inputs, int outputs)
{
    hostInputs_.store(inputs < 0 ? 0 : inputs, std::memory_order_relaxed);
    hostOutputs_.store(outputs < 0 ? 0 : outputs, std::memory_order_relaxed);
}

int HookDispatcher::drainEvents()
{
    int n = 0;
    engine::Reason r {};
    while (events_.pop(r))
    {
        ++n;
        if (onHostEvent) onHostEvent(r);
    }
    return n;
}

void HookDispatcher::pushEvent(engine::Reason r) noexcept
{
    if (!events_.push(r)) eventOverflow_.fetch_add(1, std::memory_order_relaxed);
}

void HookDispatcher::onAudioBuffer(bool isPost, int len, double srate, audio_hook_register_t* reg)
{
    if (reg == nullptr || reg->userdata1 == nullptr) return;
    auto* self = static_cast<HookDispatcher*>(reg->userdata1);
    // Dekker handshake with unregisterClient(): this thread stores inCycle_ = true and then loads the client
    // slots; the message thread stores the slot = nullptr and then loads inCycle_. Both sides' store and load
    // must be seq_cst (a plain acquire load of the slot may be reordered before the seq_cst store on arm64,
    // letting unregisterClient() see inCycle_ == false while this cycle still holds the client pointer).
    self->inCycle_.store(true, std::memory_order_seq_cst);
    self->cycle(isPost, len, srate, reg);
    self->inCycle_.store(false, std::memory_order_seq_cst);
}

void HookDispatcher::cycle(bool isPost, int len, double srate, audio_hook_register_t* reg) noexcept
{
    // Register counts when the host fills them, otherwise the host-side counts (REAPER 7.83 leaves both at 0).
    const int nIn = reg->input_nch > 0 ? reg->input_nch : hostInputs_.load(std::memory_order_relaxed);
    const int nOut = reg->output_nch > 0 ? reg->output_nch : hostOutputs_.load(std::memory_order_relaxed);
    const int nch = isPost ? nOut : nIn;
    const int n = nch < 0 ? 0 : (nch > kMaxChannels ? kMaxChannels : nch);
    // A null cycle = REAPER handed us no usable buffers at all. A direction with zero channels (output-only or
    // input-only device) is a running device, not a loss: callbacks are delivered with n == 0 for it. Single null
    // channels are not a loss either: REAPER reports channels it does not deliver (inputs outside the enabled range,
    // forum report 2026-11-12); they reach the clients as nullptr (Instance reads silence / skips them) and the
    // instance whose own channel is missing says so (channelDelivered).
    bool null = reg->GetBuffer == nullptr || len <= 0 || (nIn <= 0 && nOut <= 0);
    int firstNull = -1, nullCount = 0;
    uint64_t mask[kMaxChannels / 64] {};
    for (int i = 0; i < n && !null; ++i)
    {
        bufs_[i] = reg->GetBuffer(isPost, i);
        if (bufs_[i] == nullptr) { ++nullCount; if (firstNull < 0) firstNull = i; }
        else mask[i / 64] |= uint64_t { 1 } << (i % 64);
    }
    if (!null && n > 0 && nullCount == n) null = true;   // every channel of this direction null: no usable buffers
    if (!isPost)
    {
        inputCh_.store(nIn, std::memory_order_relaxed);
        outputCh_.store(nOut, std::memory_order_relaxed);
        firstNullCh_.store(firstNull, std::memory_order_relaxed);
        nullIn_.store(nullCount, std::memory_order_relaxed);
    }
    else nullOut_.store(nullCount, std::memory_order_relaxed);
    if (!null)
    {
        const int d = isPost ? 1 : 0;
        for (int w = 0; w < kMaxChannels / 64; ++w) delivered_[d][w].store(mask[w], std::memory_order_relaxed);
        deliveredSeen_[d].store(true, std::memory_order_release);
    }

    if (!isPost)
    {
        // Span between consecutive pre callbacks vs the expected period (F1 §22 row 1).
        const uint64_t now = clock_();
        if (lastPreNs_ != 0 && now > lastPreNs_)
        {
            const double spanUs = static_cast<double>(now - lastPreNs_) * 1e-3;
            lastSpanUs_.store(spanUs, std::memory_order_relaxed);
            // Period from the previous hardware cycle length (what the device actually runs at), not from the
            // per-instance expected block, which only drives BufferChange reporting.
            const int prevLen = lastLen_.load(std::memory_order_relaxed);
            if (prevLen > 0 && srate > 0.0)
            {
                const double periodUs = 1e6 * static_cast<double>(prevLen) / srate;
                if (spanUs > kDropoutSpanFactor * periodUs) dropouts_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        lastPreNs_ = now;

        cycles_.fetch_add(1, std::memory_order_relaxed);
        lastLen_.store(len, std::memory_order_relaxed);

        if (null)
        {
            const int streak = nullStreak_.load(std::memory_order_relaxed) + 1;
            if (streak == 1) nullSinceNs_.store(now, std::memory_order_relaxed);
            nullStreak_.store(streak, std::memory_order_release);   // publishes nullSinceNs to deviceLostNow()
            const bool lost = lost_.load(std::memory_order_relaxed);
            if (!lost && streak >= kNullCyclesToLost && now - nullSinceNs_.load(std::memory_order_relaxed) >= kLostMinNs)
            {
                lost_.store(true, std::memory_order_relaxed);
                pushEvent(engine::Reason::DeviceLost);
            }
        }
        else
        {
            const int streak = nullStreak_.load(std::memory_order_relaxed);
            if (streak > 0) lastNullRun_.store(streak, std::memory_order_relaxed);
            nullStreak_.store(0, std::memory_order_relaxed);
            if (lost_.load(std::memory_order_relaxed)) { lost_.store(false, std::memory_order_relaxed); pushEvent(engine::Reason::DeviceReturned); }

            const int B = expectedBlock_.load(std::memory_order_relaxed);
            if (B > 0 && len != B && len != lastReportedLen_) { lastReportedLen_ = len; pushEvent(engine::Reason::BufferChange); }
            if (len == B) lastReportedLen_ = 0;
        }
    }

    if (null) return;

    if (!isPost)
    {
        for (auto& slot : clients_)
            if (IHookClient* c = slot.load(std::memory_order_seq_cst)) c->onPre(cyclePos_, len, bufs_, n);
    }
    else
    {
        for (auto& slot : clients_)
            if (IHookClient* c = slot.load(std::memory_order_seq_cst)) c->onPost(cyclePos_, len, bufs_, n);
        cyclePos_ += len;
    }
}

} // namespace ni::host
