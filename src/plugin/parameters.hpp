// NovaInsert parameter table (F1 §19 parameter model). JUCE-free so unit tests can pin it.
// Ids and ORDER are stable forever: REAPER automation, project files and the Lua test drivers address them by index.
#pragma once
#include <array>
#include <cstddef>
#include <string_view>
#include "engine/types.hpp"

namespace ni::plugin
{

enum class Param : int
{
    Device = 0,            // int 0..999, 0 = none, else index into the device library order
    Mode,                  // choice Insert / Effect
    SendSel,               // choice Stereo / L / R (side of a stereo send; ignored for a mono send)
    ReturnSel,             // choice Stereo / L / R (side of a stereo return; ignored for a mono return)
    Stereo,                // bool
    SendTrim,              // float dB
    ReturnTrim,            // float dB
    PreRollLeadIn,         // bool
    ReapplyAnticipative,   // momentary bool, resets itself after one message-thread tick
    State,                 // read-only int = engine::State
    PhysLoop,              // read-only int samples
    ReportedPdc,           // read-only int samples
    InherentGap,           // read-only int samples
    Link,                  // bool: moving one trim moves the other by the opposite amount (VU GUI spec §7)
    CalAction,             // momentary int 0..3: 1 = SEND CAL toggle, 2 = RETURN CAL, 3 = cancel; resets to 0 after one tick
    CalStatus,             // read-only int = CalStatus (gui/cal_controller)
};

constexpr int kParamCount = 16;

constexpr std::array<std::string_view, kParamCount> kParamIds = {
    "device", "mode", "sendSel", "returnSel", "stereo", "sendTrim", "returnTrim",
    "preRollLeadIn", "reapplyAnticipative", "state", "physLoop", "reportedPdc", "inherentGap",
    "link", "calAction", "calStatus",
};

constexpr std::array<bool, kParamCount> kParamAutomatable = {
    true, true, true, true, true, true, true, true, true, false, false, false, false,
    true, false, false,
};

constexpr int kDeviceMax = 999;
constexpr float kTrimMinDb = -24.0f;
constexpr float kTrimMaxDb = 12.0f;
constexpr int kPhysLoopMax = engine::kLMax;                                   // 65536
constexpr int kCalActionMax = 3;
constexpr int kCalStatusMax = 13;   // CalStatus::ProgramOutOfRange; static_assert'ed against the enum where it is defined
constexpr int kStateMax = static_cast<int>(engine::State::UnsupportedSampleRate);   // 9

constexpr std::string_view paramId(Param p) noexcept { return kParamIds[static_cast<std::size_t>(p)]; }

} // namespace ni::plugin
