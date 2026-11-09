// NovaInsert engine core — shared value types (F1 §3 symbols, §4 CalibKey, §13 states, §15 reasons, §23 constants).
#pragma once
#include <cstdint>
#include <string>

namespace ni::engine
{
inline constexpr int32_t kLMax = 65536;        // F1 §23 Q8: max supported loop latency (samples)
inline constexpr int32_t kBMax = 2048;         // largest mandatory hardware buffer
inline constexpr int32_t kMaxInstances = 20;   // validated active instances
inline constexpr int32_t kMarginBlocks = 2;    // R = physLoop + kMarginBlocks * B (F1 §3)
inline constexpr int32_t kSampleRate = 48000;  // the only supported rate (R-SR-1)
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
    DriverUnsupported };

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

// RE-CALIBRATION is offered only after a failed calibration (user 2026-11-06): a Failed instance, or a failed open check
// running on the saved latency (still applying, locked or stale) — never while a check runs or a measured lock holds.
constexpr bool recalibrationOffered(State s, bool openFallback) noexcept
{
    return s == State::Failed || (openFallback && (s == State::Locked || s == State::ApplyingLatency || s == State::Stale));
}

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
