// RT-safety audit (plan Task 26 Step 1, F1 §14): one simulated session driven through the real RT entry points —
// HookDispatcher::onAudioBuffer (the static trampoline REAPER calls, fanning out to Instance::onPre/onPost) and
// Instance::process + MeterEngine::feed + SlotLoopEngine::cutProbe (processBlock) — with message-thread actions between
// cycles: prepare, idle cycles, play, a probe measurement, the measured lock (PDC change), a seek, bypass toggles (no
// processBlock calls), trim/selector changes, a record cut and a stop. Allocations are counted ONLY inside the RT calls;
// the message-thread actions run under the same live guard but outside the counted regions. 10 000 device cycles at B = 64.
// LIMITATION (alloc_guard.hpp): malloc/calloc/realloc are not intercepted; the inventory in tests/rt/rt_audit.md covers them.
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "engine/meters.hpp"
#include "host/hook_dispatcher.hpp"
#include "plugin/instance.hpp"
#include "rt/alloc_guard.hpp"
#include "unit/host_sim.hpp"

using namespace ni::engine;
using ni::host::HookDispatcher;
using ni::host::Pair;
using ni::plugin::Instance;
using ni::test::content;

namespace
{
constexpr int kDevCh = 16, kB = 64, kCycles = 10000;
constexpr int64_t kSlots = static_cast<int64_t>(kCycles + 8) * kB;
constexpr int32_t kCable = 2 * kB + 346;                    // the real analog loop: what the measurement must find
constexpr Pair kPair { { 4, 5 }, { 6, 7 }, true, true };

ReaSample g_in[kDevCh][kB];
ReaSample g_out[kDevCh][kB];
ReaSample* devBuffer(bool isOutput, int idx) { return idx >= 0 && idx < kDevCh ? (isOutput ? g_out[idx] : g_in[idx]) : nullptr; }
uint64_t g_ns = 0;
uint64_t clockNs() noexcept { return g_ns; }

struct Session
{
    HookDispatcher hooks { &clockNs };
    Instance inst { hooks };
    MeterEngine sendMeter, returnMeter;
    audio_hook_register_t reg {};
    std::vector<double> wire[2];
    std::vector<float> io[2], outL, outR;
    int64_t cycle = 0, label = 0;
    bool playing = false, segStart = false, bypassed = false, cutThisCycle = false;
    std::size_t rtAllocs = 0;

    Session()
    {
        reg.OnAudioBuffer = &HookDispatcher::onAudioBuffer;
        reg.userdata1 = &hooks;
        reg.GetBuffer = &devBuffer;   // input_nch/output_nch stay 0 as on REAPER 7.83: the host-side counts drive the cycle
        hooks.setChannelCounts(kDevCh, kDevCh);
        hooks.setExpectedBlock(kB);
        for (auto& w : wire) w.assign(static_cast<size_t>(kSlots), 0.0);
        for (auto& b : io) b.assign(static_cast<size_t>(4 * kLMax + 2 * kB), 0.0f);
        outL.assign(static_cast<size_t>(kSlots), 0.0f); outR.assign(static_cast<size_t>(kSlots), 0.0f);
        sendMeter.prepare(48000); returnMeter.prepare(48000);
    }

    void play(int64_t P) { playing = true; segStart = true; label = P; }

    void step() noexcept
    {
        const int64_t base = cycle * kB;
        for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < kB; ++k) g_in[c][k] = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kB; ++k)
            {
                const int64_t src = base + k - kCable;
                g_in[kPair.in[c]][k] = src >= 0 ? wire[c][static_cast<size_t>(src)] : 0.0;
            }
        g_ns += static_cast<uint64_t>(kB) * 1000000000ull / 48000ull;
        std::size_t before = AllocGuard::count();
        HookDispatcher::onAudioBuffer(false, kB, 48000.0, &reg);
        rtAllocs += AllocGuard::count() - before;

        if (! bypassed)
        {
            float* p[2] { io[0].data(), io[1].data() };
            const int32_t R = inst.reportedLatency();
            const int m = playing ? static_cast<int>(segStart ? (R + kB - 1) / kB * kB + kB : kB) : kB;
            for (int k = 0; k < m; ++k)
            {
                io[0][static_cast<size_t>(k)] = playing ? content(label + k, 0) : 0.25f;
                io[1][static_cast<size_t>(k)] = playing ? content(label + k, 1) : -0.25f;
            }
            before = AllocGuard::count();
            if (cutThisCycle) inst.engine().cutProbe();
            sendMeter.feed(p[0], p[1], m);
            const bool ran = inst.process(p, 2, m, playing ? label : 0, playing);
            if (ran) returnMeter.feed(p[0], p[1], m);
            rtAllocs += AllocGuard::count() - before;
            cutThisCycle = false;
            if (playing)
            {
                for (int k = 0; k < m; ++k)
                {
                    const int64_t q = label + k - R;
                    if (q >= 0 && q < kSlots) { outL[static_cast<size_t>(q)] = p[0][k]; outR[static_cast<size_t>(q)] = p[1][k]; }
                }
                label += m; segStart = false;
            }
        }

