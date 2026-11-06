#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include "host/hook_dispatcher.hpp"
#include "rt/alloc_guard.hpp"

using namespace ni::host;
using ni::engine::Reason;

namespace
{
constexpr int kCh = 4;
ReaSample g_in[kCh][2048];
ReaSample g_out[kCh][2048];
bool g_nullBuffers = false;
ReaSample* fakeGetBuffer(bool isOutput, int idx)
{
    if (g_nullBuffers || idx < 0 || idx >= kCh) return nullptr;
    return isOutput ? g_out[idx] : g_in[idx];
}
uint64_t g_nowNs = 0;
uint64_t fakeClock() noexcept { return g_nowNs; }

// Drives the real static trampoline exactly as REAPER would: pre then post, same len/srate.
struct Harness
{
    HookDispatcher d { &fakeClock };
    audio_hook_register_t reg {};
    std::vector<Reason> events;
    Harness()
    {
        reg.OnAudioBuffer = &HookDispatcher::onAudioBuffer;
        reg.userdata1 = &d;
        reg.input_nch = kCh; reg.output_nch = kCh;
        reg.GetBuffer = &fakeGetBuffer;
        d.onHostEvent = [this](Reason r) { events.push_back(r); };
        g_nullBuffers = false; g_nowNs = 0;
    }
    void feed(int len, double srate = 48000.0)
    {
        HookDispatcher::onAudioBuffer(false, len, srate, &reg);
        HookDispatcher::onAudioBuffer(true, len, srate, &reg);
        d.drainEvents();
    }
};

struct RecordingClient : IHookClient
{
    std::vector<int64_t> prePos, postPos;
    int lastNIn = 0, lastNOut = 0, lastLen = 0;
    const ReaSample* lastIn0 = nullptr;
    void onPre(int64_t cyclePos, int len, const ReaSample* const* in, int nIn) noexcept override
    { prePos.push_back(cyclePos); lastLen = len; lastNIn = nIn; lastIn0 = in[0]; }
    void onPost(int64_t cyclePos, int len, ReaSample* const* out, int nOut) noexcept override
    { postPos.push_back(cyclePos); lastLen = len; lastNOut = nOut; (void) out; }
};
} // namespace

TEST_CASE("pre/post fan out to registered clients in slot order with running cycle position")
{
    Harness h; RecordingClient a, b;
    REQUIRE(h.d.registerClient(&a) == 0);
    REQUIRE(h.d.registerClient(&b) == 1);
    h.d.setExpectedBlock(64);
    h.feed(64); h.feed(64);
    REQUIRE(a.prePos == std::vector<int64_t> { 0, 64 });
    REQUIRE(a.postPos == std::vector<int64_t> { 0, 64 });
    REQUIRE(b.prePos == std::vector<int64_t> { 0, 64 });
    REQUIRE(a.lastNIn == kCh);
    REQUIRE(a.lastNOut == kCh);
    REQUIRE(a.lastIn0 == g_in[0]);
    REQUIRE(h.d.stats().cycles == 2);
    REQUIRE(h.d.stats().lastLen == 64);
    h.d.unregisterClient(0);
    h.feed(64);
    REQUIRE(a.prePos.size() == 2);
    REQUIRE(b.prePos.size() == 3);
}

TEST_CASE("registerClient returns -1 when all 20 slots are taken")
{
    Harness h; RecordingClient c[21];
    for (int i = 0; i < 20; ++i) REQUIRE(h.d.registerClient(&c[i]) == i);
    REQUIRE(h.d.registerClient(&c[20]) == -1);
    h.d.unregisterClient(7);
    REQUIRE(h.d.registerClient(&c[20]) == 7);
}

TEST_CASE("len != expected block raises BufferChange once per change")
{
    Harness h; h.d.setExpectedBlock(64);
    h.feed(64); h.feed(64); h.feed(64);
    REQUIRE(h.events.empty());
    h.feed(128);
    REQUIRE(h.events == std::vector<Reason> { Reason::BufferChange });
    for (int i = 0; i < 5; ++i) h.feed(128);
    REQUIRE(h.events.size() == 1);
    h.d.setExpectedBlock(128);
    h.feed(128);
    REQUIRE(h.events.size() == 1);
    h.feed(64);
    REQUIRE(h.events.size() == 2);
}

TEST_CASE("null buffers for 2+ cycles and 500 ms -> DeviceLost, next good cycle -> DeviceReturned")
{
    Harness h; RecordingClient a; h.d.registerClient(&a); h.d.setExpectedBlock(64);
    const auto step = [&] { g_nowNs += 1'333'333; h.feed(64); };
    g_nowNs = 1'000'000; h.feed(64);
    g_nullBuffers = true;
    step();
    REQUIRE(h.events.empty());
    REQUIRE(h.d.stats().nullBufferStreak == 1);
    step();
    REQUIRE(h.events.empty());                     // 2 cycles but only 1.3 ms: not yet a loss
    REQUIRE_FALSE(h.d.stats().deviceLost);
    g_nowNs += HookDispatcher::kLostMinNs; h.feed(64);
    REQUIRE(h.events == std::vector<Reason> { Reason::DeviceLost });
    REQUIRE(h.d.stats().deviceLost);
    step();
    REQUIRE(h.events.size() == 1);                 // reported once
    REQUIRE(a.prePos.size() == 1);                 // clients are not called on null cycles
    g_nullBuffers = false;
    step();
    REQUIRE(h.events == std::vector<Reason> { Reason::DeviceLost, Reason::DeviceReturned });
    REQUIRE(h.d.stats().nullBufferStreak == 0);
    REQUIRE_FALSE(h.d.stats().deviceLost);
    REQUIRE(a.prePos.size() == 2);
}

