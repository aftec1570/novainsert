// SlotLoopEngine (F1 §3 slot-locked loop, §8 mute, §14 RT, §20 routing) through the simulated host of host_sim.hpp.
#include <catch2/catch_test_macros.hpp>
#include <numbers>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <vector>
#include "engine/meters.hpp"
#include "engine/slot_loop.hpp"
#include "rt/alloc_guard.hpp"
#include "unit/host_sim.hpp"

using namespace ni::engine;
using ni::test::content;
using ni::test::HostSim;

namespace
{
constexpr Routing kStereo { true, true, 0.0f, 0.0f };

// Every written position in [from, to): zero on both sides before `gapEnd`, bit-exact afterwards. Returns mismatches.
int64_t checkGapThenExact(const HostSim& s, int64_t from, int64_t gapEnd, int64_t to)
{
    int64_t bad = 0;
    for (int64_t q = from; q < to; ++q)
    {
        const auto i = static_cast<size_t>(q);
        if (! s.written[i]) { ++bad; continue; }
        if (q < gapEnd) bad += (s.outL[i] != 0.0f) + (s.outR[i] != 0.0f);
        else bad += (s.outL[i] != content(q, 0)) + (s.outR[i] != content(q, 1));
    }
    return bad;
}

float sumOf(int64_t q) { return (content(q, 0) + content(q, 1)) * 0.5f; }
} // namespace

TEST_CASE("slot_loop_exact_after_inherent_gap")
{
    const int B = GENERATE(32, 64, 128, 256, 512, 1024, 2048);
    const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    const int64_t P = 1000, tenSec = 10 * kDefaultSampleRate;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo);
    REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, P + tenSec + 8 * R + 8 * B, P + tenSec);
    s.add(e, 2, physLoop, R);
    s.runCycles(20);                                    // stopped / pre-roll blocks before the segment
    s.play(P);
    s.runCycles(static_cast<int>((tenSec + R + physLoop) / B) + 4);
    INFO("B=" << B);
    REQUIRE(s.stoppedNonZero == 0);
    REQUIRE(checkGapThenExact(s, P, P + physLoop, P + tenSec) == 0);
    const auto st = e.status();
    REQUIRE(st.inherentGap == physLoop);
    REQUIRE(st.holdUnderruns == 0);
    REQUIRE(st.lateDrops == 0);
    REQUIRE(st.continuityOk);
    REQUIRE(st.lagObserved == R);
    REQUIRE(st.stableCycles > 2);
}

TEST_CASE("slot_loop_chain_of_three")
{
    const int B = 32;
    const int32_t pl[3] { 2 * B, 3 * B + 5, 2 * B }, R[3] { pl[0] + 2 * B, pl[1] + 2 * B, pl[2] + 2 * B };
    SlotLoopEngine e[3];
    HostSim s(B, B, 200000, 100000);
    for (int i = 0; i < 3; ++i) { e[i].prepare(B); e[i].setRouting(kStereo); REQUIRE(e[i].setLatency(pl[i], R[i])); s.add(e[i], 2, pl[i], R[i]); }
    s.runCycles(5);
    s.play(0);
    s.runCycles(90000 / B);
    const int64_t gap = pl[0] + R[1] + R[2];
    REQUIRE(checkGapThenExact(s, 0, gap, 80000) == 0);
    REQUIRE(e[0].status().inherentGap == gap);
    for (auto& x : e) REQUIRE(x.status().holdUnderruns == 0);
}

TEST_CASE("slot_loop_variable_block_size")
{
    const int n = GENERATE(1, 33, 4096);
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, n, 100000, 60000);
    s.add(e, 2, physLoop, R);
    s.runCycles(3); s.play(0); s.runCycles(1000);
    INFO("n=" << n);
    REQUIRE(checkGapThenExact(s, 0, physLoop, 60000) == 0);
    REQUIRE(e.status().inherentGap == physLoop);
    REQUIRE(e.status().holdUnderruns == 0);
}

TEST_CASE("slot_loop_ring_wrap_at_lmax")
{
    const int B = 64;
    {
        const int32_t physLoop = kLMax - 2 * B - 64, R = physLoop + 2 * B;
        SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
        HostSim s(B, B, 400000, 250000);
        s.add(e, 2, physLoop, R);
        s.play(0); s.runCycles(5000);
        REQUIRE(checkGapThenExact(s, 0, physLoop, 250000) == 0);
        REQUIRE(e.status().holdUnderruns == 0);
    }
    {
        SlotLoopEngine e; e.prepare(B);
        const int32_t physLoop = kLMax - 2 * B + 1;
        REQUIRE_FALSE(e.setLatency(physLoop, physLoop + 2 * B));
        REQUIRE_FALSE(e.setLatency(1000, 1000 + 2 * B - 1));   // margin below 2 blocks
        REQUIRE(e.setLatency(1000, 1000 + 2 * B));
    }
}

