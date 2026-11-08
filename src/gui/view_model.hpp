// ViewModel (plan Task 25, F1 §12): everything the editor shows, derived from plain inputs the processor snapshots on the
// message thread. Colours are ARGB (the editor wraps them in juce::Colour). No coordination state lives here.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include "engine/types.hpp"
#include "plugin/library.hpp"

namespace ni::gui
{
inline constexpr uint32_t kGreen = 0xFF3DBE6Cu;   // Locked
inline constexpr uint32_t kAmber = 0xFFE0A325u;   // Stale
inline constexpr uint32_t kRed = 0xFFD64545u;     // Failed / Unavailable / Unsupported*
inline constexpr uint32_t kBlue = 0xFF3B82F6u;    // Measuring / Queued / ApplyingLatency
inline constexpr uint32_t kGrey = 0xFF8A8F98u;    // Idle (no device)

inline std::string reasonText(engine::Reason r)
{
    using R = engine::Reason;
    switch (r)
    {
        case R::None: return {};
        case R::ProjectLoad: return "Project loaded";
        case R::StateRestore: return "Restored from the project";
        case R::RoutingChange: return "Routing changed";
        case R::DeviceSelection: return "Device selected";
        case R::IoChange: return "Audio I/O changed";
        case R::BufferChange: return "Audio buffer size changed";
        case R::DeviceReopen: return "Audio device reopened";
        case R::DeviceLost: return "Audio device lost";
        case R::DeviceReturned: return "Audio device returned";
        case R::KeyChange: return "Calibration key changed";
        case R::ConflictResolved: return "Device conflict resolved";
        case R::NoReturn: return "No signal returned from the hardware";
        case R::Clipped: return "The return clipped during the probe";
        case R::AmbiguousPeak: return "Ambiguous correlation peak (echo or crosstalk)";
        case R::LowSnr: return "Return too weak against the noise floor";
        case R::OutOfRange: return "Loop latency outside the supported range";
        case R::Timeout: return "The probe timed out";
        case R::PdcNotObserved: return "REAPER did not apply the new latency in time";
        case R::AnticipativeOn: return "Anticipative FX processing is on for this track";
        case R::SampleRate: return "Only 48 kHz is supported";
        case R::DefaultDevice: return "Select a specific audio device, not <default>";
        case R::PairConflict: return "Another instance uses this device";
        case R::Recording: return "Measurement paused while recording";
        case R::Crosstalk: return "Crosstalk from another loop during the probe";
        case R::LagMismatch: return "Repeated probes disagreed";
        case R::ChannelAsymmetry: return "Left and right loops differ";
        case R::UserRequest: return "Re-calibration requested";
    }
    return "Unknown reason";
}

// F1 §15: REAPER 7.83 is the validated host; any other version runs, labelled, until its matrix passes. Empty = unknown.
inline bool hostVersionValidated(const std::string& v)
{
    if (v.empty()) return true;
    // 7.83: F0..F5; 7.85: the Apollo gate of 2026-11-06 (measure on open, open fallback, render exact, library faults)
    for (const char* validated : {"7.83", "7.85"})
    {
        const std::string k = validated;
        if (v.compare(0, k.size(), k) == 0 && (v.size() == k.size() || v[k.size()] == '/')) return true;
    }
    return false;
}

// The "Interface" line of PRESET SETUP and SETUP: the audio device as REAPER reports it (GetAudioDeviceInfo IDENT_IN /
// IDENT_OUT), never a hardwired interface model (user 2026-11-08: "it should use the soundcard's name what REAPER reports").
// REAPER does not report the analog level, so the dBu-at-0-dBFS values are typed in.
inline std::string interfaceText(const std::string& identIn, const std::string& identOut)
{
    if (identIn.empty() && identOut.empty()) return "no audio device";
    if (identOut.empty() || identOut == identIn) return identIn.empty() ? identOut : identIn;
    if (identIn.empty()) return identOut;
    return identIn + " / " + identOut;   // separate input and output devices
}

// One device library entry for the device list: inUseBy = the track of another instance owning any of its channels (or empty).
struct DeviceListEntry
{
    std::string name;
    std::string inUseBy;
    bool operator==(const DeviceListEntry&) const = default;
};

struct ViewInputs
{
    std::string deviceName, interfaceName, outName, inName;
    std::string interfaceOutName;              // IDENT_OUT (interfaceName is IDENT_IN)
    engine::Mode mode = engine::Mode::Insert;
    engine::State state = engine::State::Idle;
    engine::Reason reason = engine::Reason::None;
    int32_t physLoop = -1, reportedPdc = -1;   // -1 = not measured
    int32_t driverIn = -1, driverOut = -1;     // GetInputOutputLatency (REFERENCE only)
    int32_t gapSamples = 0;                    // inherent start gap of a render
    int32_t sampleRate = engine::kSampleRate;
    int64_t timestampUnix = 0;
    bool preRollLeadIn = false;
    std::vector<std::string> anticipativeViolationTracks;
    std::string conflictWith;
    int renderLeftAt = -1;
    bool staleSnapshot = false;
    bool openFallback = false;                 // measure on open failed: the project's saved latency is in use (user 2026-11-06)
    engine::Reason openFallbackReason = engine::Reason::None;
    bool measureOnOpen = true;                 // SETUP toggle (global settings.json)
    int skin = 0;                              // SETUP skin: 0 Dark / 1 Ivory / 2 Silver (global settings.json, user 2026-11-06)
    bool reverseWheel = false;                 // SETUP: trim knobs turn the other way on the mouse wheel (global, GitHub #1)
    std::string libraryWarning;                // first library load warning (missing/corrupt/read-only); empty = none
    std::string hostVersion;                   // GetAppVersion(), e.g. "7.83/macOS-arm64"; empty = unknown
    // Level CAL / VU (VU GUI spec 2026-11-02)
    bool sendStereo = false, retStereo = false;        // effective widths (what the loop, meters and readouts run on)
    bool recSendStereo = false, recReturnStereo = false;   // record widths: a STEREO direction shows its STEREO | L | R selector
    bool link = false, defaultsChanged = false;
    float sendDbuAt0dBFS = 24.0f, returnDbuAt0dBFS = 24.0f, vuRefDbfs = -20.0f;
    std::optional<float> outboardMaxInDbu;
    bool meterDbu = true;                      // meter unit for BOTH sides (per-process rocker, user 2026-11-05): dBu, else dBFS
    int calStatus = 0;                         // plugin::CalStatus
    float sendTrimDb = 0.0f, returnTrimDb = 0.0f, sendOffsetDb[2] { 0.0f, 0.0f }, returnOffsetDb[2] { 0.0f, 0.0f };
    std::vector<DeviceListEntry> devices;
    // DEVICES page (spec 2026-11-05): the host's channel names (index order) and the library records the form edits
    std::vector<std::string> outChannelNames, inChannelNames;
    std::vector<plugin::DeviceRecord> records;
};

struct ViewModel
{
    std::string deviceName, modeText, titleText, pathText, stateText;   // titleText: UTF-8, drawn via juce::String (std::string)
    uint32_t stateColour = kGrey;
    std::string physLoopText, pdcText, driverText, gapText, timestampText;
    std::vector<std::string> warnings;
    std::string failureReason;
    bool showReapplyAnticipative = false, showLeadInHint = false;
    bool recalibrateEnabled = false;   // RE-CALIBRATION: only after a failed calibration (Failed, or the open check fell back) — user 2026-11-06
    // Level CAL / VU
    std::string calText;
    bool calError = false;
    std::string sendScaleLabel, returnScaleLabel;
    float sendSubScaleOffset = -20.0f, returnSubScaleOffset = -20.0f;   // dBFS sub-scale = VU + offset
    std::vector<std::pair<std::string, bool>> deviceItems;               // text, enabled
    std::string noteText;                                                 // neutral note (not a warning)
    std::string progressText;                                             // why the track is silent right now (user 2026-11-05)

