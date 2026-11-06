// Instance (plan Task 11): the RT wrapper around SlotLoopEngine — hook channel selection out of REAPER's full hardware channel
// arrays, double <-> float conversion, temporary fixed latency (physLoop = 2B, R = 4B), hook client registration.
// A 32-channel simulated device with an ideal cable from the pair's outputs to its inputs (cross-channel, so a wrong
// selection or a channel swap cannot pass); host PDC model as in host_sim.hpp (burst R + B at the segment start).
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "plugin/instance.hpp"
#include "rt/alloc_guard.hpp"
#include "unit/host_sim.hpp"

using namespace ni::engine;
using ni::host::Pair;
using ni::plugin::Instance;
using ni::test::content;

namespace
{
uint64_t testClock() noexcept { return 0; }
constexpr int kDevCh = 32;
constexpr double kUntouchedOut = 7.0, kForeignIn = 99.0;

struct Rig
{
    Rig(Instance& i, int B, int hostCh, Pair cable, int64_t maxPos) : inst(i), B(B), hostCh(hostCh), cable(cable)
    {
        for (auto& c : in) c.assign(static_cast<size_t>(B), 0.0);
        for (auto& c : out) c.assign(static_cast<size_t>(B), 0.0);
        for (auto& w : wire) w.assign(static_cast<size_t>(maxPos + 8 * B), 0.0);
        for (auto& b : io) b.assign(static_cast<size_t>(4 * kBMax + kBMax + 4096), 0.0f);
        outL.assign(static_cast<size_t>(maxPos), -1.0f); outR.assign(static_cast<size_t>(maxPos), -1.0f);
        for (int c = 0; c < kDevCh; ++c) { inPtr[c] = in[c].data(); outPtr[c] = out[c].data(); }
    }
    Instance& inst; int B, hostCh; Pair cable;
    std::vector<double> in[kDevCh], out[kDevCh], wire[2];
    const double* inPtr[kDevCh] {}; double* outPtr[kDevCh] {};
    std::vector<float> io[2], outL, outR;
    int64_t cycle = 0, label = 0; bool segStart = true, playing = true;
    uint64_t foreignWrites = 0;   // hardware outputs other than the pair's that changed
    void (*afterPre)(Instance&) = nullptr;   // runs once between this cycle's PRE and processBlock (message-thread race)

    void step() noexcept
    {
        const int64_t base = cycle * B; const int32_t phys = inst.physLoop(), R = inst.reportedLatency();
        for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) in[c][static_cast<size_t>(k)] = kForeignIn;
        for (int c = 0; c < (cable.retStereo ? 2 : 1); ++c)
            if (cable.in[c] >= 0 && cable.in[c] < kDevCh)
                for (int k = 0; k < B; ++k)
                {
                    const int64_t src = base + k - phys;
                    in[cable.in[c]][static_cast<size_t>(k)] = src >= 0 ? wire[c][static_cast<size_t>(src)] : 0.0;
                }
        inst.onPre(base, B, inPtr, kDevCh);
        if (afterPre != nullptr) { afterPre(inst); afterPre = nullptr; }
        float* p[2] { io[0].data(), hostCh > 1 ? io[1].data() : nullptr };
        if (playing)
        {
            // REAPER primes a segment with R rounded up to whole blocks plus one block (F0.3: R 1000 at B 512 -> 1024 fed ahead)
            const int m = static_cast<int>(segStart ? (R + B - 1) / B * B + B : B);
            for (int k = 0; k < m; ++k) { io[0][static_cast<size_t>(k)] = content(label + k, 0); io[1][static_cast<size_t>(k)] = content(label + k, 1); }
            (void) inst.process(p, hostCh, m, label, true);
            for (int k = 0; k < m; ++k)
            {
                const int64_t q = label + k - R;
                if (q >= 0 && q < static_cast<int64_t>(outL.size())) { outL[static_cast<size_t>(q)] = io[0][static_cast<size_t>(k)]; outR[static_cast<size_t>(q)] = io[1][static_cast<size_t>(k)]; }
            }
            label += m; segStart = false;
        }
        else
        {
            for (int k = 0; k < B; ++k) { io[0][static_cast<size_t>(k)] = 0.25f; io[1][static_cast<size_t>(k)] = -0.25f; }
            (void) inst.process(p, hostCh, B, 0, false);
        }
        for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) out[c][static_cast<size_t>(k)] = kUntouchedOut;
        inst.onPost(base, B, outPtr, kDevCh);
        for (int c = 0; c < kDevCh; ++c)
        {
            const bool mine = c == cable.out[0] || (cable.sendStereo && c == cable.out[1]);
            if (! mine) for (int k = 0; k < B; ++k) foreignWrites += out[c][static_cast<size_t>(k)] != kUntouchedOut;
        }
        for (int c = 0; c < (cable.sendStereo ? 2 : 1); ++c)
            if (cable.out[c] >= 0 && cable.out[c] < kDevCh)
                for (int k = 0; k < B; ++k) wire[c][static_cast<size_t>(base + k)] = out[cable.out[c]][static_cast<size_t>(k)];
        ++cycle;
    }
    void run(int cycles) noexcept { for (int c = 0; c < cycles; ++c) step(); }
};