// Per-direction widths (mono/stereo routing design 2026-11-05): a mono send is (L + R) / 2 on the one output, the second
// output of the pair is never written; a mono return is the one input on both track channels.
TEST_CASE("slot_loop_width_routing_table")
{
    const bool sendStereo = GENERATE(false, true);
    const bool retStereo = GENERATE(false, true);
    const int B = 64; const int32_t physLoop = 2 * B + 7, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting({ sendStereo, retStereo, 0.0f, 0.0f }); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);   // both wires looped: a one-channel side sees silence on the other wire
    s.runCycles(2); s.play(0); s.runCycles(300);
    INFO("sendStereo=" << sendStereo << " retStereo=" << retStereo);
    // physical out: slot fs + j carries input q = j + physLoop (S0 = fs − P − physLoop for one instance)
    int64_t badWire = 0;
    for (int64_t j = 0; j < 15000; ++j)
    {
        const int64_t q = j + physLoop;
        badWire += s.wire(0, 0, s.playSlot() + j) != (sendStereo ? content(q, 0) : sumOf(q));
        badWire += s.wire(0, 1, s.playSlot() + j) != (sendStereo ? content(q, 1) : 0.0f);
    }
    REQUIRE(badWire == 0);
    int64_t bad = 0;
    for (int64_t q = physLoop; q < 15000; ++q)
    {
        const float a = sendStereo ? content(q, 0) : sumOf(q), b = sendStereo ? content(q, 1) : 0.0f;   // what returns on in 1 / in 2
        const float wantL = a, wantR = retStereo ? b : a;
        bad += (s.outL[static_cast<size_t>(q)] != wantL) + (s.outR[static_cast<size_t>(q)] != wantR);
    }
    REQUIRE(bad == 0);
}

TEST_CASE("slot_loop_mono_send_keeps_the_level_of_identical_channels")
{
    // user 2026-11-05: "make sure the sum level is the same as the original" – a mono item plays on both track channels
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting({ false, false, 0.0f, 0.0f }); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.inputOverride = [] (int64_t q, float& l, float& r) { l = r = content(q, 0); };
    s.add(e, 2, physLoop, R);
    s.runCycles(2); s.play(0); s.runCycles(200);
    int64_t bad = 0;
    for (int64_t j = 0; j < 10000; ++j) bad += s.wire(0, 0, s.playSlot() + j) != content(j + physLoop, 0);
    for (int64_t q = physLoop; q < 10000; ++q) bad += (s.outL[static_cast<size_t>(q)] != content(q, 0)) + (s.outR[static_cast<size_t>(q)] != content(q, 0));
    REQUIRE(bad == 0);
}

TEST_CASE("slot_loop_stereo_no_swap_no_sum")
{
    const int B = 128; const int32_t physLoop = 300, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(200);
    int64_t bad = 0;
    for (int64_t j = 0; j < 15000; ++j)
    {
        bad += s.wire(0, 0, s.playSlot() + j) != content(j + physLoop, 0);
        bad += s.wire(0, 1, s.playSlot() + j) != content(j + physLoop, 1);
    }
    REQUIRE(bad == 0);
    REQUIRE(content(5, 0) != content(5, 1));
    REQUIRE(checkGapThenExact(s, 0, physLoop, 15000) == 0);
}

TEST_CASE("slot_loop_fence_blocks_hook_io")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    e.setFenced(true);
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R, 0.125f);   // host pattern left in hwOut before POST
    s.play(0); s.runCycles(200);
    int64_t touched = 0, nonZeroOut = 0;
    for (int64_t slot = 0; slot < 200 * B; ++slot) touched += (s.wire(0, 0, slot) != 0.125f) + (s.wire(0, 1, slot) != 0.125f);
    for (int64_t q = 0; q < 10000; ++q) nonZeroOut += (s.outL[static_cast<size_t>(q)] != 0.0f) + (s.outR[static_cast<size_t>(q)] != 0.0f);
    REQUIRE(touched == 0);       // hwOut untouched
    REQUIRE(nonZeroOut == 0);    // hwIn unread: the wire carries 0.125 but no return frame ever reaches the output
}