    static std::string calStatusText(int s)
    {
        switch (s)
        {
            case 1: return "SEND CAL: play the track \u2014 3 s of it set to 0 VU = +4 dBu (press again to cancel)";
            case 2: case 3: return "Measuring return\u2026";
            case 4: return "Calibrated";
            case 5: return "CAL needs Insert mode";
            case 6: return "CAL is not available while recording";
            case 7: return "Select a free device first";
            case 8: return "Wait for the latency measurement";
            case 9: return "Return clipped: lower the outboard output";
            case 10: return "No return signal: check the outboard and cabling";
            case 11: return "Loop gain outside the trim range";
            case 12: return "SEND CAL: no track signal within 30 s \u2014 start playback, then press CAL";
            case 13: return "SEND CAL: the track is too quiet or too loud for the trim range";
            default: return {};
        }
    }

    // signed with U+2212 for minus; decimals = digits after the point
    static std::string signedText(float v, int decimals)
    {
        char b[32];
        std::snprintf(b, sizeof b, "%.*f", decimals, static_cast<double>(std::fabs(v)));
        const bool zero = std::string(b).find_first_not_of("0.") == std::string::npos;
        return (zero ? std::string() : v < 0 ? std::string("\u2212") : std::string("+")) + b;
    }
    static constexpr float kFloorDb = -120.0f;
    static std::string dbuText(float rmsDbfs, float dbuAt0dBFS)
    {
        return rmsDbfs <= kFloorDb ? "\u2014" : signedText(rmsDbfs + dbuAt0dBFS, 1) + " dBu";
    }
    // DEVICES page channel picker text: "3 · Apollo Out 3" (1-based index, host name when known)
    static std::string channelLabel(int index0, const std::string& name)
    {
        const std::string n = std::to_string(index0 + 1);
        return name.empty() ? n : n + " \u00b7 " + name;
    }
    static std::string dbfsText(float dbfs) { return dbfs <= kFloorDb ? "\u2014" : signedText(dbfs, 1) + " dBFS"; }
    static std::string levelText(float dbfs, bool dbu, float dbuAt0dBFS) { return dbu ? dbuText(dbfs, dbuAt0dBFS) : dbfsText(dbfs); }