constexpr Pair kStereoPair { { 10, 11 }, { 12, 13 }, true, true };
constexpr Routing kStereoRouting { true, true, 0.0f, 0.0f };
} // namespace

TEST_CASE("instance_stereo_pair_channel_selection_exact_after_gap")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    REQUIRE(inst.physLoop() == 2 * B);
    REQUIRE(inst.reportedLatency() == 4 * B);
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(300);
    int64_t bad = 0;
    for (int64_t q = 0; q < 15000; ++q)
    {
        const auto i = static_cast<size_t>(q);
        if (q < 2 * B) bad += (rig.outL[i] != 0.0f) + (rig.outR[i] != 0.0f);
        else bad += (rig.outL[i] != content(q, 0)) + (rig.outR[i] != content(q, 1));
    }
    REQUIRE(bad == 0);
    REQUIRE(rig.foreignWrites == 0);
    REQUIRE(inst.engine().status().holdUnderruns == 0);
    REQUIRE(inst.engine().status().inherentGap == 2 * B);
}

TEST_CASE("instance_mono_pair_sum_send_both_return")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 128;
    const Pair mono { { 5, -1 }, { 6, -1 }, false, false };
    Instance inst(hooks);
    REQUIRE(inst.configure(mono, { false, false, 0.0f, 0.0f }, B));
    Rig rig(inst, B, 2, mono, 20000);
    rig.run(150);
    int64_t bad = 0;
    for (int64_t q = 2 * B; q < 15000; ++q)
    {
        const float e = (content(q, 0) + content(q, 1)) * 0.5f;
        bad += (rig.outL[static_cast<size_t>(q)] != e) + (rig.outR[static_cast<size_t>(q)] != e);
    }
    REQUIRE(bad == 0);
    REQUIRE(rig.foreignWrites == 0);
}

TEST_CASE("instance_routing_widths_follow_the_pair")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    const Pair mono { { 3, -1 }, { 4, -1 }, false, false };
    Instance inst(hooks);
    REQUIRE(inst.configure(mono, kStereoRouting /* widths stereo: the pair's mono widths win */, B));
    Rig rig(inst, B, 2, mono, 20000);
    rig.run(200);
    int64_t bad = 0;
    for (int64_t q = 2 * B; q < 10000; ++q)
    {
        const float e = (content(q, 0) + content(q, 1)) * 0.5f;
        bad += (rig.outL[static_cast<size_t>(q)] != e) + (rig.outR[static_cast<size_t>(q)] != e);
    }
    REQUIRE(bad == 0);
    REQUIRE(rig.foreignWrites == 0);   // channel out[1] = -1 is never written
}

TEST_CASE("instance_mono_send_stereo_return")
{
    // one output (the sum), two inputs: in 1 carries the loop of out 1, in 2 returns silence (nothing feeds the unit's R side)
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    const Pair pair { { 10, -1 }, { 12, 13 }, false, true };
    Instance inst(hooks);
    REQUIRE(inst.configure(pair, { false, true, 0.0f, 0.0f }, B));
    Rig rig(inst, B, 2, pair, 20000);
    rig.run(200);
    int64_t bad = 0;
    for (int64_t q = 2 * B; q < 10000; ++q)
    {
        const float e = (content(q, 0) + content(q, 1)) * 0.5f;
        bad += (rig.outL[static_cast<size_t>(q)] != e) + (rig.outR[static_cast<size_t>(q)] != 0.0f);
    }
    REQUIRE(bad == 0);
    REQUIRE(rig.foreignWrites == 0);   // out 11 (the pair's unused [1]) is never written
}