TEST_CASE("slot_loop_mute_fade_5ms")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 60000, 40000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(100);
    const int64_t q0 = s.nextLabel() - s.rTotal();
    e.setMuted(true);
    s.runCycles(100);
    const int64_t q1 = s.nextLabel() - s.rTotal();
    e.setMuted(false);
    s.runCycles(100);
    const double pi = 3.14159265358979323846;
    int64_t bad = 0;
    for (int64_t k = 0; k < q1 - q0; ++k)
    {
        const double g = k < 240 ? 0.5 * (1.0 + std::cos(pi * static_cast<double>(k) / 240.0)) : 0.0;
        const auto i = static_cast<size_t>(q0 + k);
        if (k >= 240) bad += (s.outL[i] != 0.0f) + (s.outR[i] != 0.0f);   // exactly 0.0 after the fade
        else bad += (std::abs(s.outL[i] - g * content(q0 + k, 0)) > 1e-6) + (std::abs(s.outR[i] - g * content(q0 + k, 1)) > 1e-6);
    }
    for (int64_t k = 0; k < 400; ++k)
    {
        const double g = k < 240 ? 0.5 * (1.0 - std::cos(pi * static_cast<double>(k) / 240.0)) : 1.0;
        const auto i = static_cast<size_t>(q1 + k);
        if (k >= 240) bad += (s.outL[i] != content(q1 + k, 0)) + (s.outR[i] != content(q1 + k, 1));
        else bad += (std::abs(s.outL[i] - g * content(q1 + k, 0)) > 1e-6) + (std::abs(s.outR[i] - g * content(q1 + k, 1)) > 1e-6);
    }
    REQUIRE(q1 - q0 > 240);
    REQUIRE(bad == 0);
}

TEST_CASE("slot_loop_probe_replaces_program")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 60000, 40000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(50);
    std::vector<float> probe(500);
    for (size_t i = 0; i < probe.size(); ++i) probe[i] = 0.5f * std::sin(0.05f * static_cast<float>(i)) + 0.001f;
    e.setProbe(probe.data(), static_cast<int32_t>(probe.size()));
    const int64_t probeSlot = s.cycle() * B;   // the next POST starts the probe
    s.runCycles(20);
    int64_t bad = 0;
    for (int64_t i = 0; i < 20 * B; ++i)
    {
        const float want = i < 500 ? probe[static_cast<size_t>(i)] : 0.0f;   // probe, then silence while armed: no program
        bad += (s.wire(0, 0, probeSlot + i) != want) + (s.wire(0, 1, probeSlot + i) != want);
    }
    REQUIRE(bad == 0);
    e.setProbe(nullptr, 0);
    const int64_t backSlot = s.cycle() * B;
    s.runCycles(5);
    const int64_t S0 = s.playSlot() - 0 - physLoop;
    for (int64_t i = 0; i < 5 * B; ++i) bad += s.wire(0, 0, backSlot + i) != content(backSlot + i - S0, 0);
    REQUIRE(bad == 0);   // program resumes slot-locked after the probe is disarmed
}

TEST_CASE("slot_loop_captured_count_tracks_current_probe")
{
    const int B = 64;
    SlotLoopEngine e; e.prepare(B); e.setRouting({ false, false, 0.0f, 0.0f });
    REQUIRE(e.setLatency(300, 300 + 2 * B));
    HostSim s(B, B, 40000, 1000);
    s.add(e, 1, 300, 300 + 2 * B);
    s.runCycles(5);
    REQUIRE(e.capturedCount() == 0);   // no probe armed
    std::vector<float> probe(400, 0.1f);
    e.setProbe(probe.data(), 400);
    REQUIRE(e.capturedCount() == 0);   // armed, not latched by the hook yet
    s.runCycles(10);
    const int32_t c = e.capturedCount();
    REQUIRE(c >= 9 * B);
    REQUIRE(c <= 10 * B);
    s.runCycles(1);
    REQUIRE(e.capturedCount() == c + B);
    e.setProbe(nullptr, 0);
    REQUIRE(e.capturedCount() == 0);
}

TEST_CASE("slot_loop_cut_probe_silences_the_send_from_the_next_block")
{
    // Record start mid-probe (plan Review Focus 3): processBlock cuts the armed probe RT-safely; the send carries silence from
    // the next POST on (never the program – the message thread disarms later), and a probe armed after the cut plays normally.
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 60000, 40000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(20);
    std::vector<float> probe(5000, 0.25f);
    e.setProbe(probe.data(), 5000);
    const int64_t probeSlot = s.cycle() * B;
    s.runCycles(10);
    e.cutProbe();
    const int64_t cutSlot = s.cycle() * B;
    s.runCycles(10);
    int64_t bad = 0;
    for (int64_t i = probeSlot; i < cutSlot; ++i) bad += s.wire(0, 0, i) != 0.25f;
    for (int64_t i = cutSlot; i < cutSlot + 10 * B; ++i) bad += (s.wire(0, 0, i) != 0.0f) + (s.wire(0, 1, i) != 0.0f);
    REQUIRE(bad == 0);
    std::vector<float> next(300, 0.5f);
    e.setProbe(next.data(), 300);
    const int64_t nextSlot = s.cycle() * B;
    s.runCycles(10);
    for (int64_t i = 0; i < 300; ++i) bad += s.wire(0, 0, nextSlot + i) != 0.5f;
    REQUIRE(bad == 0);
}

