// Measurer — one instance's calibration sequence (F1 §7 attempts and lock, §8 levels and mute window; plan Task 15).
// Message-thread object driven by a 20 ms timer (tick()). Per attempt: mute (5 ms engine fade, one tick to settle) → level
// probe at LevelPolicy::startLevelDb → LevelPolicy::nextLevelDb from its G_peak (no raise → stay) → 3 latency probes →
// evaluateLock per channel (stereo: both channels must lock on the same lag, else ChannelAsymmetry) → unmute once the last
// probe's return-discard window (postSec + L_max after its last sweep sample = probe length + L_max captured) has passed.
// A failed attempt is retried once automatically; 15 s per attempt, 30 s in total, then Failed(Timeout). A retry after a
// timeout first waits out the drain (the cut probe's return may still arrive) and its 15 s start after that wait.
// The RT side (probe injection, return capture, mute fade) is the engine behind ProbeIo; correlation runs on a worker behind
// CorrelationWorker, whose results come back on the message thread through onCorrelation() (production: a process-wide
// low-priority thread + MessageManager::callAsync, wired in Task 16). The capture buffers handed to a job stay untouched
// until that job's result (current or stale) has come back. All buffers are allocated in the constructor.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "engine/correlator.hpp"
#include "engine/level_policy.hpp"
#include "engine/probe.hpp"
#include "engine/slot_loop.hpp"
#include "engine/types.hpp"

namespace ni::plugin
{

// The engine's measurement interface (SlotLoopEngine implements it through SlotLoopProbeIo).
struct ProbeIo
{
    virtual ~ProbeIo() = default;
    virtual void setMuted(bool m) noexcept = 0;
    virtual void setProbe(const float* probe, int32_t n) noexcept = 0;   // nullptr disarms at the next block boundary
    virtual int32_t capturedCount() const noexcept = 0;
    virtual bool captureReturn(float* l, float* r, int32_t n) noexcept = 0;
};

class SlotLoopProbeIo final : public ProbeIo
{
public:
    explicit SlotLoopProbeIo(engine::SlotLoopEngine& e) noexcept : e_(e) {}
    void setMuted(bool m) noexcept override { e_.setMuted(m); }
    void setProbe(const float* probe, int32_t n) noexcept override { e_.setProbe(probe, n); }
    int32_t capturedCount() const noexcept override { return e_.capturedCount(); }
    bool captureReturn(float* l, float* r, int32_t n) noexcept override { return e_.captureReturn(l, r, n); }
private:
    engine::SlotLoopEngine& e_;
};

struct CorrJob { uint64_t token; const float* l; const float* r; int32_t n; int32_t hwBlock; };   // r == nullptr: mono

struct CorrelationWorker
{
    virtual ~CorrelationWorker() = default;
    virtual void submit(const CorrJob& job) noexcept = 0;   // message thread; the result returns via Measurer::onCorrelation
};

class Measurer
{
public:
    static constexpr int64_t kSettleMs = 20, kAttemptTimeoutMs = 15000, kTotalTimeoutMs = 30000;
    static constexpr int kAttempts = 2, kLatencyProbes = 3;
    // A probe's window closes once its return is in: the first sample ≥ 20 dB over the pre-silence noise (and ≥ −60 dBFS)
    // after the probe start, + the probe's length + this guard (ringing, near echoes for the ambiguity check). A window
    // that runs to its end without an arrival fails the attempt (NoReturn) and drains: the return may still be in flight.
    static constexpr int32_t kGuardSamples = 960;   // 20 ms

    struct Progress { int attempt; int probeIndex; float levelDb; int64_t msElapsed; };   // probeIndex 0 = level probe

    // attempts: 2 = the attempt + its retry run here (F1 §7); 1 = the Scheduler owns the retry (product wiring, Task 16).
    Measurer(ProbeIo& io, CorrelationWorker& worker, const engine::ProbeGenerator& probe, std::function<int64_t()> nowMs,
             int attempts = kAttempts);

    void start(uint32_t generation, const engine::LevelHistory& history, bool stereo, int hwBlock);
    void abortForRecording() { cancel(engine::Reason::Recording); }
    void cancel(engine::Reason why);
    void tick();
    void onCorrelation(uint64_t token, const engine::CorrResult& l, const engine::CorrResult& r);

    bool busy() const noexcept { return phase_ != Phase::Idle; }
    engine::Reason lastReason() const noexcept { return lastReason_; }
    Progress progress() const;

    std::function<void(uint32_t gen, const engine::CalibResult&)> onResult;
    // True while the send carries program (transport playing). Empty = never. Message thread, polled from start() and tick().
    std::function<bool()> programLive;

private:
    enum class Phase : uint8_t { Idle, Settle, Capture, AwaitLevel, Tail };

    void beginAttempt(int64_t settleMs);
    void arm(int index, float levelDb);
    void processResult();
    void evaluate();
    void attemptFailed(engine::Reason why, bool drainBeforeUnmute);
    void finish(const engine::CalibResult& r, bool drainBeforeUnmute);
    void stopProbe(bool drainBeforeUnmute);
    void silenceIfLive();
    int32_t windowEnd(int32_t captured) noexcept;   // samples to analyse now, 0 = keep listening, −1 = no arrival in the window

    ProbeIo& io_;
    CorrelationWorker& worker_;
    const engine::ProbeGenerator& probe_;
    std::function<int64_t()> now_;
    const int32_t len_;
    const int attempts_;
    const int64_t drainMs_;
    float sweepRmsDb_ = 0.0f;   // probe RMS over the sweep at 0 dBFS peak
    std::vector<float> probeBuf_, capL_, capR_;

    Phase phase_ = Phase::Idle;
    engine::Reason lastReason_ = engine::Reason::None;
    uint32_t gen_ = 0;
    bool stereo_ = false;
    int hwBlock_ = 0;
    engine::LevelHistory history_ {};
    int attempt_ = 0, probeIdx_ = 0, results_ = 0;
    float levelDb_ = 0.0f, latLevelDb_ = 0.0f;
    bool levelDone_ = false;
    int64_t totalStart_ = 0, attemptStart_ = 0, phaseStart_ = 0, unmuteAt_ = -1, settleMs_ = kSettleMs;
    bool muted_ = false;
    bool silenced_ = false;   // this attempt's send is held silent (program live): its settle is the L_max drain
    uint64_t nextToken_ = 0, awaited_ = 0, inFlight_ = 0;
    int awaitedIdx_ = 0;
    bool pending_ = false;
    uint64_t pendingToken_ = 0;
    engine::CorrResult pendL_ {}, pendR_ {}, levelL_ {}, levelR_ {};
    engine::CorrResult lat_[2][kLatencyProbes] {};
};

} // namespace ni::plugin