TEST_CASE("instance_stereo_send_mono_return")
{
    // two outputs, one input: in 1 (the loop of out 1 = the track's L) feeds both track channels
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    const Pair pair { { 10, 11 }, { 12, -1 }, true, false };
    Instance inst(hooks);
    REQUIRE(inst.configure(pair, { true, false, 0.0f, 0.0f }, B));
    Rig rig(inst, B, 2, pair, 20000);
    rig.run(200);
    int64_t bad = 0;
    for (int64_t q = 2 * B; q < 10000; ++q)
        bad += (rig.outL[static_cast<size_t>(q)] != content(q, 0)) + (rig.outR[static_cast<size_t>(q)] != content(q, 0));
    REQUIRE(bad == 0);
    REQUIRE(rig.foreignWrites == 0);
}

TEST_CASE("instance_single_channel_host_bus")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 1, kStereoPair, 20000);
    rig.run(200);
    int64_t bad = 0;
    for (int64_t q = 2 * B; q < 10000; ++q) bad += rig.outL[static_cast<size_t>(q)] != content(q, 0);
    REQUIRE(bad == 0);
}

TEST_CASE("instance_configure_rejects_invalid_and_sizes_fixed_latency")
{
    ni::host::HookDispatcher hooks { &testClock };
    Instance inst(hooks);
    REQUIRE_FALSE(inst.configure(kStereoPair, kStereoRouting, 0));
    REQUIRE_FALSE(inst.configure(kStereoPair, kStereoRouting, kBMax + 1));
    REQUIRE_FALSE(inst.configure({ { -1, 11 }, { 12, 13 }, true }, kStereoRouting, 64));
    REQUIRE_FALSE(inst.configure({ { 10, 11 }, { 12, -1 }, true }, kStereoRouting, 64));
    REQUIRE_FALSE(inst.active());
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, kBMax));
    REQUIRE(inst.active());
    REQUIRE(inst.physLoop() == 2 * kBMax);
    REQUIRE(inst.reportedLatency() == 4 * kBMax);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, 32));   // block change: no reallocation, new latency
    REQUIRE(inst.reportedLatency() == 128);
}

TEST_CASE("instance_release_stops_hook_io_and_process")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(50);
    inst.release();
    REQUIRE_FALSE(inst.active());
    float l[4] { 1, 1, 1, 1 }, r[4] { 1, 1, 1, 1 }; float* io[2] { l, r };
    REQUIRE_FALSE(inst.process(io, 2, 4, 0, true));
    for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) rig.out[c][static_cast<size_t>(k)] = kUntouchedOut;
    inst.onPost(rig.cycle * B, B, rig.outPtr, kDevCh);
    uint64_t changed = 0;
    for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) changed += rig.out[c][static_cast<size_t>(k)] != kUntouchedOut;
    REQUIRE(changed == 0);
}

TEST_CASE("instance_channel_beyond_device_is_ignored")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    const Pair far { { 40, 41 }, { 40, 41 }, true };
    Instance inst(hooks);
    REQUIRE(inst.configure(far, kStereoRouting, B));
    Rig rig(inst, B, 2, far, 20000);
    rig.run(100);
    REQUIRE(rig.foreignWrites == 0);
    int64_t nonZero = 0;
    for (int64_t q = 0; q < 5000; ++q) nonZero += (rig.outL[static_cast<size_t>(q)] != 0.0f) + (rig.outR[static_cast<size_t>(q)] != 0.0f);
    REQUIRE(nonZero == 0);
}

TEST_CASE("instance_send_frames_counted_only_while_playing")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.playing = false; rig.run(20);
    REQUIRE(inst.sendFrames() == 0);
    rig.playing = true; rig.run(10);
    REQUIRE(inst.sendFrames() == static_cast<uint64_t>(4 * B + B + 9 * B));
}

TEST_CASE("instance_registers_one_hook_client_while_configured")
{
    ni::host::HookDispatcher hooks { &testClock };
    struct Dummy final : ni::host::IHookClient
    {
        void onPre(int64_t, int, const ReaSample* const*, int) noexcept override {}
        void onPost(int64_t, int, ReaSample* const*, int) noexcept override {}
    } dummy;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, 64));
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, 128));   // reconfigure keeps the one registration
    int got = 0;
    while (hooks.registerClient(&dummy) >= 0) ++got;
    REQUIRE(got == ni::host::HookDispatcher::kMaxClients - 1);
    inst.release();
    REQUIRE(hooks.registerClient(&dummy) >= 0);
}

