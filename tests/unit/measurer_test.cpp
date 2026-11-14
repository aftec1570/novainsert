// Measurer (plan Task 15, F1 §7 attempts/lock, §8 levels + mute window): adaptive level probe → 3 latency probes →
// evaluateLock, one automatic retry, 15 s / 30 s timeouts, record abort, stale-generation drop, stereo asymmetry.
// Fake engine (ProbeIo) returns the armed probe delayed by D at gain g; fake worker runs the real Correlator on demand.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include "plugin/measurer.hpp"
#include "../rt/alloc_guard.hpp"

using namespace ni::engine;
using ni::plugin::CorrJob;
using ni::plugin::CorrelationWorker;
using ni::plugin::Measurer;
using ni::plugin::ProbeIo;

namespace
{
const ProbeGenerator& probe()
{
    static const ProbeGenerator g { ProbeSpec {} };
    return g;
}

Correlator& correlator()
{
    static Correlator c { probe() };
    return c;
}

struct FakeIo final : ProbeIo
{
    int32_t D[2] { 1000, 1000 };
    float split[2] { 0.0f, 0.0f };          // fractional delay: this share of the click arrives one sample later (D + 1)
    float gainDb = 0.0f;
    bool noiseOnly = false;
    // Transport playing: the send carries loud program (about −12 dBFS) whenever no probe is armed, and that program comes
    // back D samples later, exactly as the loop returns it while a probe is latched (it is still in flight).
    bool playing = false;
    std::optional<int64_t> progEnd;           // the hook sample the send stopped carrying program (first arm while disarmed)
    int64_t armAt = 0;

    int64_t sample = 0;                       // hook sample clock
    const float* armed = nullptr;
    int32_t armedN = 0, captured = 0;
    struct Arm { int64_t at; float peakDb; };
    std::vector<Arm> arms;
    struct Mute { int64_t at; bool on; };
    std::vector<Mute> mutes;
    std::optional<int64_t> stopAt;

    FakeIo() { arms.reserve(64); mutes.reserve(64); }

    void setMuted(bool m) noexcept override { mutes.push_back({ sample, m }); }
    void setProbe(const float* p, int32_t n) noexcept override
    {
        if (p == nullptr || n <= 0)
        {
            if (armed != nullptr) stopAt = sample;
            armed = nullptr; armedN = 0; captured = 0; progEnd.reset();   // disarmed: the program goes out again
            return;
        }
        if (armed == nullptr && ! progEnd) progEnd = sample;
        float pk = 0.0f;
        for (int32_t i = 0; i < n; ++i) pk = std::max(pk, std::fabs(p[i]));
        arms.push_back({ sample, 20.0f * std::log10(pk) });
        armed = p; armedN = n; captured = 0; armAt = sample;
    }
    int32_t capturedCount() const noexcept override { return armed != nullptr ? captured : 0; }
    bool captureReturn(float* l, float* r, int32_t n) noexcept override
    {
        if (armed == nullptr || captured < n) return false;
        fill(l, n, D[0], 0u);
        if (r != nullptr) fill(r, n, D[1], 1u);
        return true;
    }
    void advance(int32_t n) { sample += n; if (armed != nullptr) captured = std::min(captured + n, SlotLoopEngine::kCaptureCap); }

private:
    void fill(float* dst, int32_t n, int32_t d, unsigned ch) const
    {
        const float g = std::pow(10.0f, gainDb / 20.0f);
        for (int32_t i = 0; i < n; ++i)
        {
            if (noiseOnly)
            {
                uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ (static_cast<uint32_t>(arms.size()) * 40503u + ch * 977u);
                h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                dst[i] = 0.03f * (static_cast<float>(h & 0xffffu) / 32768.0f - 1.0f);
            }
            else
            {
                const float f = split[ch];
                const float now = (i >= d && i - d < armedN) ? armed[i - d] : 0.0f;
                const float late = (i >= d + 1 && i - d - 1 < armedN) ? armed[i - d - 1] : 0.0f;
                dst[i] = ((1.0f - f) * now + f * late) * g;
            }
            if (playing && ! noiseOnly && (! progEnd || armAt + i - d < *progEnd))   // program sent before the send went silent
            {
                uint32_t h = static_cast<uint32_t>(armAt + i - d) * 2246822519u ^ (ch * 3266489917u);
                h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
                dst[i] += 0.25f * (static_cast<float>(h & 0xffffu) / 32768.0f - 1.0f) * g;
            }
        }
    }
};

struct FakeWorker final : CorrelationWorker
{
    Correlator* corr = &correlator();
    bool answer = true;
    std::optional<CorrJob> held;
    int submitted = 0;
    void submit(const CorrJob& j) noexcept override { held = j; ++submitted; }
    // Delivers the held job's result (message thread), as the production worker does through callAsync.
    void pump(Measurer& m)
    {
        if (! answer || ! held) return;
        const auto j = *held;
        held.reset();
        const auto l = corr->analyse(j.l, j.n, j.hwBlock);
        const auto r = j.r != nullptr ? corr->analyse(j.r, j.n, j.hwBlock) : l;
        m.onCorrelation(j.token, l, r);
    }
};

struct Rig
{
    static constexpr int B = 128;
    FakeIo io;
    FakeWorker w;
    int64_t now = 0;
    Measurer m { io, w, probe(), [this] { return now; } };
    struct Got { uint32_t gen; CalibResult r; };
    std::vector<Got> got;