// REAPER re-prepares at the end of every online render (PREPARE nonrt=1 -> nonrt=0, ~0.1 s) and hands the hook a few
// null-buffer cycles while the device keeps running (f3_6 Apollo rn_rpt, 2026-11-01): not a device loss, no re-measure.
TEST_CASE("a null-buffer blip shorter than 500 ms is not a device loss")
{
    Harness h; RecordingClient a; h.d.registerClient(&a); h.d.setExpectedBlock(64);
    g_nowNs = 1'000'000; h.feed(64);
    g_nullBuffers = true;
    for (int i = 0; i < 20; ++i) { g_nowNs += 1'333'333; h.feed(64); }
    g_nowNs += 100'000'000; h.feed(64);            // a 100 ms callback gap inside the blip, still < 500 ms since the first null
    REQUIRE(h.d.stats().nullBufferStreak == 21);
    g_nullBuffers = false;
    g_nowNs += 1'333'333; h.feed(64);
    REQUIRE(h.events.empty());
    REQUIRE(h.d.stats().lastNullRun == 21);        // kept for the HOOKSTAT diagnostics
    REQUIRE(a.prePos.size() == 2);                 // still no client call on the null cycles
}

TEST_CASE("null buffers at a 2048 block are lost once 500 ms have passed, not after 2 cycles")
{
    Harness h; h.d.setExpectedBlock(2048);         // period 42.7 ms
    g_nowNs = 1'000'000; h.feed(2048);
    g_nullBuffers = true;
    for (int i = 0; i < 11; ++i) { g_nowNs += 42'666'667; h.feed(2048); }   // 0..426 ms after the first null
    REQUIRE(h.events.empty());
    for (int i = 0; i < 2; ++i) { g_nowNs += 42'666'667; h.feed(2048); }    // 512 ms
    REQUIRE(h.events == std::vector<Reason> { Reason::DeviceLost });
}

TEST_CASE("span > 1.5 period counts a dropout")
{
    Harness h; h.d.setExpectedBlock(64);           // period = 64/48000 s = 1333.3 us
    g_nowNs = 1'000'000; h.feed(64);
    g_nowNs += 1'333'333; h.feed(64);               // exactly one period: fine
    REQUIRE(h.d.stats().dropouts == 0);
    g_nowNs += 1'900'000; h.feed(64);               // 1.42 periods: fine
    REQUIRE(h.d.stats().dropouts == 0);
    g_nowNs += 2'100'000; h.feed(64);               // 1.57 periods: dropout
    REQUIRE(h.d.stats().dropouts == 1);
    REQUIRE(h.d.stats().lastSpanUs > 2099.0);
    REQUIRE(h.d.stats().lastSpanUs < 2101.0);
}

TEST_CASE("hook path allocates nothing")
{
    Harness h; RecordingClient a; a.prePos.reserve(64); a.postPos.reserve(64);
    h.d.registerClient(&a); h.d.setExpectedBlock(64);
    h.feed(64);
    AllocGuard g;
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(false, 64, 48000.0, &h.reg));
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(true, 64, 48000.0, &h.reg));
    g_nullBuffers = true;
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(false, 64, 48000.0, &h.reg));
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(true, 64, 48000.0, &h.reg));
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(false, 64, 48000.0, &h.reg));   // 2nd null -> event push
    REQUIRE_NO_ALLOC(HookDispatcher::onAudioBuffer(true, 128, 48000.0, &h.reg));   // len change -> event push
}

TEST_CASE("unregister during cycles never dereferences a freed client")
{
    struct Counting : IHookClient
    {
        std::atomic<int>* calls;
        explicit Counting(std::atomic<int>* c) : calls(c) {}
        void onPre(int64_t, int, const ReaSample* const*, int) noexcept override { calls->fetch_add(1); }
        void onPost(int64_t, int, ReaSample* const*, int) noexcept override { calls->fetch_add(1); }
    };
    Harness h; h.d.setExpectedBlock(64);
    std::atomic<bool> stop { false };
    std::atomic<int> calls { 0 };
    std::thread hook([&] { while (!stop.load()) { HookDispatcher::onAudioBuffer(false, 64, 48000.0, &h.reg); HookDispatcher::onAudioBuffer(true, 64, 48000.0, &h.reg); } });
    for (int i = 0; i < 10000; ++i)
    {
        auto* c = new Counting(&calls);
        const int slot = h.d.registerClient(c);
        REQUIRE(slot >= 0);
        h.d.unregisterClient(slot);
        delete c;                                   // ASan/TSan: must be safe right after unregister returns
    }
    stop.store(true); hook.join();
    REQUIRE(calls.load() >= 0);
}