    static std::string samplesText(int32_t smp, int32_t fs)
    {
        if (smp < 0 || fs <= 0) return "—";
        char b[64];
        std::snprintf(b, sizeof b, "%d smp · %.2f ms", smp, 1000.0 * smp / fs);
        return b;
    }

    static ViewModel from(const ViewInputs& in)
    {
        using S = engine::State;
        ViewModel vm;
        vm.deviceName = in.deviceName.empty() ? "No device" : in.deviceName;
        vm.modeText = in.mode == engine::Mode::Insert ? "Insert" : "Effect";
        vm.titleText = "NovaInsert  ·  " + vm.modeText;
        // no device selected: no route and no measurement time (the previous pair's values are not this instance's any more)
        const bool noDevice = in.deviceName.empty();
        vm.pathText = noDevice ? "No device selected" : in.interfaceName + " · " + in.outName + " → " + in.inName;
        switch (in.state)
        {
            case S::Idle: vm.stateText = "Idle"; vm.stateColour = kGrey; break;
            case S::Queued: vm.stateText = "Queued"; vm.stateColour = kBlue; break;
            case S::Measuring: vm.stateText = "Measuring"; vm.stateColour = kBlue; break;
            case S::ApplyingLatency: vm.stateText = "Applying latency"; vm.stateColour = kBlue; break;
            case S::Locked: vm.stateText = "Locked"; vm.stateColour = kGreen; break;
            case S::Stale: vm.stateText = "Stale"; vm.stateColour = kAmber; break;
            case S::Failed: vm.stateText = "Failed"; vm.stateColour = kRed; break;
            case S::Unavailable: vm.stateText = "Unavailable"; vm.stateColour = kRed; break;
            case S::UnsupportedHost: vm.stateText = "Unsupported host"; vm.stateColour = kRed; break;
            case S::UnsupportedSampleRate: vm.stateText = "Unsupported sample rate"; vm.stateColour = kRed; break;
        }
        vm.physLoopText = samplesText(in.physLoop, in.sampleRate);
        vm.pdcText = samplesText(in.reportedPdc, in.sampleRate);
        vm.driverText = in.driverIn < 0 || in.driverOut < 0
                            ? "—"
                            : samplesText(in.driverIn + in.driverOut, in.sampleRate) + " (reference)";
        vm.gapText = in.gapSamples > 0 ? samplesText(in.gapSamples, in.sampleRate) : "none";
        if (in.timestampUnix > 0 && !noDevice)
        {
            const std::time_t t = static_cast<std::time_t>(in.timestampUnix);
            std::tm tm {};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[32];
            std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
            vm.timestampText = b;
        }
        for (const auto& t : in.anticipativeViolationTracks) vm.warnings.push_back("Anticipative FX re-enabled on " + t);
        if (!in.conflictWith.empty()) vm.warnings.push_back("Device in use by " + in.conflictWith);
        if (in.renderLeftAt >= 0) vm.warnings.push_back("Render speed left at " + std::to_string(in.renderLeftAt));
        if (in.staleSnapshot) vm.warnings.push_back("The library record differs from this project's snapshot (kept)");
        if (in.openFallback)
            vm.warnings.push_back("Open check failed: " + reasonText(in.openFallbackReason) + " \u2014 saved latency in use");
        if (!in.libraryWarning.empty()) vm.warnings.push_back("Device library: " + in.libraryWarning);
        if (!hostVersionValidated(in.hostVersion)) vm.warnings.push_back("Unvalidated host version " + in.hostVersion);
        vm.showReapplyAnticipative = !in.anticipativeViolationTracks.empty();
        vm.showLeadInHint = in.gapSamples > 0 && !in.preRollLeadIn;
        const bool failing = in.state == S::Failed || in.state == S::Stale || in.state == S::Unavailable
                             || in.state == S::UnsupportedHost || in.state == S::UnsupportedSampleRate;
        if (failing) vm.failureReason = reasonText(in.reason);
        vm.calText = calStatusText(in.calStatus);
        vm.calError = in.calStatus >= 5;
        // one unit on both faces: dBu → 0 VU = +4 dBu at each converter (the true analog level, plan P3); dBFS → 0 VU = the
        // SETUP reference on both
        const bool whole = std::fabs(in.vuRefDbfs - std::round(in.vuRefDbfs)) < 0.05f;
        const std::string ref = in.meterDbu ? std::string("+4 dBu") : signedText(in.vuRefDbfs, whole ? 0 : 1) + " dBFS";
        vm.sendSubScaleOffset = in.meterDbu ? 4.0f - in.sendDbuAt0dBFS : in.vuRefDbfs;
        vm.returnSubScaleOffset = in.meterDbu ? 4.0f - in.returnDbuAt0dBFS : in.vuRefDbfs;
        vm.sendScaleLabel = "SEND \u00b7 0 VU = " + ref;
        vm.returnScaleLabel = "RETURN \u00b7 0 VU = " + ref;
        for (const auto& d : in.devices)
            vm.deviceItems.emplace_back(d.inUseBy.empty() ? d.name : d.name + " \u2014 in use: " + d.inUseBy, d.inUseBy.empty());
        if (in.defaultsChanged) vm.noteText = "Device defaults changed";
        // the track is muted while the loop is being measured and until REAPER has taken the new delay: say so
        if (in.state == engine::State::Measuring)            vm.progressText = "Latency check in progress \u2014 output muted until the loop is locked";
        else if (in.state == engine::State::Queued)          vm.progressText = "Latency check queued \u2014 output muted until the loop is locked";
        else if (in.state == engine::State::ApplyingLatency) vm.progressText = "Latency measured \u2014 output muted until REAPER applies the delay (play once)";
        vm.recalibrateEnabled = ! noDevice && engine::recalibrationOffered(in.state, in.openFallback);
        return vm;
    }
};
} // namespace ni::gui