    Rig() { got.reserve(8); m.onResult = [this](uint32_t g, const CalibResult& r) { got.push_back({ g, r }); }; }
    void step() { io.advance(48 * 20); w.pump(m); now += 20; m.tick(); }
    void run(int64_t ms) { for (int64_t t = 0; t < ms; t += 20) step(); }
    bool runUntilResult(int64_t maxMs = 40000)
    {
        for (int64_t t = 0; t < maxMs && got.empty(); t += 20) step();
        return ! got.empty();
    }
};

LevelHistory unknown() { return { false, 0.0f, 0.0f }; }
} // namespace

TEST_CASE("measurer_locks_in_one_attempt_D_1000_gain_0dB")
{
    Rig r;
    r.m.start(7, unknown(), false, Rig::B);
    REQUIRE(r.runUntilResult());
    r.run(2000);
    REQUIRE(r.got.size() == 1);
    const auto& res = r.got[0].r;
    REQUIRE(r.got[0].gen == 7);
    REQUIRE(res.failReason == Reason::None);
    REQUIRE(res.physLoop[0] == 1000);
    REQUIRE(res.reportedPdc == 1000 + 2 * Rig::B);
    REQUIRE(res.generation == 7);
    REQUIRE(std::fabs(res.gainPeakDb) < 0.5f);
    REQUIRE(r.io.arms.size() == 4);   // level probe + 3 latency probes, one attempt
    REQUIRE(std::fabs(r.io.arms[0].peakDb - -36.0f) < 0.01f);
    for (size_t i = 1; i < 4; ++i) REQUIRE(std::fabs(r.io.arms[i].peakDb - -24.0f) < 0.01f);   // +12 dB step cap (ruling)
    REQUIRE(r.io.armed == nullptr);
    REQUIRE_FALSE(r.m.busy());
}

TEST_CASE("measurer_two_attempts_then_failed")
{
    Rig r;
    r.io.noiseOnly = true;
    r.m.start(3, unknown(), false, Rig::B);
    int maxAttempt = 0;
    for (int64_t t = 0; t < 40000 && r.got.empty(); t += 20) { r.step(); maxAttempt = std::max(maxAttempt, r.m.progress().attempt); }
    REQUIRE(r.got.size() == 1);
    REQUIRE(maxAttempt == 2);
    REQUIRE(r.io.arms.size() == 2);   // no arrival in the level probe's window ends each attempt at once (2026-11-05 ruling)
    const auto why = r.got[0].r.failReason;
    REQUIRE((why == Reason::LowSnr || why == Reason::NoReturn));
    r.run(5000);
    REQUIRE(r.got.size() == 1);
}