TEST_CASE("slot_loop_capture_return_aligned_to_probe_start")
{
    const int B = 64; const int32_t physLoop = 300;   // not a block multiple; transport stopped (calibration at load)
    SlotLoopEngine e; e.prepare(B); e.setRouting({ false, false, 0.0f, 0.0f });
    REQUIRE(e.setLatency(physLoop, physLoop + 2 * B));
    HostSim s(B, B, 40000, 1000);
    s.add(e, 1, physLoop, physLoop + 2 * B);
    s.runCycles(10);
    std::vector<float> probe(400);
    for (size_t i = 0; i < probe.size(); ++i) probe[i] = 0.25f + 0.001f * static_cast<float>(i);
    e.setProbe(probe.data(), 400);
    const int32_t N = physLoop + 400 + 100;
    std::vector<float> cap(static_cast<size_t>(N), -1.0f);
    s.runCycles(2);
    REQUIRE_FALSE(e.captureReturn(cap.data(), nullptr, N));   // not filled yet
    s.runCycles(30);
    REQUIRE(e.captureReturn(cap.data(), nullptr, N));
    int64_t bad = 0;
    for (int32_t i = 0; i < N; ++i)
    {
        const float want = (i >= physLoop && i < physLoop + 400) ? probe[static_cast<size_t>(i - physLoop)] : 0.0f;
        bad += cap[static_cast<size_t>(i)] != want;
    }
    REQUIRE(bad == 0);
}

namespace
{
void injectNan(int64_t q, float& l, float& r)
{
    if (q == 1000) l = std::nanf("");
    if (q == 1001) r = INFINITY;
}
} // namespace

TEST_CASE("slot_loop_nan_flush_counted")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 20000, 5000);
    s.add(e, 2, physLoop, R);
    s.inputOverride = &injectNan;
    s.play(0); s.runCycles(100);
    REQUIRE(e.status().nanFlushed == 2);
    REQUIRE(s.outL[1000] == 0.0f);
    REQUIRE(s.outR[1001] == 0.0f);
    REQUIRE(s.outR[1000] == content(1000, 1));
    int64_t nonFinite = 0;
    for (int64_t q = 0; q < 5000; ++q) nonFinite += ! std::isfinite(s.outL[static_cast<size_t>(q)]) + ! std::isfinite(s.outR[static_cast<size_t>(q)]);
    REQUIRE(nonFinite == 0);
}

TEST_CASE("slot_loop_seek_new_segment_reanchors")
{
    const int B = 64; const int32_t physLoop = 2 * B + 3, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 60000, 140000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(200);
    s.play(100000);   // seek: position jump + new segment generation
    s.runCycles(300);
    REQUIRE(checkGapThenExact(s, 100000, 100000 + physLoop, 115000) == 0);
    REQUIRE(e.status().inherentGap == physLoop);
    REQUIRE(e.status().holdUnderruns == 0);
}

TEST_CASE("slot_loop_continuity_flag_on_missing_return_block")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(100);
    REQUIRE(e.status().continuityOk);
    REQUIRE(e.status().stableCycles > 50);
    s.skipPreAtCycle = s.cycle() + 50;
    s.runCycles(100);
    const auto st = e.status();
    REQUIRE_FALSE(st.continuityOk);
    REQUIRE(st.holdUnderruns > 0);
    REQUIRE(st.stableCycles < 50);   // counting restarted after the dropout cycle
}

TEST_CASE("slot_loop_rt_no_alloc")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 80000, 70000);
    s.add(e, 2, physLoop, R);
    s.runCycles(2);
    s.play(0);
    {
        AllocGuard g;
        s.runCycles(1000);
        REQUIRE(AllocGuard::count() == 0);
    }
    REQUIRE(e.status().holdUnderruns == 0);
}