        for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < kB; ++k) g_out[c][k] = 0.0;
        before = AllocGuard::count();
        HookDispatcher::onAudioBuffer(true, kB, 48000.0, &reg);
        rtAllocs += AllocGuard::count() - before;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kB; ++k) wire[c][static_cast<size_t>(base + k)] = g_out[kPair.out[c]][k];
        ++cycle;
    }
    void run(int n) noexcept { for (int i = 0; i < n; ++i) step(); }
};
} // namespace

TEST_CASE("rt_audit_alloc_counter_sees_an_allocation")
{
    // The audit's counted regions use the same counter: prove it is live, so a 0 below is not vacuous.
    AllocGuard g;
    const std::size_t before = AllocGuard::count();
    static int* volatile sink = nullptr;   // volatile: the optimiser may not elide the new/delete pair (C++14 [expr.new]/10)
    sink = new int(7);
    REQUIRE(AllocGuard::count() == before + 1);
    delete sink;
}

TEST_CASE("rt_audit_full_session_allocates_nothing_on_rt_entry_points")
{
    auto s = std::make_unique<Session>();
    REQUIRE(s->inst.configure(kPair, Routing { true, true, 0.0f, 0.0f }, kB));
    std::vector<float> probe(4096), capL(static_cast<size_t>(SlotLoopEngine::kCaptureCap)), capR(capL.size());
    for (size_t i = 0; i < probe.size(); ++i) probe[i] = (i % 7 == 0) ? 0.5f : -0.125f;

    AllocGuard g;
    s->run(200);                                                       // idle, stopped: processBlock with playing = false
    s->play(0);
    s->run(800);                                                       // provisional latency (2B loop assumed, 2B + 346 real)

    // measurement: probe armed from the message thread, captured on the hook, then disarmed
    s->inst.engine().setMuted(true);
    s->inst.engine().setProbe(probe.data(), static_cast<int32_t>(probe.size()));
    int guardCycles = 0;
    while (s->inst.engine().capturedCount() < static_cast<int32_t>(probe.size()) + 2 * kCable && guardCycles++ < 2000) s->run(1);
    REQUIRE(s->inst.engine().captureReturn(capL.data(), capR.data(), static_cast<int32_t>(probe.size()) + 2 * kCable));
    s->inst.engine().setProbe(nullptr, 0);
    s->inst.engine().setMuted(false);

    REQUIRE(s->inst.applyMeasured(kCable));                            // PDC change: R = ceil((physLoop + 2B) / B) · B
    s->play(s->label); s->run(1500);                                   // host re-primes at the new R

    for (int t = 0; t < 6; ++t)                                        // bypass toggles: hook continues, processBlock absent
    {
        s->bypassed = (t % 2) == 0;
        s->run(150);
    }
    s->bypassed = false;
    for (int t = 0; t < 8; ++t)                                        // parameter changes: trims and selectors (atomics)
    {
        s->inst.engine().setRouting(Routing { true, t % 2 == 0, -0.5f * static_cast<float>(t), 0.25f * static_cast<float>(t) });
        s->run(100);
    }
    s->inst.engine().setRouting(Routing { true, true, 0.0f, 0.0f });

    // record start during a measurement: processBlock cuts the probe
    s->inst.engine().setProbe(probe.data(), static_cast<int32_t>(probe.size()));
    s->run(10);
    s->cutThisCycle = true;
    s->run(50);
    s->inst.engine().setProbe(nullptr, 0);

    s->playing = false; s->run(300);                                   // stop
    const int64_t P = 100000;
    s->play(P);                                                        // seek: new segment at a far position
    const int left = kCycles - static_cast<int>(s->cycle);
    REQUIRE(left > 2000);
    s->run(left);

    REQUIRE(s->cycle == kCycles);
    REQUIRE(s->rtAllocs == 0);

    // the session really exercised the paths: hook delivered every cycle, no dropout, loop exact after the seek
    const auto hs = s->hooks.stats();
    REQUIRE(hs.cycles >= static_cast<uint64_t>(kCycles));
    REQUIRE(hs.dropouts == 0);
    REQUIRE(s->inst.physLoop() == kCable);
    int64_t bad = 0;
    for (int64_t q = P + kCable; q < P + 60000; ++q)
        bad += (s->outL[static_cast<size_t>(q)] != content(q, 0)) + (s->outR[static_cast<size_t>(q)] != content(q, 1));
    REQUIRE(bad == 0);
    const auto rd = s->returnMeter.read(-20.0f);
    REQUIRE(rd.peakDb[0] > -60.0f);
}