TEST_CASE("measurer_attempt_timeout_15s")
{
    Rig r;
    r.w.answer = false;
    r.m.start(1, unknown(), false, Rig::B);
    r.run(14900);
    REQUIRE(r.m.progress().attempt == 1);
    r.run(200);
    REQUIRE(r.m.progress().attempt == 2);
    r.run(3500);
    REQUIRE(r.io.arms.size() == 2);   // attempt 2's level probe waits for attempt 1's return to drain (postSec + L_max)
    const auto& p = probe();
    REQUIRE(r.io.arms[1].at >= 15000 * 48 + (p.length() - p.sweepStart() - p.sweepLength() + kLMax));
    r.run(11200);
    REQUIRE(r.got.empty());
    r.run(400);
    REQUIRE(r.got.size() == 1);
    REQUIRE(r.got[0].r.failReason == Reason::Timeout);
    REQUIRE_FALSE(r.m.busy());
}

TEST_CASE("measurer_record_abort_within_one_block")
{
    Rig r;
    r.m.start(2, unknown(), false, Rig::B);
    while (r.io.arms.size() < 2) r.step();   // probe 2 (first latency probe) armed
    r.run(500);
    const int64_t abortAt = r.io.sample;
    r.m.abortForRecording();
    REQUIRE(r.io.stopAt.has_value());
    REQUIRE(*r.io.stopAt <= abortAt + Rig::B);   // disarmed now; the engine applies it at the next block boundary
    REQUIRE(r.io.armed == nullptr);
    REQUIRE_FALSE(r.m.busy());
    REQUIRE(r.m.lastReason() == Reason::Recording);
    r.run(20000);
    REQUIRE(r.got.empty());
    REQUIRE(r.io.arms.size() == 2);
    REQUIRE_FALSE(r.io.mutes.empty());
    REQUIRE_FALSE(r.io.mutes.back().on);   // unmuted after the return-discard drain
    r.m.start(2, unknown(), false, Rig::B);   // restart allowed
    REQUIRE(r.runUntilResult());
    REQUIRE(r.got[0].r.failReason == Reason::None);
    REQUIRE(r.got[0].r.physLoop[0] == 1000);
}

TEST_CASE("measurer_stale_generation_result_dropped")
{
    Rig r;
    r.w.answer = false;
    r.m.start(10, unknown(), false, Rig::B);
    while (! r.w.held) r.step();   // level-probe job for gen 10 in flight
    r.m.cancel(Reason::KeyChange);
    r.m.start(11, unknown(), false, Rig::B);
    r.w.answer = true;            // the stale gen-10 result arrives now, after the restart
    REQUIRE(r.runUntilResult());
    r.run(2000);
    REQUIRE(r.got.size() == 1);
    REQUIRE(r.got[0].gen == 11);
    REQUIRE(r.got[0].r.failReason == Reason::None);
}

TEST_CASE("measurer_mute_entry_exit_separate")
{
    Rig r;
    r.m.start(4, unknown(), false, Rig::B);
    REQUIRE(r.runUntilResult());
    REQUIRE(r.io.mutes.size() == 2);
    REQUIRE(r.io.mutes[0].on);
    REQUIRE(r.io.mutes[0].at <= r.io.arms.front().at);
    REQUIRE_FALSE(r.io.mutes[1].on);
    const auto& p = probe();
    // unmuted once the last probe's window has closed: its return arrived (D) and the guard passed (2026-11-05 ruling)
    const int64_t lastReturnEnd = r.io.arms.back().at + p.sweepStart() + 1000 + p.sweepLength() + Measurer::kGuardSamples;
    REQUIRE(r.io.mutes[1].at >= lastReturnEnd);
    REQUIRE(r.io.mutes[1].at <= lastReturnEnd + 3 * 960);
}

TEST_CASE("measurer_stereo_asymmetry_fails")
{
    Rig r;
    r.io.D[1] = 1001;
    r.m.start(6, unknown(), true, Rig::B);
    REQUIRE(r.runUntilResult());
    const auto& res = r.got[0].r;
    REQUIRE(res.failReason == Reason::ChannelAsymmetry);
    REQUIRE(res.physLoop[0] == 1000);
    REQUIRE(res.physLoop[1] == 1001);
}