// Task 11 additions (Instance wiring): the hook wrapper converts REAPER's double buffers through float scratch and must
// know whether the engine wrote the hardware output; the device block can change without reallocating the rings.
TEST_CASE("slot_loop_onpost_reports_whether_it_wrote")
{
    const int B = 64;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(2 * B, 4 * B));
    std::vector<float> l(B, 7.0f), r(B, 7.0f); float* out[2] { l.data(), r.data() };
    REQUIRE(e.onPost(0, B, out));
    e.setFenced(true);
    REQUIRE_FALSE(e.onPost(B, B, out));
    SlotLoopEngine unprepared;
    REQUIRE_FALSE(unprepared.onPost(0, B, out));
}

TEST_CASE("slot_loop_set_hw_block_revalidates_latency_without_realloc")
{
    SlotLoopEngine e; e.prepare(64); e.setRouting(kStereo);
    REQUIRE(e.setLatency(128, 256));
    AllocGuard g;
    REQUIRE_NO_ALLOC(e.setHwBlock(256));
    REQUIRE_FALSE(e.setLatency(512, 1023));   // R >= physLoop + 2·256 now required
    REQUIRE(e.setLatency(512, 1024));
}

TEST_CASE("slot_loop_segments_counted")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = 4 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    REQUIRE(e.status().segments == 0);
    HostSim s(B, B, 60000, 140000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(100);
    REQUIRE(e.status().segments == 1);
    s.play(100000); s.runCycles(100);
    REQUIRE(e.status().segments == 2);
    s.stop(); s.runCycles(10); s.play(0); s.runCycles(50);
    REQUIRE(e.status().segments == 3);
}

TEST_CASE("slot_loop_trim_residue_near_zero_db_is_bit_exact_unity")
{
    // A 0 dB default stored as a normalised host parameter (-24..+12 dB) round-trips to ~1e-6 dB,
    // which as a gain is 1 - 2^-23: the loop must still be bit-exact (found by f3_3 in REAPER).
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B);
    e.setRouting(Routing { true, true, 1.2e-6f, 1.2e-6f });
    REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(400);
    REQUIRE(checkGapThenExact(s, 0, physLoop, 20000) == 0);
}

TEST_CASE("slot_loop_unprimed_loop_wrap_exact_from_loop_start_and_gap_reported")
{
    // REAPER loop wrap, measured by the f3_3 heard capture: no PDC re-prime; the loop start is heard exact from its first
    // sample, the last R heard samples of the previous pass are silent (LIMITATION), and inherentGap must report them.
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 60000, 140000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(200);
    s.jumpUnprimed(100000);
    s.runCycles(300);
    INFO("gap " << e.status().inherentGap << " under " << e.status().holdUnderruns << " cont " << e.status().continuityOk);
    REQUIRE(checkGapThenExact(s, 100000, 100000, 115000) == 0);
    int64_t tailNonZero = 0;
    for (int32_t t = 0; t < R; ++t) tailNonZero += (s.wrapTailL[static_cast<size_t>(t)] != 0.0f) + (s.wrapTailR[static_cast<size_t>(t)] != 0.0f);
    REQUIRE(tailNonZero == 0);
    REQUIRE(e.status().holdUnderruns == 0);
    REQUIRE(e.status().continuityOk);
    REQUIRE(e.status().inherentGap == R);
}

TEST_CASE("slot_loop_split_block_loop_wrap_reports_heard_gap")
{
    // REAPER loop wrap inside a device cycle (loop length not a multiple of B, f3_3 heard capture at B = 512): the host
    // pushes s samples to the loop end and B − s from the loop start before the same POST. The silence heard between the
    // passes must equal inherentGap of the wrap segment, and the loop start must be bit-exact right after it.
    const int B = GENERATE(64, 512);
    const int split = GENERATE_COPY(0, B / 4, B / 2, 3 * B / 4, 24);   // old-pass samples in the wrap block (0 = aligned)
    const int32_t physLoop = 2 * B, R = 4 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    const int64_t P = 100000;
    HostSim sim(B, B, 8 * 48000, P + 300 * B);
    sim.add(e, 2, physLoop, R);
    sim.play(0); sim.runCycles(200);
    const int64_t loopEnd = sim.nextLabel() + split;    // the old pass ends `split` samples into the next cycle
    sim.jumpUnprimedAt(split, P);
    sim.runCycles(300);
    // heard stream: the old pass is exact up to loopEnd − R (the s samples of the split block included), then silence for
    // the previous pass's last R positions (outputs for labels P .. P + R − 1), then the loop start minus lost samples
    int64_t oldBad = 0;
    for (int64_t q = loopEnd - R - 2 * B; q < loopEnd - R; ++q)
        oldBad += ! sim.written[static_cast<size_t>(q)] || sim.outL[static_cast<size_t>(q)] != content(q, 0) || sim.outR[static_cast<size_t>(q)] != content(q, 1);
    int64_t tailNonZero = 0;
    for (int32_t t = 0; t < R; ++t) tailNonZero += (sim.wrapTailL[static_cast<size_t>(t)] != 0.0f) + (sim.wrapTailR[static_cast<size_t>(t)] != 0.0f);
    int64_t lost = 0;
    while (lost < 4 * B && sim.written[static_cast<size_t>(P + lost)] && sim.outL[static_cast<size_t>(P + lost)] == 0.0f && sim.outR[static_cast<size_t>(P + lost)] == 0.0f) ++lost;
    const auto st = e.status();
    INFO("B=" << B << " s=" << split << " lost=" << lost << " gap=" << st.inherentGap << " under=" << st.holdUnderruns
              << " late=" << st.lateDrops << " cont=" << st.continuityOk);
    REQUIRE(oldBad == 0);
    REQUIRE(tailNonZero == 0);
    REQUIRE(checkGapThenExact(sim, P, P + lost, P + 250 * B) == 0);   // 300 cycles − R − margin: every position written
    REQUIRE(st.holdUnderruns == 0);
    REQUIRE(st.lateDrops == 0);
    REQUIRE(st.continuityOk);
    REQUIRE(st.inherentGap == R + lost);
}

