// Level CAL controller (VU GUI spec 2026-11-02 §2/§7). Message thread only; JUCE-free. Drives the reference tone through a
// CalPort (the instance's looping, send-trimmed latency-probe path) and turns what comes back into trim writes:
//   SEND CAL:   (user 2026-11-06) measures the track's own program on the send meter — no tone, no mute — until 3 s of signal
//               were averaged (mean power, AES17), then writes sendTrim so that average leaves the converter at 0 VU = +4 dBu:
//               sendTrim = (4 − sendDbuAt0dBFS) − (program level before the trim). Second press cancels; 30 s without program →
//               NoProgram. With LINK on the processor moves the return by the opposite amount.
//   RETURN CAL: waits physLoop + 0.5 s settle + 3 s window of capture, measures the loop gain per channel and writes the ganged
//               return trim = the mean of the per-channel unity trims and the offsets = their difference to it.
#pragma once
#include <array>
#include <optional>
#include <vector>

namespace ni::plugin
{
enum class CalStatus : int
{
    Idle = 0, SendRunning, ReturnSettling, ReturnMeasuring, Done,
    RefusedEffect, RefusedRecording, RefusedNoPair, RefusedMeasuring, ReturnClipped, NoReturn, OutOfRange, NoProgram, ProgramOutOfRange
};

struct CalPort
{
    virtual ~CalPort() = default;
    virtual void startTone(const float* tone, int n) = 0;   // looping, send-trimmed probe + host mute on; capture restarts
    virtual void stopTone() = 0;                            // disarm + unmute
    virtual int captured() const = 0;                       // samples captured since startTone
    virtual bool capture(float* l, float* r, int n) = 0;    // the first n captured samples (pre return trim)
    virtual int physLoop() const = 0;
};

struct CalContext
{
    bool effectMode = false, recording = false, pairOwned = false, measuring = false, sendStereo = false, retStereo = false;   // effective widths
    float vuRefDbfs = -20.0f, sendDbuAt0dBFS = 24.0f, sendTrimDb = 0.0f, returnTrimDb = 0.0f;
    float sendOffsetDb[2] { 0.0f, 0.0f }, returnOffsetDb[2] { 0.0f, 0.0f };
    float sendLevelDbfs[2] { -120.0f, -120.0f };   // the send meter now (AES17 dBFS, after trim and offsets): SEND CAL's input
};

struct CalWrite
{
    std::optional<float> sendTrimDb, returnTrimDb;
    std::optional<std::array<float, 2>> returnOffsetDb;
};

class CalController
{
public:
    static constexpr double kProgramSeconds = 3.0;      // SEND CAL: seconds of program averaged
    static constexpr double kSendMaxSeconds = 30.0;     // SEND CAL: no 3 s of program within this → NoProgram
    static constexpr float kProgramGateDbfs = -60.0f;   // a tick counts when the louder channel before the trim is above this
    static constexpr double kReturnSlackSeconds = 10.0;   // beyond the loop + settle + window time before "no return"
    static constexpr int kToneSamples = 4800;

    explicit CalController(CalPort& port);
    CalStatus pressSend(const CalContext& c);
    CalStatus pressReturn(const CalContext& c);
    void cancel();
    CalStatus tick(const CalContext& c, double nowSeconds);
    std::optional<CalWrite> takeResult();
    CalStatus status() const { return status_; }
    bool active() const;

private:
    std::optional<CalStatus> refusal(const CalContext& c) const;
    void finish(CalStatus s);
    CalStatus measure(const CalContext& c);
    CalStatus program(const CalContext& c, double now);

    CalPort& port_;
    CalStatus status_ = CalStatus::Idle;
    std::vector<float> tone_, capL_, capR_;
    std::optional<CalWrite> result_;
    std::optional<double> startedAt_;
    double progSeconds_ = 0.0, progPow_[2] { 0.0, 0.0 };
    std::optional<double> lastTick_;
    int need_ = 0;
};

static_assert(static_cast<int>(CalStatus::ProgramOutOfRange) == 13, "CalStatus must match P::kCalStatusMax");
} // namespace ni::plugin
