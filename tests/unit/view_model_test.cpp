// ViewModel (plan Task 25, F1 §12): state text + colour, physLoop/PDC texts, warnings, lead-in hint, failure reason strings.
#include <catch2/catch_test_macros.hpp>
#include <set>
#include "gui/view_model.hpp"

using namespace ni::gui;
using ni::engine::Reason;
using ni::engine::State;

namespace
{
ViewInputs base()
{
    ViewInputs in;
    in.deviceName = "Pultec"; in.interfaceName = "Apollo x8"; in.outName = "Out 3"; in.inName = "In 3";
    in.state = State::Locked; in.physLoop = 128; in.reportedPdc = 384; in.sampleRate = 48000;
    return in;
}
} // namespace

TEST_CASE("view_model_state_text_and_colour")
{
    struct Row { State s; const char* text; uint32_t colour; };
    const Row rows[] = {
        { State::Locked, "Locked", kGreen }, { State::Stale, "Stale", kAmber },
        { State::Failed, "Failed", kRed }, { State::Unavailable, "Unavailable", kRed },
        { State::UnsupportedHost, "Unsupported host", kRed }, { State::UnsupportedSampleRate, "Unsupported sample rate", kRed },
        { State::Measuring, "Measuring", kBlue }, { State::Queued, "Queued", kBlue }, { State::ApplyingLatency, "Applying latency", kBlue },
        { State::Idle, "Idle", kGrey },
    };
    for (const Row& r : rows)
    {
        ViewInputs in = base();
        in.state = r.s;
        const ViewModel vm = ViewModel::from(in);
        INFO(r.text);
        REQUIRE(vm.stateText == r.text);
        REQUIRE(vm.stateColour == r.colour);
    }
    REQUIRE(kGreen == 0xFF3DBE6Cu);
    REQUIRE(kAmber == 0xFFE0A325u);
    REQUIRE(kRed == 0xFFD64545u);
    REQUIRE(kBlue == 0xFF3B82F6u);
}

TEST_CASE("view_model_phys_loop_and_pdc_text")
{
    const ViewModel vm = ViewModel::from(base());
    REQUIRE(vm.physLoopText == "128 smp · 2.67 ms");
    REQUIRE(vm.pdcText == "384 smp · 8.00 ms");
    REQUIRE(vm.pathText == "Apollo x8 · Out 3 → In 3");
}

TEST_CASE("view_model_unmeasured_shows_dash")
{
    ViewInputs in = base();
    in.state = State::Queued; in.physLoop = -1;
    REQUIRE(ViewModel::from(in).physLoopText == "—");
}

TEST_CASE("view_model_anticipative_warning_names_the_track")
{
    ViewInputs in = base();
    in.anticipativeViolationTracks = { "Bass" };
    const ViewModel vm = ViewModel::from(in);
    REQUIRE(vm.warnings.size() == 1);
    REQUIRE(vm.warnings[0] == "Anticipative FX re-enabled on Bass");
    REQUIRE(vm.showReapplyAnticipative);
}

TEST_CASE("view_model_lead_in_hint")
{
    ViewInputs in = base();
    in.gapSamples = 384; in.preRollLeadIn = false;
    REQUIRE(ViewModel::from(in).showLeadInHint);
    in.preRollLeadIn = true;
    REQUIRE_FALSE(ViewModel::from(in).showLeadInHint);
    in.preRollLeadIn = false; in.gapSamples = 0;
    REQUIRE_FALSE(ViewModel::from(in).showLeadInHint);
}

TEST_CASE("view_model_failure_reason_text_for_every_reason")
{
    std::set<std::string> seen;
    for (int r = static_cast<int>(Reason::ProjectLoad); r <= static_cast<int>(Reason::UserRequest); ++r)
    {
        const std::string t = reasonText(static_cast<Reason>(r));
        INFO(r);
        REQUIRE_FALSE(t.empty());
        seen.insert(t);
    }
    REQUIRE(seen.size() == 27);   // 27 distinct strings
    REQUIRE(reasonText(Reason::None).empty());
    ViewInputs in = base();
    in.state = State::Failed; in.reason = Reason::NoReturn;
    REQUIRE(ViewModel::from(in).failureReason == reasonText(Reason::NoReturn));
    in.state = State::Locked;
    REQUIRE(ViewModel::from(in).failureReason.empty());
}