// S1 review I2: an output-only device (0 input channels) is a running device, not a device loss.
TEST_CASE("output-only device (0 inputs) is not a device loss and still delivers pre/post")
{
    Harness h; RecordingClient c; h.d.registerClient(&c);
    h.reg.input_nch = 0;
    for (int i = 0; i < 4; ++i) h.feed(64);
    REQUIRE(h.events.empty());
    REQUIRE(h.d.stats().nullBufferStreak == 0);
    REQUIRE(c.prePos.size() == 4);
    REQUIRE(c.lastNIn == 0);
    REQUIRE(c.postPos.size() == 4);
    REQUIRE(c.lastNOut == kCh);
}

// S1 review I3: the dropout period follows the hardware cycle length, not a per-instance expected block.
TEST_CASE("dropout period follows the actual cycle length even when the expected block is wrong")
{
    Harness h; h.d.setExpectedBlock(2048);           // a stale/oversized expectation must not disable detection
    g_nowNs = 1'000'000; h.feed(64);                 // period = 64/48000 s = 1333.3 us
    g_nowNs += 1'333'333; h.feed(64);
    REQUIRE(h.d.stats().dropouts == 0);
    g_nowNs += 2'100'000; h.feed(64);                // 1.57 periods: dropout
    REQUIRE(h.d.stats().dropouts == 1);
}

namespace
{
int g_nullIdx = -1;   // one channel index whose buffer is null (both directions)
ReaSample* fakeGetBufferOneNull(bool isOutput, int idx)
{
    if (idx == g_nullIdx) return nullptr;
    return fakeGetBuffer(isOutput, idx);
}
}

TEST_CASE("stats report the host's channel counts and the first channel whose buffer was null")
{
    Harness h; h.reg.GetBuffer = &fakeGetBufferOneNull; g_nullIdx = -1;
    h.feed(64);
    REQUIRE(h.d.stats().inputChannels == kCh);
    REQUIRE(h.d.stats().outputChannels == kCh);
    REQUIRE(h.d.stats().firstNullChannel == -1);
    g_nullIdx = 2;
    h.feed(64);
    REQUIRE(h.d.stats().firstNullChannel == 2);
    REQUIRE(h.d.stats().nullBufferStreak == 1);
    g_nullIdx = -1;
    h.feed(64);
    REQUIRE(h.d.stats().firstNullChannel == -1);
}

// REAPER 7.83 never fills audio_hook_register_t::input_nch/output_nch (F0.2: 0 in 504/504 cycles while
// GetNumAudioInputs/Outputs() == 64/64). The host-side channel counts drive the cycle; a zeroed register is not a loss.
TEST_CASE("zeroed register channel counts fall back to the host-side counts and are not a device loss")
{
    Harness h; RecordingClient c; h.d.registerClient(&c);
    h.reg.input_nch = 0; h.reg.output_nch = 0;
    h.d.setChannelCounts(kCh, kCh);
    for (int i = 0; i < 4; ++i) h.feed(64);
    REQUIRE(h.events.empty());
    REQUIRE(h.d.stats().nullBufferStreak == 0);
    REQUIRE(h.d.stats().inputChannels == kCh);
    REQUIRE(h.d.stats().outputChannels == kCh);
    REQUIRE(c.prePos.size() == 4);
    REQUIRE(c.lastNIn == kCh);
    REQUIRE(c.lastNOut == kCh);
    REQUIRE(c.lastIn0 == g_in[0]);
}

// S6 review I2: REAPER hands a few null cycles and then stops calling the hook while the device key still reads open — the
// hook never sees the 500 ms mark, so the message-thread poll ages the run itself.
TEST_CASE("a null run whose callbacks stop is a loss for the poll once 500 ms have passed")
{
    Harness h; h.d.setExpectedBlock(64);
    g_nowNs = 1'000'000; h.feed(64);
    g_nullBuffers = true;
    for (int i = 0; i < 3; ++i) { g_nowNs += 1'333'333; h.feed(64); }
    REQUIRE_FALSE(h.d.deviceLostNow());
    g_nowNs += 400'000'000;
    REQUIRE_FALSE(h.d.deviceLostNow());          // 404 ms since the first null
    g_nowNs += 200'000'000;
    REQUIRE(h.d.deviceLostNow());                // no callback since, 604 ms
    REQUIRE(h.events.empty());                   // the hook itself reported nothing
    g_nullBuffers = false;
    g_nowNs += 1'333'333; h.feed(64);
    REQUIRE_FALSE(h.d.deviceLostNow());
}

TEST_CASE("one null cycle followed by silence is not a loss")
{
    Harness h; h.d.setExpectedBlock(64);
    g_nowNs = 1'000'000; h.feed(64);
    g_nullBuffers = true;
    g_nowNs += 1'333'333; h.feed(64);
    g_nowNs += 2'000'000'000;
    REQUIRE_FALSE(h.d.deviceLostNow());          // a stopped stream after one null is a stop (DeviceReopen polling), not a run
}