TEST_CASE("measurer_stereo_half_sample_straddle_locks_on_the_common_lag")
{
    // L at about 1000.14, R at about 1000.57: R's whole-sample peak is 1001, yet both are one loop (SSL EQ R at 872.5)
    Rig r;
    r.io.split[0] = 0.3f; r.io.split[1] = 0.52f;
    r.m.start(6, unknown(), true, Rig::B);
    REQUIRE(r.runUntilResult());
    const auto& res = r.got[0].r;
    REQUIRE(res.failReason == Reason::None);
    REQUIRE(res.physLoop[0] == 1000);
    REQUIRE(res.physLoop[1] == 1000);
    REQUIRE(res.fractional[1] > 0.5f);   // R's residual against the common lag is kept for the diagnostics
}

TEST_CASE("measurer_stereo_lag_rule")
{
    REQUIRE(Measurer::stereoLag(872, 872.1, 872, 872.4) == 872);
    REQUIRE(Measurer::stereoLag(872, 872.106, 873, 872.501) == 872);   // the Apollo SSL EQ result of 2026-11-14
    REQUIRE(Measurer::stereoLag(873, 872.6, 872, 872.4) == 873);       // mean 872.5 rounds away from zero
    REQUIRE(Measurer::stereoLag(1000, 1000.0, 1001, 1001.0) == -1);    // a whole sample of skew is a different loop
    REQUIRE(Measurer::stereoLag(1000, 1000.2, 1002, 1001.6) == -1);    // two samples apart, never one loop
    REQUIRE(Measurer::stereoLag(1000, 1000.0, 1001, 1000.51) == -1);   // skew 0.51 > kStereoSkewMax
}

TEST_CASE("measurer_stereo_symmetric_locks")
{
    Rig r;
    r.io.D[0] = r.io.D[1] = 2500;
    r.m.start(6, unknown(), true, Rig::B);
    REQUIRE(r.runUntilResult());
    REQUIRE(r.got[0].r.failReason == Reason::None);
    REQUIRE(r.got[0].r.physLoop[0] == 2500);
    REQUIRE(r.got[0].r.physLoop[1] == 2500);
}

TEST_CASE("measurer_no_allocation_on_message_thread_after_start")
{
    (void) correlator();   // the worker's correlator allocates once, in its constructor
    Rig r;
    r.m.start(8, unknown(), false, Rig::B);
    size_t inTick = 0, inPump = 0, inAdvance = 0;
    {
        AllocGuard g;
        for (int64_t t = 0; t < 40000 && r.got.empty(); t += 20)
        {
            size_t a = AllocGuard::count();
            r.io.advance(48 * 20);
            inAdvance += AllocGuard::count() - a; a = AllocGuard::count();
            r.w.pump(r.m);
            inPump += AllocGuard::count() - a; a = AllocGuard::count();
            r.now += 20;
            r.m.tick();
            inTick += AllocGuard::count() - a;
        }
    }
    REQUIRE(r.got.size() == 1);
    REQUIRE(inTick == 0);
    WARN("allocations during a full attempt: tick " << inTick << ", worker " << inPump << ", engine " << inAdvance);
}

TEST_CASE("measurer_single_attempt_mode_leaves_the_retry_to_the_scheduler")
{
    FakeIo io;
    io.noiseOnly = true;
    FakeWorker w;
    int64_t now = 0;
    Measurer m { io, w, probe(), [&now] { return now; }, 1 };
    int results = 0;
    Reason why = Reason::None;
    m.onResult = [&](uint32_t, const CalibResult& r) { ++results; why = r.failReason; };
    m.start(1, unknown(), false, 128);
    for (int64_t t = 0; t < 40000 && results == 0; t += 20) { io.advance(960); w.pump(m); now += 20; m.tick(); }
    REQUIRE(results == 1);
    REQUIRE(io.arms.size() == 1);   // one attempt: the level probe heard nothing, so the attempt ends there (2026-11-05 ruling)
    REQUIRE((why == Reason::LowSnr || why == Reason::NoReturn));
}

