// CAL controller (VU GUI plan Task 6, spec 2026-11-02 §2/§7): SEND CAL to +4 dBu, per-channel RETURN unity, refusals, aborts.
// The fake port models the loop: return = tone · gain_ch, delayed by physLoop; captured() is advanced by the test.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include "engine/level_cal.hpp"
#include "plugin/cal_controller.hpp"

using namespace ni::plugin;
using Catch::Matchers::WithinAbs;
namespace cal = ni::engine::cal;

namespace
{
struct FakePort final : CalPort
{
    const float* tone = nullptr; int toneN = 0; bool on = false; int starts = 0;
    int elapsed = 0, loop = 300;
    float gainDb[2] { 0.0f, 0.0f };
    void startTone(const float* t, int n) override { tone = t; toneN = n; on = true; ++starts; elapsed = 0; }
    void stopTone() override { on = false; }
    int captured() const override { return elapsed; }
    bool capture(float* l, float* r, int n) override
    {
        const float g[2] = { std::pow(10.0f, gainDb[0] / 20.0f), std::pow(10.0f, gainDb[1] / 20.0f) };
        for (int i = 0; i < n; ++i)
        {
            const float x = i < loop ? 0.0f : tone[(i - loop) % toneN];
            l[i] = x * g[0]; r[i] = x * g[1];
        }
        return true;
    }
    int physLoop() const override { return loop; }
};

CalContext ctx()
{
    CalContext c {};
    c.effectMode = false; c.recording = false; c.pairOwned = true; c.measuring = false; c.sendStereo = true; c.retStereo = true;
    c.vuRefDbfs = -20.0f; c.sendDbuAt0dBFS = 24.0f; c.sendTrimDb = 0.0f; c.returnTrimDb = 0.0f;
    return c;
}

// runs a RETURN CAL to its end and returns the final status
CalStatus runReturn(CalController& cc, FakePort& p, const CalContext& c)
{
    REQUIRE(cc.pressReturn(c) == CalStatus::ReturnSettling);
    double t = 0.0;
    cc.tick(c, t);
    p.elapsed = p.loop + cal::kSettleSamples + cal::kWindowSamples / 2;
    t += 2.0;
    REQUIRE(cc.tick(c, t) == CalStatus::ReturnMeasuring);
    p.elapsed = p.loop + cal::kSettleSamples + cal::kWindowSamples;
    t += 2.0;
    return cc.tick(c, t);
}
} // namespace

TEST_CASE("cal_controller_refusals")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.effectMode = true;
    CHECK(cc.pressReturn(c) == CalStatus::RefusedEffect);   // SEND CAL works in Effect mode too (it only reads the track)
    c = ctx(); c.recording = true;
    CHECK(cc.pressReturn(c) == CalStatus::RefusedRecording);
    c = ctx(); c.pairOwned = false;
    CHECK(cc.pressSend(c) == CalStatus::RefusedNoPair);
    c = ctx(); c.measuring = true;
    CHECK(cc.pressReturn(c) == CalStatus::RefusedMeasuring);
    CHECK(p.starts == 0);
    CHECK_FALSE(cc.active());
    CHECK_FALSE(cc.takeResult().has_value());
}

// SEND CAL (user 2026-11-06): "it should put out the track material at +4 dBu adjusting the trim … If linked the adjustment
// by the calibration should affect the linked return" / "+0 … which should be +4 dB" — the track's own program, averaged over
// 3 s of signal, is set to 0 VU = +4 dBu at the converter: sendTrim = (4 − sendDbuAt0dBFS) − (program level before the trim).
// No tone, no mute. The send meter (AES17, after trim and offsets) arrives in CalContext::sendLevelDbfs on every tick.
namespace
{
// ticks every 20 ms for `seconds` with the given send meter levels; returns the last status
CalStatus feed(CalController& cc, CalContext& c, double& t, double seconds, float l, float r)
{
    CalStatus s = cc.status();
    for (double e = 0.0; e < seconds - 1e-9; e += 0.02)
    {
        c.sendLevelDbfs[0] = l; c.sendLevelDbfs[1] = r;
        t += 0.02;
        s = cc.tick(c, t);
        if (s != CalStatus::SendRunning) break;
    }
    return s;
}
} // namespace

