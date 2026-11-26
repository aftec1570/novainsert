// Per-unit return polarity (D2, 1.5.0): the engine multiplies the return by -1 after the capture and before the return trim, both
// channels together, with no added latency, in Insert and Effect mode (one engine path; only the latency source differs).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>
#include <vector>
#include "engine/slot_loop.hpp"
#include "plugin/latency_source.hpp"
#include "rt/alloc_guard.hpp"
#include "unit/host_sim.hpp"

using namespace ni::engine;
using ni::test::content;
using ni::test::HostSim;

namespace
{
// One simulated play of `maxPos` samples through a digital loop; the same input for every call, so two runs differ only in the switch.
struct Out { std::vector<float> l, r; int32_t physLoop; };
Out play(Mode mode, bool sendStereo, bool retStereo, bool inverted, float returnTrimDb = 0.0f, float retOffsetDb = 0.0f)
{
    const int B = 64;
    // Insert: the probe's measured physLoop; Effect: the driver round trip (latencySource, as the processor derives it)
    const auto src = ni::plugin::latencySource(mode, 40, 117, B);
    const int32_t physLoop = mode == Mode::Insert ? 2 * B + 346 : src.phys;
    REQUIRE((mode == Mode::Insert ? src.probe : ! src.probe));
    const int32_t R = physLoop + 2 * B;
    Routing rt { sendStereo, retStereo, 0.0f, returnTrimDb };
    rt.returnOffsetDb[0] = retOffsetDb; rt.returnInverted = inverted;
    SlotLoopEngine e; e.prepare(B); e.setRouting(rt);
    REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.runCycles(2); s.play(0); s.runCycles(300);
    return { s.outL, s.outR, physLoop };
}
} // namespace

TEST_CASE("polarity_switch_on_is_the_exact_negation_of_switch_off")
{
    const Mode mode = GENERATE(Mode::Insert, Mode::Effect);
    const bool sendStereo = GENERATE(false, true);
    const bool retStereo = GENERATE(false, true);   // mono return = the one input on both track channels; mono unit = both channels flip together
    const float trim = GENERATE(0.0f, -6.0f);       // the sign goes in before the trim: still exact with a trim and an offset set
    INFO("mode=" << (mode == Mode::Insert ? "Insert" : "Effect") << " sendStereo=" << sendStereo << " retStereo=" << retStereo << " trim=" << trim);
    const auto off = play(mode, sendStereo, retStereo, false, trim, -1.5f);
    const auto on = play(mode, sendStereo, retStereo, true, trim, -1.5f);
    REQUIRE(off.physLoop == on.physLoop);   // no added latency
    int64_t bad = 0, nonZero = 0;
    for (size_t i = 0; i < 15000; ++i)
    {
        bad += (on.l[i] != -off.l[i]) + (on.r[i] != -off.r[i]);
        nonZero += (off.l[i] != 0.0f) + (off.r[i] != 0.0f);
    }
    REQUIRE(bad == 0);          // sample by sample
    REQUIRE(nonZero > 10000);   // and the signal really came back (a silent pair would pass trivially)
}

TEST_CASE("polarity_switch_off_is_bit_identical_to_the_engine_without_the_field")
{
    const auto a = play(Mode::Insert, true, true, false);
    SlotLoopEngine e; e.prepare(64);
    Routing rt { true, true, 0.0f, 0.0f };   // aggregate without the polarity member: default normal
    REQUIRE_FALSE(rt.returnInverted);
    e.setRouting(rt); REQUIRE(e.setLatency(2 * 64 + 346, 2 * 64 + 346 + 128));
    HostSim s(64, 64, 40000, 20000);
    s.add(e, 2, 2 * 64 + 346, 2 * 64 + 346 + 128);
    s.runCycles(2); s.play(0); s.runCycles(300);
    int64_t bad = 0;
    for (size_t i = 0; i < 15000; ++i) bad += (a.l[i] != s.outL[i]) + (a.r[i] != s.outR[i]);
    REQUIRE(bad == 0);
}

TEST_CASE("polarity_the_capture_stays_raw_so_the_measurement_still_sees_the_hardware")
{
    // the switch acts on the playback path only: the correlator's capture is the converter's signal, inverted or not
    const int B = 64; const int32_t physLoop = 300;
    std::vector<float> cap[2];
    for (int inv = 0; inv < 2; ++inv)
    {
        SlotLoopEngine e; e.prepare(B);
        Routing rt { false, false, 0.0f, 0.0f }; rt.returnInverted = inv != 0;
        e.setRouting(rt); REQUIRE(e.setLatency(physLoop, physLoop + 2 * B));
        HostSim s(B, B, 40000, 1000);
        s.add(e, 1, physLoop, physLoop + 2 * B);
        s.runCycles(10);
        std::vector<float> probe(400);
        for (size_t i = 0; i < probe.size(); ++i) probe[i] = 0.25f + 0.001f * static_cast<float>(i);
        e.setProbe(probe.data(), 400);
        const int32_t N = physLoop + 400 + 100;
        cap[inv].assign(static_cast<size_t>(N), -1.0f);
        s.runCycles(32);
        REQUIRE(e.captureReturn(cap[inv].data(), nullptr, N));
    }
    REQUIRE(cap[0] == cap[1]);
}

TEST_CASE("polarity_flip_while_playing_is_a_single_step_at_a_block_boundary")
{
    const int B = 64; const int32_t physLoop = 2 * B + 5, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B); e.setRouting({ true, true, 0.0f, 0.0f }); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 40000, 20000);
    s.add(e, 2, physLoop, R);
    s.runCycles(2); s.play(0); s.runCycles(100);
    e.setReturnInverted(true);   // what the processor's sync does from the message thread
    s.runCycles(200);
    int64_t pos = 0, neg = 0, other = 0, firstNeg = -1;
    for (int64_t q = physLoop; q < 15000; ++q)
    {
        const float v = s.outL[static_cast<size_t>(q)], c = content(q, 0);
        if (v == c) { ++pos; if (firstNeg >= 0) ++other; }
        else if (v == -c) { ++neg; if (firstNeg < 0) firstNeg = q; }
        else ++other;
    }
    REQUIRE(other == 0);   // never anything but +x before the step and -x after it
    REQUIRE(pos > 0);
    REQUIRE(neg > 0);
}

TEST_CASE("polarity_rt_no_alloc")
{
    const int B = 64; const int32_t physLoop = 2 * B, R = physLoop + 2 * B;
    SlotLoopEngine e; e.prepare(B);
    Routing rt { true, true, 0.0f, 0.0f }; rt.returnInverted = true;
    e.setRouting(rt); REQUIRE(e.setLatency(physLoop, R));
    HostSim s(B, B, 200000, 100000);
    s.add(e, 2, physLoop, R);
    s.runCycles(2); s.play(0);
    AllocGuard g;
    REQUIRE_NO_ALLOC(s.runCycles(500));
    REQUIRE_NO_ALLOC(e.setReturnInverted(false));
}