namespace
{
// Analog loop (HostSim ring a): the heard output q must be the loop's own response to send stream content(q):
// out[q] − a·out[q−1] == content(q) within float rounding. Holds only when consecutive outputs are consecutive, correctly
// tagged returns (alignment), so it replaces bit-exactness for a loop with memory. Returns mismatches in [from, to).
int64_t checkRingAligned(const HostSim& s, float a, int64_t from, int64_t to)
{
    int64_t bad = 0;
    for (int64_t q = from; q < to; ++q)
    {
        const auto i = static_cast<size_t>(q);
        if (! s.written[i] || ! s.written[i - 1]) { ++bad; continue; }
        bad += std::fabs(s.outL[i] - a * s.outL[i - 1] - content(q, 0)) > 1e-5f;
        bad += std::fabs(s.outR[i] - a * s.outR[i - 1] - content(q, 1)) > 1e-5f;
    }
    return bad;
}
} // namespace

TEST_CASE("slot_loop_analog_loop_wrap_outputs_zero_before_segment_start")
{
    // Apollo / SSL analog loop finding: the hardware return keeps ringing with the previous pass's last sends after an
    // unprimed wrap. Those return frames carry tags before the new segment's start p0 and must never be output: the
    // heard pre-gap region [p0, p0 + R) is exact 0.0, the loop content is aligned right after it.
    const int B = 64; const int32_t physLoop = 488, R = 640;
    const int split = GENERATE(0, 24, 48);   // aligned wrap, and the wrap splitting the device block
    const float a = 0.9f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    const int64_t P = 100000;
    HostSim sim(B, B, 8 * 48000, P + 300 * B);
    sim.setRing(sim.add(e, 2, physLoop, R), a);
    sim.play(0); sim.runCycles(200);
    REQUIRE(checkRingAligned(sim, a, 2 * R, 150 * B) == 0);          // steady state: the ring model itself is aligned
    sim.jumpUnprimedAt(split, P);
    sim.runCycles(300);
    int64_t tailNonZero = 0, firstNonZero = -1;
    for (int32_t t = 0; t < R; ++t)
    {
        const bool nz = sim.wrapTailL[static_cast<size_t>(t)] != 0.0f || sim.wrapTailR[static_cast<size_t>(t)] != 0.0f;
        tailNonZero += nz;
        if (nz && firstNonZero < 0) firstNonZero = t;
    }
    const auto st = e.status();
    INFO("split=" << split << " tailNonZero=" << tailNonZero << " firstNonZero=" << firstNonZero << " gap=" << st.inherentGap
                  << " under=" << st.holdUnderruns << " lag=" << st.lagObserved << " stable=" << st.stableCycles);
    REQUIRE(tailNonZero == 0);                                       // every output for want < p0 is exact 0.0
    REQUIRE(checkRingAligned(sim, a, P + 1, P + 250 * B) == 0);      // loop content aligned from P on (tag == want)
    REQUIRE(st.inherentGap == R);
    REQUIRE(st.holdUnderruns == 0);
    REQUIRE(st.continuityOk);
    REQUIRE(st.lagObserved == R);
    REQUIRE(st.stableCycles > 2);
}

