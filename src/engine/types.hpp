// NovaInsert engine core – shared value types (F1 §3 symbols, §4 CalibKey, §13 states, §15 reasons, §23 constants).
#pragma once
#include <cstdint>
#include <string>

namespace ni::engine
{
inline constexpr int32_t kLMax = 65536;        // F1 §23 Q8: max supported loop latency (samples)
inline constexpr int32_t kBMax = 2048;         // largest mandatory hardware buffer
inline constexpr int32_t kValidatedInstances = 20;   // active instances covered by the bench matrices; no runtime limit (2026-11-14)
inline constexpr int32_t kMarginBlocks = 2;    // R = physLoop + kMarginBlocks * B (F1 §3)
inline constexpr int32_t kDefaultSampleRate = 48000;   // rate assumed until the device reports one; the click probe's sample counts
// The supported device rates (R-SR-1, widened 2026-11-14 from 48 kHz only). Every other rate → UnsupportedSampleRate.
inline constexpr int32_t kSupportedSampleRates[] = { 44100, 48000, 88200, 96000, 192000 };
inline constexpr int32_t kMaxSampleRate = 192000;
constexpr bool isSupportedSampleRate(int32_t fs) noexcept
{
    for (const int32_t r : kSupportedSampleRates) if (r == fs) return true;
    return false;
}
inline constexpr int32_t kAlgorithmVersion = 1; // CalibKey field (F1 §4): bump when the measurement algorithm changes

struct Frame  { float l, r; int32_t gen; int64_t pos; };   // send ring: pos = project input sample index
struct RFrame { float l, r; int64_t pos; int32_t gen; };   // return ring: pos = input sample this return belongs to

enum class State : uint8_t { Idle = 0, Queued, Measuring, ApplyingLatency, Locked, Stale, Failed, Unavailable,
                             UnsupportedHost, UnsupportedSampleRate };

enum class Reason : uint8_t { None = 0,
    // re-evaluation triggers (F1 §13)
    ProjectLoad, StateRestore, RoutingChange, DeviceSelection, IoChange, BufferChange, DeviceReopen, DeviceLost,
    DeviceReturned, KeyChange, ConflictResolved,
    // failure reasons (F1 §7 lock conditions, §9, §15)
    NoReturn, Clipped, AmbiguousPeak, LowSnr, OutOfRange, Timeout, PdcNotObserved, AnticipativeOn, SampleRate,
    DefaultDevice, PairConflict, Recording, Crosstalk, LagMismatch, ChannelAsymmetry,
    // the user's RE-CALIBRATION (user 2026-11-06); appended so the stored failReason values keep their numbers
    UserRequest,
    // REAPER runs on a driver family this build does not support (1.1.0 port: CoreAudio / ASIO / ALSA); appended likewise
    DriverUnsupported,
    // REAPER reports a channel of this instance's loop but hands no buffer for it (outside the device's enabled input/output
    // range; forum report 2026-11-12); appended likewise
    ChannelNotDelivered,
    // the device sample rate changed (rate-independence 2026-11-14): a latency in samples no longer holds, re-measure
    SampleRateChange,
    // a bypassed instance handed its channels to an instance waiting for them, and claims them back when un-bypassed
    // (forum request 2026-11-17); appended likewise
    BypassYield, BypassResume };

// Side of a STEREO direction (mono/stereo routing design 2026-11-05): the whole pair, or one side of a stereo unit used as a
// mono loop. A MONO direction ignores its selector.
enum class SideSel : uint8_t { Stereo = 0, L, R };
enum class Mode      : uint8_t { Insert = 0, Effect };

// May a latency check be queued for an instance in state s? Automatic triggers leave Failed (and the unsupported /
// unavailable states) alone; the user's RE-CALIBRATION also restarts a Failed instance (user 2026-11-06).
constexpr bool measureRequestAllowed(State s, Reason r) noexcept
{
    switch (s)
    {
        case State::Idle: case State::Queued: case State::Measuring: case State::ApplyingLatency: case State::Locked: case State::Stale:
            return true;
        case State::Failed: return r == Reason::UserRequest;
        case State::Unavailable: case State::UnsupportedHost: case State::UnsupportedSampleRate: return false;
    }
    return false;
}

// Bypass handover (forum request 2026-11-17), decided on the 500 ms host poll. A bypassed instance keeps its channels while
// nobody else wants them, so a plain A/B bypass never re-measures; it yields them only when another instance waits for one of
// them (owner) or leaves the queue so it is never promoted while bypassed (waiter). A yielded instance claims again (and
// measures) once it is un-bypassed; if the channels are taken by then it waits like any other conflict.
enum class BypassAction : uint8_t { None, Yield, Resume };
constexpr BypassAction bypassAction(bool bypassed, bool yielded, bool owner, bool waiting, bool someoneWaitsOnMe) noexcept
{
    if (yielded) return bypassed ? BypassAction::None : BypassAction::Resume;
    if (! bypassed) return BypassAction::None;
    if (waiting) return BypassAction::Yield;
    return owner && someoneWaitsOnMe ? BypassAction::Yield : BypassAction::None;
}

// RE-CALIBRATION is offered only after a failed calibration (user 2026-11-06): a Failed instance, or a failed open check
// running on the saved latency (still applying, locked or stale) – never while a check runs or a measured lock holds.
constexpr bool recalibrationOffered(State s, bool openFallback) noexcept
{
    return s == State::Failed || (openFallback && (s == State::Locked || s == State::ApplyingLatency || s == State::Stale));
}

// Return polarity (D2, 1.5.0). The measurement detects an inverted return (correlator, CalibResult::negativePolarity per channel);
// it is accepted, never a lock failure. The unit has ONE polarity switch for both channels, so the loop counts as inverted when
// every channel it measured is: with one channel inverted and the other not, flipping both would not repair it.
constexpr bool returnDetectedInverted(const bool negativePolarity[2], bool retStereo) noexcept
{
    return negativePolarity[0] && (! retStereo || negativePolarity[1]);
}
// The flag "return is inverted" (with its one-click "Set polarity switch"): detected inverted while the switch is on normal. With
// the switch on inverted there is no flag (the hardware's inversion is compensated).
constexpr bool polarityFlag(bool detectedInverted, bool switchInverted) noexcept { return detectedInverted && ! switchInverted; }

// Calibration key (F1 §4): every field must match for a stored calibration to be reused. The library id is stored beside
// the key, never inside it.
struct CalibKey
{
    std::string identIn, identOut;
    int32_t sendIdx[2] { -1, -1 };   std::string sendName[2];
    int32_t returnIdx[2] { -1, -1 }; std::string returnName[2];
    int32_t sampleRate = 0;
    int32_t bufferSize = 0;
    bool sendStereo = false, returnStereo = false;   // record widths
    SideSel send = SideSel::Stereo, ret = SideSel::Stereo;
    Mode mode = Mode::Insert;
    int32_t algorithmVersion = 0;

    // Field-complete by construction (C++20 defaulted comparison): a new field is compared automatically (F1 §4).
    bool operator==(const CalibKey&) const = default;
};

// Result of one calibration attempt (F1 §7 stored per probe/attempt). Values MEASURED unless failReason != None.
struct CalibResult
{
    int32_t physLoop[2] { 0, 0 };        // hook slots, per channel (mono: [0] only)
    int32_t reportedPdc = 0;             // R = physLoop + kMarginBlocks * B
    float fractional[2] { 0.f, 0.f };    // diagnostic only (F1 §7)
    bool negativePolarity[2] { false, false };
    float sendPeakDb = 0.f, returnPeakDb = 0.f, sendRmsDb = 0.f, returnRmsDb = 0.f, gainPeakDb = 0.f;
    float pbrDb[2] { 0.f, 0.f };         // peak-to-background (condition 3)
    float snrDb[2] { 0.f, 0.f };         // condition 5
    int64_t timestampUnix = 0;
    uint32_t generation = 0;
    Reason failReason = Reason::None;
};
} // namespace ni::engine