TEST_CASE("instance_rt_no_alloc")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 200000);
    AllocGuard g;
    REQUIRE_NO_ALLOC(rig.run(1000));
}

TEST_CASE("instance_rejected_reconfigure_while_active_releases_the_old_pair")
{
    // S3 review I1: a rejected configure must not leave the previous pair's channel map writing hardware outputs.
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(50);
    const Pair bad { { 8, -1 }, { 8, 9 }, true, true };   // stereo send with a missing right output
    REQUIRE_FALSE(inst.configure(bad, kStereoRouting, B));
    REQUIRE_FALSE(inst.active());
    for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) rig.out[c][static_cast<size_t>(k)] = kUntouchedOut;
    inst.onPost(rig.cycle * B, B, rig.outPtr, kDevCh);
    uint64_t changed = 0;
    for (int c = 0; c < kDevCh; ++c) for (int k = 0; k < B; ++k) changed += rig.out[c][static_cast<size_t>(k)] != kUntouchedOut;
    REQUIRE(changed == 0);
    REQUIRE_FALSE(inst.configure(kStereoPair, kStereoRouting, 0));   // invalid block size: also released, nothing kept
    REQUIRE_FALSE(inst.active());
}

TEST_CASE("instance_loop_extra_extends_physloop_and_stays_exact_after_gap")
{
    // Real interfaces loop in 2B + converter/HAL/path cost (F0.5b: Apollo Pultec over ADAT 2B + 32 + 314). Until the
    // Task 16 Measurer, the device record's s3LoopExtra carries that cost: physLoop = 2B + extra, R = physLoop + 2B rounded
    // up to whole blocks. An unaligned R costs gap: REAPER feeds the PDC burst in whole blocks, so the host runs
    // ceil(R/B)·B − R samples further ahead and those inputs cannot come back in time (Apollo bench b64 Pultec: gap 512, not 473).
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64, extra = 346;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B, extra));
    REQUIRE(inst.physLoop() == 2 * B + extra);
    REQUIRE(inst.reportedLatency() == (4 * B + extra + B - 1) / B * B);   // block-aligned: REAPER primes whole blocks
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(300);
    const int64_t gap = 2 * B + extra;
    int64_t bad = 0;
    for (int64_t q = 0; q < 15000; ++q)
    {
        const auto i = static_cast<size_t>(q);
        if (q < gap) bad += (rig.outL[i] != 0.0f) + (rig.outR[i] != 0.0f);
        else bad += (rig.outL[i] != content(q, 0)) + (rig.outR[i] != content(q, 1));
    }
    REQUIRE(bad == 0);
    REQUIRE(inst.engine().status().holdUnderruns == 0);
    REQUIRE(inst.engine().status().inherentGap == gap);
    REQUIRE_FALSE(inst.configure(kStereoPair, kStereoRouting, B, -1));
    REQUIRE_FALSE(inst.active());
}

TEST_CASE("instance_activation_mid_play_anchors_on_a_fresh_pre")
{
    // Review S3 I-1: configure() can land after this cycle's PRE fan-out and before this track's processBlock. The PRE of
    // that cycle never reached the engine (inactive), so a segment started in that processBlock would anchor on a stale
    // preBase_ (0 for a never-active instance) and stay silent until the next stop/seek. Wanted: exact again from the
    // first segment anchored on a fresh PRE.
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    for (const bool wasActive : { false, true })
    {
        CAPTURE(wasActive);
        Instance inst(hooks);
        Rig rig(inst, B, 2, kStereoPair, 40000);
        if (wasActive) { REQUIRE(inst.configure(kStereoPair, kStereoRouting, B)); rig.run(100); inst.release(); }
        rig.run(40);   // playing, instance inactive: no PRE reaches the engine
        rig.afterPre = [](Instance& i) { (void) i.configure(kStereoPair, kStereoRouting, 64); };
        const int64_t act = rig.label;
        rig.run(250);
        const int64_t from = act + 2 * B, to = rig.label - 4 * B;
        int64_t bad = 0;
        for (int64_t q = from; q < to; ++q)
        {
            const auto i = static_cast<size_t>(q);
            bad += (rig.outL[i] != content(q, 0)) + (rig.outR[i] != content(q, 1));
        }
        CAPTURE(from, to);
        REQUIRE(bad == 0);
        REQUIRE(inst.engine().status().holdUnderruns == 0);
    }
}