TEST_CASE("cal_controller_send_cal_sets_the_program_to_plus4_dbu")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.sendTrimDb = -5.0f;   // +24 dBu alignment → +4 dBu = −20 dBFS
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    CHECK(cc.active());
    CHECK(p.starts == 0);                    // no tone: the track itself is measured, nothing is muted
    CHECK_FALSE(cc.takeResult().has_value());   // nothing is written before the program is measured
    CHECK(feed(cc, c, t, 2.9, -30.0f, -33.0f) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 0.2, -30.0f, -33.0f) == CalStatus::Done);
    const auto r = cc.takeResult();
    REQUIRE(r);
    CHECK_THAT(*r->sendTrimDb, WithinAbs(5.0, 0.01));   // the louder channel −30 at −5 → −20 at +5
    CHECK_FALSE(r->returnTrimDb.has_value());
    CHECK(p.starts == 0);
}

TEST_CASE("cal_controller_send_cal_follows_a_different_alignment")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.sendDbuAt0dBFS = 20.0f; c.sendTrimDb = 0.0f;   // Apollo headroom +20 dBu → +4 dBu = −16 dBFS
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    REQUIRE(feed(cc, c, t, 3.1, -22.0f, -22.0f) == CalStatus::Done);
    CHECK_THAT(*cc.takeResult()->sendTrimDb, WithinAbs(6.0, 0.01));
}

TEST_CASE("cal_controller_send_cal_averages_power_and_ignores_silence")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 5.0, -120.0f, -120.0f) == CalStatus::SendRunning);   // stopped transport: waits for the program
    CHECK(feed(cc, c, t, 1.5, -20.0f, -20.0f) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 1.6, -30.0f, -30.0f) == CalStatus::Done);
    // mean power of 1.5 s at −20 and 1.5 s at −30 (the 3 s window) = −22.6 dBFS → trim +2.6
    const double mean = 10.0 * std::log10(0.5 * (std::pow(10.0, -2.0) + std::pow(10.0, -3.0)));
    CHECK_THAT(*cc.takeResult()->sendTrimDb, WithinAbs(-20.0 - mean, 0.05));
}

TEST_CASE("cal_controller_send_cal_is_not_fooled_by_a_trim_move_while_measuring")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.sendTrimDb = -5.0f;
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    feed(cc, c, t, 1.5, -30.0f, -30.0f);
    c.sendTrimDb = 0.0f;   // the user turns the trim: the meter follows, the program before the trim stays −25
    REQUIRE(feed(cc, c, t, 1.6, -25.0f, -25.0f) == CalStatus::Done);
    CHECK_THAT(*cc.takeResult()->sendTrimDb, WithinAbs(5.0, 0.01));
}

TEST_CASE("cal_controller_send_cal_mono_reads_the_one_output")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.sendStereo = false;
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    REQUIRE(feed(cc, c, t, 3.1, -26.0f, -10.0f) == CalStatus::Done);   // R is not a send output for a mono send
    CHECK_THAT(*cc.takeResult()->sendTrimDb, WithinAbs(6.0, 0.01));
}

TEST_CASE("cal_controller_send_cal_out_of_range_writes_nothing")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 3.1, -45.0f, -45.0f) == CalStatus::ProgramOutOfRange);   // would need +25 dB, the trim stops at +12
    CHECK_FALSE(cc.takeResult().has_value());
    CHECK_FALSE(cc.active());
}

TEST_CASE("cal_controller_send_cal_times_out_without_program")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 29.0, -120.0f, -120.0f) == CalStatus::SendRunning);
    CHECK(feed(cc, c, t, 1.5, -120.0f, -120.0f) == CalStatus::NoProgram);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_send_cal_second_press_cancels")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    feed(cc, c, t, 1.0, -30.0f, -30.0f);
    CHECK(cc.pressSend(c) == CalStatus::Idle);
    CHECK_FALSE(cc.takeResult().has_value());
    CHECK_FALSE(cc.active());
}

TEST_CASE("cal_controller_send_cal_in_effect_mode")
{
    FakePort p; CalController cc(p);
    auto c = ctx(); c.effectMode = true;
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    REQUIRE(feed(cc, c, t, 3.1, -20.0f, -20.0f) == CalStatus::Done);
    CHECK_THAT(*cc.takeResult()->sendTrimDb, WithinAbs(0.0, 0.01));
}

TEST_CASE("cal_controller_send_cal_record_start_aborts")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    feed(cc, c, t, 1.0, -30.0f, -30.0f);
    c.recording = true;
    CHECK(cc.tick(c, t + 0.02) == CalStatus::RefusedRecording);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_return_cal_per_channel")
{
    FakePort p; CalController cc(p);
    p.gainDb[0] = -1.5f; p.gainDb[1] = 0.7f;
    const auto c = ctx();
    CHECK(runReturn(cc, p, c) == CalStatus::Done);
    CHECK_FALSE(p.on);
    const auto r = cc.takeResult();
    REQUIRE(r);
    REQUIRE(r->returnTrimDb);
    REQUIRE(r->returnOffsetDb);
    CHECK_FALSE(r->sendTrimDb.has_value());
    CHECK_THAT(*r->returnTrimDb, WithinAbs(0.4, 0.05));
    CHECK_THAT((*r->returnOffsetDb)[0], WithinAbs(1.1, 0.05));
    CHECK_THAT((*r->returnOffsetDb)[1], WithinAbs(-1.1, 0.05));
}

