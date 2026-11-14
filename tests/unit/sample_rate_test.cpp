// Rate independence (2026-11-14): 44.1 / 48 / 88.2 / 96 / 192 kHz are supported; every time constant that used to be a 48 kHz
// sample count now follows the device rate (meters, fade, CAL tone/settle/window, PDC window, measurer drain, GUI ms).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
#include "engine/level_cal.hpp"
#include "engine/meters.hpp"
#include "engine/pdc_observer.hpp"
#include "engine/probe.hpp"
#include "engine/slot_loop.hpp"
#include "engine/state_machine.hpp"
#include "gui/view_model.hpp"
#include "plugin/cal_controller.hpp"
#include "plugin/measurer.hpp"

using namespace ni::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE("supported sample rates are exactly 44.1, 48, 88.2, 96 and 192 kHz", "[rate]")
{
    for (const int fs : { 44100, 48000, 88200, 96000, 192000 }) REQUIRE(isSupportedSampleRate(fs));
    for (const int fs : { 0, 8000, 22050, 32000, 176400, 384000, -48000 }) REQUIRE_FALSE(isSupportedSampleRate(fs));
}

TEST_CASE("SampleRateChange is appended after ChannelNotDelivered and re-measures a lock", "[rate][types]")
{
    REQUIRE(static_cast<int>(Reason::SampleRateChange) == static_cast<int>(Reason::ChannelNotDelivered) + 1);
    REQUIRE(measureRequestAllowed(State::Locked, Reason::SampleRateChange));
    REQUIRE(measureRequestAllowed(State::Stale, Reason::SampleRateChange));
    REQUIRE(hostPollRequeue(State::Locked, Reason::SampleRateChange, Reason::None, false) == Reason::SampleRateChange);
    REQUIRE(hostPollRequeue(State::Idle, Reason::SampleRateChange, Reason::None, false) == Reason::SampleRateChange);
    REQUIRE(ni::gui::reasonText(Reason::SampleRateChange) == "Sample rate changed");
    REQUIRE(ni::gui::reasonText(Reason::SampleRate).find("44.1/48/88.2/96/192 kHz") != std::string::npos);
}

TEST_CASE("CAL tone is exactly 1 kHz, loops seamlessly and keeps its AES17 level at every rate", "[rate][cal]")
{
    const int fs = GENERATE(44100, 48000, 88200, 96000, 192000);
    INFO(fs);
    const int n = cal::toneLoopSamples(fs);
    REQUIRE(n == fs / 10);
    REQUIRE(n <= cal::kMaxToneLoopSamples);
    std::vector<float> two(static_cast<size_t>(2 * n));
    cal::makeTone(two.data(), 2 * n, -20.0f, fs);
    for (int i = 0; i < n; ++i) REQUIRE(two[static_cast<size_t>(i)] == two[static_cast<size_t>(i + n)]);   // loop boundary is seamless
    double s = 0.0; float pk = 0.0f;
    for (int i = 0; i < n; ++i) { s += static_cast<double>(two[size_t(i)]) * two[size_t(i)]; pk = std::max(pk, std::fabs(two[size_t(i)])); }
    REQUIRE_THAT(10.0 * std::log10(s / n) + cal::kAes17Db, WithinAbs(-20.0, 0.01));
    REQUIRE_THAT(20.0 * std::log10(pk), WithinAbs(-20.0, 0.05));
    // zero crossings rising: exactly 100 per 100 ms loop → 1 kHz
    int rising = 0;
    for (int i = 1; i < n; ++i) rising += two[size_t(i - 1)] < 0.0f && two[size_t(i)] >= 0.0f;
    REQUIRE(rising >= 99); REQUIRE(rising <= 100);
}