// ---- the click ping (user 2026-11-05: "3 seconds max, a single transient spike", as ReaInsert's ping does) ----
namespace
{
const ProbeGenerator& clickProbe() { static const ProbeGenerator g { ProbeSpec::click() }; return g; }
Correlator& clickCorr() { static Correlator c { clickProbe() }; return c; }
struct ClickRig
{
    FakeIo io;
    FakeWorker w;
    int64_t now = 0;
    Measurer m { io, w, clickProbe(), [this] { return now; } };
    std::vector<CalibResult> got;
    int64_t doneAt = -1;
    ClickRig() { w.corr = &clickCorr(); got.reserve(8); m.onResult = [this](uint32_t, const CalibResult& r) { got.push_back(r); doneAt = now; }; }
    void step() { io.advance(48 * 20); w.pump(m); now += 20; m.tick(); }
    bool runUntilResult(int64_t maxMs) { for (int64_t t = 0; t < maxMs && got.empty(); t += 20) step(); return ! got.empty(); }
};
} // namespace

TEST_CASE("measurer_click_ping_locks_in_well_under_a_second")
{
    for (int32_t D : { 300, 1000, 4500 })   // AD/DA round trips: small buffers .. 2048-sample buffers
    {
        CAPTURE(D);
        ClickRig r;
        r.io.D[0] = r.io.D[1] = D;
        r.m.start(7, unknown(), false, 128);
        REQUIRE(r.runUntilResult(3000));
        REQUIRE(r.got[0].failReason == Reason::None);
        REQUIRE(r.got[0].physLoop[0] == D);
        REQUIRE(r.io.arms.size() == 4);                            // level click + 3 agreeing clicks
        REQUIRE(r.doneAt <= 1000);                                  // the windows close once the click is back
        REQUIRE_FALSE(r.io.mutes.back().on);                       // unmuted at the lock: the click's return is in
        REQUIRE(r.io.mutes.back().at <= r.io.sample);
    }
}

TEST_CASE("measurer_click_loop_longer_than_the_window_fails_and_never_locks")
{
    ClickRig r;
    r.io.D[0] = r.io.D[1] = 30000;   // beyond the 0.45 s window: the level click's window ends without an arrival
    r.m.start(9, unknown(), false, 128);
    REQUIRE(r.runUntilResult(10000));
    REQUIRE(r.got[0].failReason == Reason::NoReturn);
    REQUIRE_FALSE(r.m.busy());
}

TEST_CASE("measurer_click_stereo_locks_both_channels")
{
    ClickRig r;
    r.io.D[0] = r.io.D[1] = 777;
    r.m.start(6, unknown(), true, 128);
    REQUIRE(r.runUntilResult(3000));
    REQUIRE(r.got[0].failReason == Reason::None);
    REQUIRE(r.got[0].physLoop[0] == 777);
    REQUIRE(r.got[0].physLoop[1] == 777);
    REQUIRE(r.doneAt <= 1000);
}

// Bug 2026-11-10 (Apollo ap_pdc_running_defer): a measurement started while the transport plays failed NoReturn twice. The
// program sent just before the probe latched was still in flight round the loop, landed in the probe's pre-silence and raised
// the arrival threshold above the click. While the program is live the Measurer silences the send and waits out the loop
// (L_max) before the probe, so no program can reach the probe's capture.
TEST_CASE("measurer_click_locks_while_the_transport_plays")
{
    for (int32_t D : { 300, 4500, 20000 })   // up to the click window (0.47 s)
    {
        CAPTURE(D);
        ClickRig r;
        r.io.D[0] = r.io.D[1] = D;
        r.io.playing = true;
        r.m.programLive = [] { return true; };
        r.m.start(8, unknown(), true, 128);
        REQUIRE(r.runUntilResult(6000));
        REQUIRE(r.got[0].failReason == Reason::None);
        REQUIRE(r.got[0].physLoop[0] == D);
        REQUIRE(r.got[0].physLoop[1] == D);
        REQUIRE(r.doneAt <= 4000);   // one drain (post-silence + L_max ≈ 1.8 s) + the stopped-transport click time
    }
}

TEST_CASE("measurer_click_silences_the_send_when_play_starts_during_the_settle")
{
    ClickRig r;
    r.io.D[0] = r.io.D[1] = 2000;
    bool live = false;
    r.m.programLive = [&live] { return live; };
    r.m.start(9, unknown(), false, 128);
    live = true; r.io.playing = true;   // play pressed before the first tick
    REQUIRE(r.runUntilResult(6000));
    REQUIRE(r.got[0].failReason == Reason::None);
    REQUIRE(r.got[0].physLoop[0] == 2000);
}