TEST_CASE("slot_loop_analog_loop_primed_seek_outputs_zero_before_segment_start")
{
    // Primed seek during play on an analog loop: the pre-gap outputs are discarded by the host; the heard gap region
    // [p0, p0 + physLoop) is exact 0.0 and the content is aligned afterwards.
    const int B = 64; const int32_t physLoop = 488, R = 640;
    const float a = 0.9f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    const int64_t P = 100000;
    HostSim sim(B, B, 8 * 48000, P + 300 * B);
    sim.setRing(sim.add(e, 2, physLoop, R), a);
    sim.play(0); sim.runCycles(200);
    sim.play(P); sim.runCycles(300);
    int64_t gapNonZero = 0;
    for (int64_t q = P; q < P + physLoop; ++q) gapNonZero += (sim.outL[static_cast<size_t>(q)] != 0.0f) + (sim.outR[static_cast<size_t>(q)] != 0.0f);
    const auto st = e.status();
    INFO("gapNonZero=" << gapNonZero << " gap=" << st.inherentGap << " under=" << st.holdUnderruns);
    REQUIRE(gapNonZero == 0);
    REQUIRE(checkRingAligned(sim, a, P + physLoop + R, P + 250 * B) == 0);   // once the ring has forgotten the pre-seek sends
    REQUIRE(st.inherentGap == physLoop);
    REQUIRE(st.holdUnderruns == 0);
    REQUIRE(st.continuityOk);
}

// ---- VU GUI plan Task 3: per-channel trims, looping trimmed CAL probe, hook-side meter taps ----------------------------------
TEST_CASE("slot_loop_per_channel_send_and_return_gain")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    Routing rt = kStereo;
    rt.sendOffsetDb[0] = -6.0f; rt.returnOffsetDb[1] = 6.0f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(200);
    const float g6 = std::pow(10.0f, -6.0f / 20.0f);
    int64_t bad = 0;
    for (int64_t j = 0; j < 10000; ++j)
    {
        bad += std::fabs(s.wire(0, 0, s.playSlot() + j) - content(j + physLoop, 0) * g6) > 1e-6f;
        bad += s.wire(0, 1, s.playSlot() + j) != content(j + physLoop, 1);
    }
    for (int64_t q = physLoop; q < 10000; ++q)
    {
        bad += std::fabs(s.outL[static_cast<size_t>(q)] - content(q, 0) * g6) > 1e-6f;
        bad += std::fabs(s.outR[static_cast<size_t>(q)] - content(q, 1) / g6) > 1e-5f;
    }
    REQUIRE(bad == 0);
}

TEST_CASE("slot_loop_trim_change_ramps_within_one_block_and_lands_exactly")
{
    // user 2026-11-05 "make sure the trims actually work": a trim set from processBlock (setTrims, every block) reaches the
    // audio at that block – not on the 20 ms message-thread timer – as a ramp across that one block (no zipper step), and is
    // exact from the next block on, on the send and on the return; the per-channel offsets still add on top.
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    Routing rt = kStereo; rt.returnOffsetDb[1] = 1.0f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.play(0); s.runCycles(100);
    e.setTrims(-6.0f, 3.0f);
    s.runCycles(100);
    const float gs = std::pow(10.0f, -6.0f / 20.0f), grL = std::pow(10.0f, 3.0f / 20.0f), grR = std::pow(10.0f, 4.0f / 20.0f);
    // send wire: gain 1 … ramp (≤ B samples, monotonic, per-sample step ≤ (1 − gs)/B) … exactly gs
    std::vector<float> ratio;
    for (int64_t j = 0; j < 150 * B; ++j) ratio.push_back(s.wire(0, 0, s.playSlot() + j) / content(j + physLoop, 0));
    size_t first = 0; while (first < ratio.size() && std::fabs(ratio[first] - 1.0f) < 1e-6f) ++first;
    size_t last = first; while (last < ratio.size() && std::fabs(ratio[last] - gs) > 1e-6f) ++last;
    REQUIRE(first > 50 * B);
    REQUIRE(last < ratio.size());
    REQUIRE(last - first <= static_cast<size_t>(B));
    for (size_t j = first; j <= last; ++j) REQUIRE(ratio[j] <= ratio[j - 1] + 1e-6f);
    for (size_t j = first; j <= last; ++j) REQUIRE(ratio[j - 1] - ratio[j] <= (1.0f - gs) / B + 1e-5f);
    for (size_t j = last; j < ratio.size(); ++j) REQUIRE(std::fabs(ratio[j] - gs) < 1e-6f);
    // plugin output after everything settled: send trim × loop (unity) × return trim (+ offset on R)
    for (int64_t q = 190 * B; q < 199 * B; ++q)
    {
        REQUIRE(std::fabs(s.outL[static_cast<size_t>(q)] - content(q, 0) * gs * grL) < 1e-5f);
        REQUIRE(std::fabs(s.outR[static_cast<size_t>(q)] - content(q, 1) * gs * grR) < 1e-5f);
    }
}