TEST_CASE("CAL settle and window: 48 kHz unchanged, higher rates fit the capture in whole tone loops", "[rate][cal]")
{
    constexpr int cap = ni::plugin::CalController::kCaptureCap;
    STATIC_REQUIRE(cap == SlotLoopEngine::kCaptureCap);
    REQUIRE(cal::settleSamples(48000) == 24000);
    REQUIRE(cal::windowSamples(48000, kLMax, cap) == 144000);   // the 1.2.1 values
    REQUIRE(cal::windowSamples(44100, kLMax, cap) == 132300);   // 3.0 s
    for (const int fs : { 44100, 48000, 88200, 96000, 192000 })
    {
        INFO(fs);
        const int w = cal::windowSamples(fs, kLMax, cap);
        REQUIRE(w > 0);
        REQUIRE(w % cal::toneLoopSamples(fs) == 0);
        REQUIRE(kLMax + cal::settleSamples(fs) + w <= cap);
        REQUIRE(static_cast<double>(w) / fs >= 0.5);   // never shorter than half a second (500 tone cycles)
        REQUIRE(static_cast<double>(w) / fs <= 3.0);
    }
}

TEST_CASE("meter ballistics are timed in seconds: 20 dB/s fall at 48 and 192 kHz alike", "[rate][meters]")
{
    const int fs = GENERATE(44100, 48000, 96000, 192000);
    INFO(fs);
    MeterEngine m; m.prepare(48000);
    m.setSampleRate(fs);                           // retuned by the feeding thread on the next feed
    REQUIRE(m.sampleRate() == fs);
    std::vector<float> one(1, 1.0f), zeros(static_cast<size_t>(fs / 10), 0.0f);
    m.feed(one.data(), one.data(), 1);
    for (int k = 0; k < 10; ++k) m.feed(zeros.data(), zeros.data(), fs / 10);   // 1 s of silence
    REQUIRE_THAT(m.read(-20.0f).peakDb[0], WithinAbs(-20.0, 0.1));
}

TEST_CASE("slot loop fade is 5 ms at every rate", "[rate][slot]")
{
    REQUIRE(SlotLoopEngine::fadeLenFor(48000) == 240);
    REQUIRE(SlotLoopEngine::fadeLenFor(44100) == 221);
    REQUIRE(SlotLoopEngine::fadeLenFor(88200) == 441);
    REQUIRE(SlotLoopEngine::fadeLenFor(96000) == 480);
    REQUIRE(SlotLoopEngine::fadeLenFor(192000) == 960);
    SlotLoopEngine e;
    REQUIRE(e.fadeLength() == 240);
    e.setSampleRate(96000);
    REQUIRE(e.fadeLength() == 480);
}

TEST_CASE("PDC observer window counts 8 hook cycles at the device rate", "[rate][pdc]")
{
    HostTransitionObserver o;
    o.begin(0, 100, 8192, true, 0, 44100);
    REQUIRE(o.windowMs() == (8LL * 8192 * 1000 + 44100 - 1) / 44100);   // 1487 ms
    o.begin(0, 100, 8192, true, 0, 192000);
    REQUIRE(o.windowMs() == 500);                                         // 342 ms → the 500 ms floor
    o.begin(0, 100, 8192, true, 0);                                       // default: 48 kHz (1366 ms)
    REQUIRE(o.windowMs() == (8LL * 8192 * 1000 + 48000 - 1) / 48000);
}

TEST_CASE("measurer drain time follows the rate (the click's sample counts are fixed)", "[rate][measurer]")
{
    const ProbeGenerator click { ProbeSpec::click() };
    const auto at48 = ni::plugin::Measurer::drainMsAt(click, 48000);
    const auto at96 = ni::plugin::Measurer::drainMsAt(click, 96000);
    const auto at441 = ni::plugin::Measurer::drainMsAt(click, 44100);
    REQUIRE(at48 == (static_cast<int64_t>(click.length() - click.sweepStart() - click.sweepLength()) + kLMax) * 1000 / 48000 + 1);
    REQUIRE(at96 < at48);
    REQUIRE(at441 > at48);   // longer at 44.1 kHz: 1.2.1's 48 kHz figure would close the drain 8 % early
    // the longest product capture (the click at any rate) fits the engine's capture buffer
    REQUIRE(click.length() <= SlotLoopEngine::kCaptureCap);
}

TEST_CASE("GUI readouts convert samples to ms at the device rate", "[rate][gui]")
{
    ni::gui::ViewInputs in;
    in.state = State::Locked; in.physLoop = 960; in.reportedPdc = 960; in.sampleRate = 96000;
    in.deviceName = "x"; in.interfaceName = "y";
    const auto vm = ni::gui::ViewModel::from(in);
    REQUIRE(vm.physLoopText.find("10.00 ms") != std::string::npos);
}