TEST_CASE("instance_apply_measured_replaces_provisional_latency")
{
    // Task 16: the Measurer's physLoop replaces configure()'s provisional 2B + extra; R = physLoop + 2B rounded up to whole
    // blocks; the loop stays exact after the inherent gap; rejected while inactive or beyond L_max − 2B.
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE_FALSE(inst.applyMeasured(500));   // not configured
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    REQUIRE(inst.applyMeasured(2 * B + 346));
    REQUIRE(inst.physLoop() == 2 * B + 346);
    REQUIRE(inst.reportedLatency() == (4 * B + 346 + B - 1) / B * B);
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(300);
    const int64_t gap = 2 * B + 346;
    int64_t bad = 0;
    for (int64_t q = 0; q < 15000; ++q)
    {
        const auto i = static_cast<size_t>(q);
        if (q < gap) bad += (rig.outL[i] != 0.0f) + (rig.outR[i] != 0.0f);
        else bad += (rig.outL[i] != content(q, 0)) + (rig.outR[i] != content(q, 1));
    }
    REQUIRE(bad == 0);
    REQUIRE_FALSE(inst.applyMeasured(kLMax - 2 * B + 1));
    REQUIRE_FALSE(inst.applyMeasured(-1));
    REQUIRE(inst.physLoop() == 2 * B + 346);   // a rejected value changes nothing
    inst.release();
    REQUIRE_FALSE(inst.applyMeasured(500));
}

// Task 19: the instance feeds its HostTransitionObserver from the RT side — processBlock calls while playing (with the hook
// cycle index) and one onCycle per hook POST (every returned tag == p − R, contiguous) — so the PdcReporter can accept a
// PDC change only after the slot engine saw it settle (F1 §9 Q9).
TEST_CASE("instance_feeds_the_pdc_observer_until_stable_latency_is_accepted")
{
    using Phase = HostTransitionObserver::Phase;
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 40000);
    rig.playing = false;
    inst.pdcObserver().begin(0, inst.reportedLatency(), B, false, 0);   // change requested while stopped
    rig.run(20);
    REQUIRE(inst.pdcObserver().poll(1) == Phase::LatencyChangeRequested);   // stopped blocks are not a catch-up event
    inst.pdcObserver().onTransport(true, 2);                                // the first play segment is the event
    REQUIRE(inst.pdcObserver().poll(2) == Phase::HostTransitionObserved);
    rig.playing = true; rig.segStart = true;
    rig.run(20);
    REQUIRE(inst.pdcObserver().poll(3) == Phase::StableLatencyAccepted);
}

TEST_CASE("instance_pdc_observer_rt_feed_does_not_allocate")
{
    ni::host::HookDispatcher hooks { &testClock };
    const int B = 64;
    Instance inst(hooks);
    REQUIRE(inst.configure(kStereoPair, kStereoRouting, B));
    Rig rig(inst, B, 2, kStereoPair, 20000);
    rig.run(4);
    inst.pdcObserver().begin(inst.reportedLatency(), inst.reportedLatency() + B, B, true, 0);
    AllocGuard g;
    REQUIRE_NO_ALLOC(rig.run(20));
}

// Task 21 (F1 §10): the RenderArbiter's "involved" test needs when this instance last ran processBlock — a steady-clock ms
// stamp written by process() (RT: one clock read + one relaxed store), whether or not the loop is active.
TEST_CASE("instance_process_stamps_last_processed_ms")
{
    ni::host::HookDispatcher hooks { &testClock };
    Instance inst(hooks);
    REQUIRE(inst.lastProcessedMs() < 0);   // never processed
    const int64_t t0 = Instance::steadyMs();
    float l[4] {}, r[4] {}; float* io[2] { l, r };
    (void) inst.process(io, 2, 4, 0, false);   // inactive: still processed
    const int64_t t1 = Instance::steadyMs();
    REQUIRE(inst.lastProcessedMs() >= t0);
    REQUIRE(inst.lastProcessedMs() <= t1);
}