TEST_CASE("slot_loop_looping_trimmed_probe")
{
    const int B = 4096;
    Routing rt = kStereo; rt.sendTrimDb = -6.0f; rt.sendOffsetDb[1] = -6.0f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt); REQUIRE(e.setLatency(2 * B, 4 * B));
    std::vector<float> tone(4800);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * float(i % 48) / 48.0f);
    e.setProbe(tone.data(), 4800, true, true);
    std::vector<float> l(B), r(B), il(B, 0.0f), ir(B, 0.0f);
    float* out[2] { l.data(), r.data() };
    const float* in[2] { il.data(), ir.data() };
    const float gL = std::pow(10.0f, -6.0f / 20.0f), gR = std::pow(10.0f, -12.0f / 20.0f);
    int64_t bad = 0;
    for (int c = 0; c < 3; ++c)
    {
        e.onPre(int64_t(c) * B, B, in);
        REQUIRE(e.onPost(int64_t(c) * B, B, out));
        for (int k = 0; k < B; ++k)
        {
            const float t = tone[size_t((c * B + k) % 4800)];
            bad += std::fabs(l[size_t(k)] - t * gL) > 1e-6f;
            bad += std::fabs(r[size_t(k)] - t * gR) > 1e-6f;
        }
    }
    REQUIRE(bad == 0);   // still the tone after 12288 samples (> 4800): it loops, scaled per channel
}

TEST_CASE("slot_loop_hook_meter_taps")
{
    const int B = 4800;
    Routing rt = kStereo; rt.sendTrimDb = -6.0f; rt.returnTrimDb = -12.0f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt); REQUIRE(e.setLatency(2 * B, 4 * B));
    MeterEngine sm, rm; sm.prepare(kDefaultSampleRate); rm.prepare(kDefaultSampleRate);
    e.setMeters(&sm, &rm);
    std::vector<float> half(B, 0.5f), l(B), r(B);
    e.setProbe(half.data(), B, true, true);
    float* out[2] { l.data(), r.data() };
    const float* in[2] { half.data(), half.data() };
    e.onPre(0, B, in);
    REQUIRE(e.onPost(0, B, out));
    const auto s = sm.read(-20.0f), q = rm.read(-20.0f);
    REQUIRE(std::fabs(s.peakDb[0] - (-12.04f)) < 0.05f);
    REQUIRE(std::fabs(s.peakDb[1] - (-12.04f)) < 0.05f);
    REQUIRE(std::fabs(q.peakDb[0] - (-18.06f)) < 0.05f);   // after the return trim (user 2026-11-05: the knob must show in the meter)
    REQUIRE(std::fabs(q.peakDb[1] - (-18.06f)) < 0.05f);
}

TEST_CASE("slot_loop_return_meter_clip_is_the_converter_input_not_the_trimmed_level")
{
    const int B = 4800;
    Routing rt = kStereo; rt.returnTrimDb = -12.0f;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt); REQUIRE(e.setLatency(2 * B, 4 * B));
    MeterEngine sm, rm; sm.prepare(kDefaultSampleRate); rm.prepare(kDefaultSampleRate);
    e.setMeters(&sm, &rm);
    std::vector<float> hot(B, 1.001f), l(B), r(B);
    float* out[2] { l.data(), r.data() };
    const float* in[2] { hot.data(), hot.data() };
    e.onPre(0, B, in);
    REQUIRE(e.onPost(0, B, out));
    const auto q = rm.read(-20.0f);
    REQUIRE(std::fabs(q.peakDb[0] - (-11.99f)) < 0.05f);
    REQUIRE(q.clip[0]); REQUIRE(q.clip[1]);
}

TEST_CASE("slot_loop_rt_no_alloc_with_meters_and_looping_probe")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting(kStereo); REQUIRE(e.setLatency(physLoop, R));
    MeterEngine sm, rm; sm.prepare(kDefaultSampleRate); rm.prepare(kDefaultSampleRate);
    e.setMeters(&sm, &rm);
    std::vector<float> tone(4800, 0.1f);
    HostSim s(B, B, 80000, 70000);
    s.add(e, 2, physLoop, R);
    s.play(0);
    e.setProbe(tone.data(), 4800, true, true);
    {
        AllocGuard g;
        s.runCycles(1000);
        REQUIRE(AllocGuard::count() == 0);
    }
    REQUIRE(sm.read(-20.0f).peakDb[0] > -21.0f);
}