TEST_CASE("view_model_other_warnings")
{
    ViewInputs in = base();
    in.conflictWith = "Vox";
    in.renderLeftAt = 1;
    in.staleSnapshot = true;
    const ViewModel vm = ViewModel::from(in);
    REQUIRE(vm.warnings.size() == 3);
    REQUIRE(vm.warnings[0] == "Device in use by Vox");
    REQUIRE(vm.warnings[1] == "Render speed left at 1");
    REQUIRE(vm.warnings[2] == "The library record differs from this project's snapshot (kept)");
}

TEST_CASE("view_model_library_warning_is_shown_while_audio_runs")
{
    // F1 §21: a missing/corrupt/read-only library runs with the in-project snapshot and says so; never blocks audio
    ViewInputs in = base();
    in.state = State::Locked;
    in.libraryWarning = "devices.json unreadable";
    const ViewModel vm = ViewModel::from(in);
    REQUIRE(vm.warnings.size() == 1);
    REQUIRE(vm.warnings[0] == "Device library: devices.json unreadable");
}

TEST_CASE("view_model_host_version_label")
{
    // F1 §15: any host other than a validated one (7.83; 7.85 by the 2026-11-06 Apollo gate) runs but is marked; a validated
    // version (any arch suffix) shows no label (f3_11 INFO row).
    ViewInputs in = base();
    in.hostVersion = "7.83/macOS-arm64";
    REQUIRE(ViewModel::from(in).warnings.empty());
    in.hostVersion = "7.85/macOS-arm64";
    REQUIRE(ViewModel::from(in).warnings.empty());
    in.hostVersion = "7.86/macOS-arm64";      // newer than anything gated: marked
    REQUIRE(ViewModel::from(in).warnings.size() == 1);
    in.hostVersion = "7.850";                 // not 7.85
    REQUIRE(ViewModel::from(in).warnings.size() == 1);
    in.hostVersion = "7.84/macOS-arm64";
    auto vm = ViewModel::from(in);
    REQUIRE(vm.warnings.size() == 1);
    REQUIRE(vm.warnings[0] == "Unvalidated host version 7.84/macOS-arm64");
    in.hostVersion = "7.830";                 // not 7.83: prefix must end at the version boundary
    REQUIRE(ViewModel::from(in).warnings.size() == 1);
    in.hostVersion = "";                      // unknown (no REAPER API): no label, the state already says UnsupportedHost
    REQUIRE(ViewModel::from(in).warnings.empty());
}

// Task 25 screenshots (Apollo -s9shot): the editor header read "NovaInsert Â· Insert" — a UTF-8 literal went through
// juce::String (const char*) (ASCII). The title is built here as UTF-8 and the editor draws it through the std::string path.
TEST_CASE("view_model_title_is_utf8_text_with_the_mode")
{
    auto in = base();
    CHECK(ViewModel::from(in).titleText == "NovaInsert  \xC2\xB7  Insert");
    in.mode = ni::engine::Mode::Effect;
    CHECK(ViewModel::from(in).titleText == "NovaInsert  \xC2\xB7  Effect");
}

// Task 25 screenshots: with no device selected the header kept the previous pair's route and "measured <time>"
TEST_CASE("view_model_no_device_shows_no_route_or_measurement_time")
{
    auto in = base();
    in.timestampUnix = 1793604201;
    CHECK(!ViewModel::from(in).timestampText.empty());
    in.deviceName.clear(); in.state = State::Idle; in.physLoop = -1; in.reportedPdc = -1;
    const auto vm = ViewModel::from(in);
    CHECK(vm.deviceName == "No device");
    CHECK(vm.pathText == "No device selected");
    CHECK(vm.timestampText.empty());
}

// ---- VU GUI plan Task 8: CAL copy, converter scales, device-in-use list ------------------------------------------------------
TEST_CASE("view_model_cal_text")
{
    const std::vector<std::pair<std::string, bool>> want = {
        { "", false },
        { "SEND CAL: play the track — 3 s of it set to 0 VU = +4 dBu (press again to cancel)", false },
        { "Measuring return…", false },
        { "Measuring return…", false },
        { "Calibrated", false },
        { "CAL needs Insert mode", true },
        { "CAL is not available while recording", true },
        { "Select a free device first", true },
        { "Wait for the latency measurement", true },
        { "Return clipped: lower the outboard output", true },
        { "No return signal: check the outboard and cabling", true },
        { "Loop gain outside the trim range", true },
        { "SEND CAL: no track signal within 30 s — start playback, then press CAL", true },
        { "SEND CAL: the track is too quiet or too loud for the trim range", true },
    };
    for (int s = 0; s < static_cast<int>(want.size()); ++s)
    {
        ViewInputs in = base();
        in.calStatus = s;
        const auto vm = ViewModel::from(in);
        CHECK(vm.calText == want[static_cast<size_t>(s)].first);
        CHECK(vm.calError == want[static_cast<size_t>(s)].second);
    }
}