TEST_CASE("cal_controller_mono_uses_channel_l")
{
    FakePort p; CalController cc(p);
    p.gainDb[0] = -2.0f; p.gainDb[1] = -60.0f;
    auto c = ctx(); c.sendStereo = false; c.retStereo = false;
    CHECK(runReturn(cc, p, c) == CalStatus::Done);
    const auto r = cc.takeResult();
    REQUIRE(r);
    CHECK_THAT(*r->returnTrimDb, WithinAbs(2.0, 0.05));
    CHECK((*r->returnOffsetDb)[0] == 0.0f);
    CHECK((*r->returnOffsetDb)[1] == 0.0f);
}

TEST_CASE("cal_controller_return_clipped")
{
    FakePort p; CalController cc(p);
    p.gainDb[0] = p.gainDb[1] = 25.0f;
    CHECK(runReturn(cc, p, ctx()) == CalStatus::ReturnClipped);
    CHECK_FALSE(p.on);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_no_return")
{
    FakePort p; CalController cc(p);
    p.gainDb[0] = p.gainDb[1] = -80.0f;
    CHECK(runReturn(cc, p, ctx()) == CalStatus::NoReturn);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_out_of_range")
{
    FakePort p; CalController cc(p);
    p.gainDb[0] = p.gainDb[1] = -30.0f;
    CHECK(runReturn(cc, p, ctx()) == CalStatus::OutOfRange);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_record_start_aborts_cal")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    REQUIRE(cc.pressReturn(c) == CalStatus::ReturnSettling);
    p.elapsed = p.loop + cal::kSettleSamples + 100;
    REQUIRE(cc.tick(c, 1.0) == CalStatus::ReturnMeasuring);
    c.recording = true;
    CHECK(cc.tick(c, 1.1) == CalStatus::RefusedRecording);
    CHECK_FALSE(p.on);
    CHECK_FALSE(cc.active());
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_return_without_audio_times_out")
{
    FakePort p; CalController cc(p);
    const auto c = ctx();
    REQUIRE(cc.pressReturn(c) == CalStatus::ReturnSettling);
    cc.tick(c, 0.0);
    CHECK(cc.tick(c, 9.0) == CalStatus::ReturnSettling);
    CHECK(cc.tick(c, 15.0) == CalStatus::NoReturn);
    CHECK_FALSE(p.on);
}

TEST_CASE("cal_controller_cancel")
{
    FakePort p; CalController cc(p);
    REQUIRE(cc.pressReturn(ctx()) == CalStatus::ReturnSettling);
    cc.cancel();
    CHECK_FALSE(p.on);
    CHECK(cc.status() == CalStatus::Idle);
    CHECK_FALSE(cc.takeResult().has_value());
}

TEST_CASE("cal_controller_mono_send_stereo_return_uses_the_one_sent_level")
{
    // a mono send leaves on one output at sendTrim + sendOffset[0]; the R send offset does not exist for it, so both return
    // channels are measured against the same sent level (mono/stereo routing design 2026-11-05)
    FakePort p; CalController cc(p);
    p.gainDb[0] = p.gainDb[1] = -2.0f;
    auto c = ctx(); c.sendStereo = false; c.retStereo = true; c.sendOffsetDb[1] = 6.0f;
    CHECK(runReturn(cc, p, c) == CalStatus::Done);
    const auto r = cc.takeResult();
    REQUIRE(r);
    CHECK_THAT(*r->returnTrimDb, WithinAbs(2.0, 0.05));
    CHECK_THAT((*r->returnOffsetDb)[0], WithinAbs(0.0, 0.05));
    CHECK_THAT((*r->returnOffsetDb)[1], WithinAbs(0.0, 0.05));
}

TEST_CASE("cal_controller_send_cal_latency_measurement_aborts_it")
{
    FakePort p; CalController cc(p);
    auto c = ctx();
    double t = 0.0;
    REQUIRE(cc.pressSend(c) == CalStatus::SendRunning);
    feed(cc, c, t, 1.0, -30.0f, -30.0f);
    c.measuring = true;   // the send carries the latency clicks: they must not be averaged as program
    CHECK(cc.tick(c, t + 0.02) == CalStatus::RefusedMeasuring);
    CHECK_FALSE(cc.takeResult().has_value());
    CHECK(p.starts == 0);
}