TEST_CASE("view_model_sub_scale_offsets")
{
    // one unit for both meters, dBu by default (user 2026-11-05): 0 VU = +4 dBu at each converter; in dBFS both sit at the
    // SETUP reference
    ViewInputs in = base();
    in.sendDbuAt0dBFS = 24.0f; in.returnDbuAt0dBFS = 20.2f; in.vuRefDbfs = -18.0f;
    const auto vm = ViewModel::from(in);
    CHECK(vm.sendSubScaleOffset == -20.0f);
    CHECK(std::fabs(vm.returnSubScaleOffset - (-16.2f)) < 1e-5f);
    CHECK(vm.sendScaleLabel == "SEND · 0 VU = +4 dBu");
    CHECK(vm.returnScaleLabel == "RETURN · 0 VU = +4 dBu");
    in.meterDbu = false;
    const auto fs = ViewModel::from(in);
    CHECK(fs.sendSubScaleOffset == -18.0f);
    CHECK(fs.returnSubScaleOffset == -18.0f);
    CHECK(fs.sendScaleLabel == "SEND · 0 VU = −18 dBFS");
    CHECK(fs.returnScaleLabel == "RETURN · 0 VU = −18 dBFS");
    in.vuRefDbfs = -20.0f;
    CHECK(ViewModel::from(in).returnScaleLabel == "RETURN · 0 VU = −20 dBFS");
}

TEST_CASE("view_model_level_text_follows_the_meter_unit")
{
    CHECK(ViewModel::levelText(-20.0f, true, 24.0f) == "+4.0 dBu");
    CHECK(ViewModel::levelText(-20.0f, false, 24.0f) == "−20.0 dBFS");
    CHECK(ViewModel::levelText(-140.0f, true, 24.0f) == "—");
}

TEST_CASE("view_model_device_list_in_use")
{
    ViewInputs in = base();
    in.devices = { { "SSL EQ stereo", "Vox" }, { "SSL EQ 1 mono", "" } };
    const auto vm = ViewModel::from(in);
    REQUIRE(vm.deviceItems.size() == 2);
    CHECK(vm.deviceItems[0] == std::make_pair(std::string("SSL EQ stereo — in use: Vox"), false));
    CHECK(vm.deviceItems[1] == std::make_pair(std::string("SSL EQ 1 mono"), true));
}

TEST_CASE("view_model_dbu_and_dbfs_text")
{
    CHECK(ViewModel::dbuText(-20.0f, 24.0f) == "+4.0 dBu");
    CHECK(ViewModel::dbuText(-30.0f, 24.0f) == "−6.0 dBu");
    CHECK(ViewModel::dbuText(-140.0f, 24.0f) == "—");
    CHECK(ViewModel::dbfsText(-20.0f) == "−20.0 dBFS");
    CHECK(ViewModel::dbfsText(-140.0f) == "—");
}

TEST_CASE("view_model_defaults_changed_is_a_note")
{
    ViewInputs in = base();
    in.defaultsChanged = true;
    const auto vm = ViewModel::from(in);
    CHECK(vm.noteText == "Device defaults changed");
    for (const auto& w : vm.warnings) CHECK(w.find("defaults") == std::string::npos);
    in.defaultsChanged = false;
    CHECK(ViewModel::from(in).noteText.empty());
}

TEST_CASE("view_model_channel_label")
{
    REQUIRE(ViewModel::channelLabel(2, "Apollo Out 3") == "3 · Apollo Out 3");
    REQUIRE(ViewModel::channelLabel(2, "") == "3");
}

TEST_CASE("view_model_progress_text_says_the_output_is_muted_until_locked")
{
    // user 2026-11-05: after a device change the track is silent until the loop locks — the editor must say so
    ViewInputs in = base();
    in.state = State::Measuring;
    REQUIRE(ViewModel::from(in).progressText == "Latency check in progress — output muted until the loop is locked");
    in.state = State::Queued;
    REQUIRE(ViewModel::from(in).progressText == "Latency check queued — output muted until the loop is locked");
    in.state = State::ApplyingLatency;
    REQUIRE(ViewModel::from(in).progressText == "Latency measured — output muted until REAPER applies the delay (play once)");
    for (State s : { State::Locked, State::Idle, State::Failed, State::Stale })
    {
        in.state = s;
        REQUIRE(ViewModel::from(in).progressText.empty());
    }
}

TEST_CASE("view_model_open_fallback_is_a_warning_naming_the_failure")
{
    ViewInputs in = base();
    in.state = State::ApplyingLatency;
    in.openFallback = true;
    in.openFallbackReason = Reason::NoReturn;
    const auto vm = ViewModel::from(in);
    bool found = false;
    for (const auto& w : vm.warnings)
        if (w == "Open check failed: " + reasonText(Reason::NoReturn) + " \u2014 saved latency in use") found = true;
    REQUIRE(found);
    for (int r = 0; r <= static_cast<int>(Reason::UserRequest); ++r)   // fits the status line with the longest reason (≤ 90 chars)
    {
        in.openFallbackReason = static_cast<Reason>(r);
        for (const auto& w : ViewModel::from(in).warnings)
            if (w.rfind("Open check failed", 0) == 0) {
                size_t chars = 0;
                for (unsigned char c : w) chars += (c & 0xC0) != 0x80;   // UTF-8 code points
                REQUIRE(chars <= 90);
            }
    }
    in.openFallback = false;
    for (const auto& w : ViewModel::from(in).warnings) REQUIRE(w.find("saved latency") == std::string::npos);
}

// RE-CALIBRATION (user 2026-11-06): the button under SETUP is live only when the calibration failed — a Failed instance or a
// failed open check running on the saved latency — never while a check runs or a good lock holds
TEST_CASE("view_model_recalibrate_enabled_only_after_a_failed_calibration")
{
    ViewInputs in = base();
    in.state = State::Failed; in.reason = Reason::NoReturn;
    REQUIRE(ViewModel::from(in).recalibrateEnabled);
    in.state = State::Locked; in.openFallback = true; in.openFallbackReason = Reason::LowSnr;
    REQUIRE(ViewModel::from(in).recalibrateEnabled);
    in.state = State::ApplyingLatency;   // the fallback latency still waiting for REAPER: re-calibration may replace it
    REQUIRE(ViewModel::from(in).recalibrateEnabled);
    in.state = State::Measuring;          // a check is running: nothing to restart
    REQUIRE_FALSE(ViewModel::from(in).recalibrateEnabled);
    in.state = State::Locked; in.openFallback = false;
    REQUIRE_FALSE(ViewModel::from(in).recalibrateEnabled);
    for (State s : { State::Idle, State::Queued, State::Measuring, State::ApplyingLatency, State::Stale, State::Unavailable,
                     State::UnsupportedHost, State::UnsupportedSampleRate })
    {
        in.state = s;
        INFO(static_cast<int>(s));
        REQUIRE_FALSE(ViewModel::from(in).recalibrateEnabled);
    }
    in.state = State::Failed; in.deviceName.clear();   // no device: nothing to measure
    REQUIRE_FALSE(ViewModel::from(in).recalibrateEnabled);
}

TEST_CASE("measure_request_from_failed_only_for_the_user_recalibration")
{
    using ni::engine::measureRequestAllowed;
    REQUIRE(measureRequestAllowed(State::Failed, Reason::UserRequest));
    REQUIRE_FALSE(measureRequestAllowed(State::Failed, Reason::RoutingChange));   // automatic triggers leave Failed alone, as before
    REQUIRE(measureRequestAllowed(State::Locked, Reason::UserRequest));          // open-check fallback runs Locked
    for (State s : { State::Idle, State::Queued, State::Locked, State::Stale, State::Measuring, State::ApplyingLatency })
        REQUIRE(measureRequestAllowed(s, Reason::BufferChange));
    for (State s : { State::Unavailable, State::UnsupportedHost, State::UnsupportedSampleRate })
        REQUIRE_FALSE(measureRequestAllowed(s, Reason::UserRequest));
    REQUIRE(ni::gui::reasonText(Reason::UserRequest) == "Re-calibration requested");
}
