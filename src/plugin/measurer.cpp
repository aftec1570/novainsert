// Measurer (plan Task 15): see measurer.hpp.
#include "plugin/measurer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace ni::plugin
{
using namespace ni::engine;

namespace
{
LockInputs same(const CorrResult& r, uint32_t gen, int B) noexcept
{
    LockInputs in {};
    in.probe[0] = in.probe[1] = in.probe[2] = r;
    in.genStart = in.genNow = gen;
    in.hwBlock = B;
    return in;
}
} // namespace

Measurer::Measurer(ProbeIo& io, CorrelationWorker& worker, const ProbeGenerator& probe, std::function<int64_t()> nowMs, int attempts)
    : io_(io), worker_(worker), probe_(probe), now_(std::move(nowMs)), len_(probe.length()), attempts_(std::clamp(attempts, 1, kAttempts)),
      drainMs_(drainMsAt(probe, kDefaultSampleRate)),
      probeBuf_(static_cast<size_t>(probe.length())), capL_(probeBuf_.size()), capR_(probeBuf_.size())
{
    probe_.render(probeBuf_.data(), 0.0f);
    double s = 0.0;
    for (int32_t i = 0; i < probe.sweepLength(); ++i)
    {
        const double v = probeBuf_[static_cast<size_t>(probe.sweepStart() + i)];
        s += v * v;
    }
    sweepRmsDb_ = static_cast<float>(10.0 * std::log10(s / probe.sweepLength()));
}

// Post-probe silence + L_max in flight, as time at the device rate (the probe's sample counts are fixed).
int64_t Measurer::drainMsAt(const ProbeGenerator& probe, int fs) noexcept
{
    const int64_t f = fs > 0 ? fs : kDefaultSampleRate;
    return (static_cast<int64_t>(probe.length() - probe.sweepStart() - probe.sweepLength()) + kLMax) * 1000 / f + 1;
}

void Measurer::setSampleRate(int fs) noexcept { drainMs_ = drainMsAt(probe_, fs); }

void Measurer::start(uint32_t generation, const LevelHistory& history, bool stereo, int hwBlock)
{
    if (busy()) stopProbe(false);
    gen_ = generation; history_ = history; stereo_ = stereo; hwBlock_ = hwBlock;
    lastReason_ = Reason::None;
    unmuteAt_ = -1;
    if (! muted_) { io_.setMuted(true); muted_ = true; }
    totalStart_ = now_();
    attempt_ = 0;
    beginAttempt(kSettleMs);
}

void Measurer::beginAttempt(int64_t settleMs)
{
    ++attempt_;
    phaseStart_ = now_();
    attemptStart_ = phaseStart_ + settleMs;
    settleMs_ = settleMs;
    phase_ = Phase::Settle;
    probeIdx_ = 0; results_ = 0; levelDone_ = false; awaited_ = 0; pending_ = false;
    levelDb_ = latLevelDb_ = LevelPolicy::startLevelDb(history_);
    silenced_ = false;   // a failed attempt disarmed: the program goes out again until silenced
    silenceIfLive();
}

// While the transport plays the send carries program until a probe latches, and that program is still in flight round the loop
// when the probe starts: it lands in the probe's pre-silence (the arrival threshold rises above the click → NoReturn) or after
// it (a false arrival). So once the program is live, a silent one-sample probe holds the send silent (an armed probe sends
// silence after its end, never the program) and the settle becomes the L_max drain; the real probe replaces it at arm().
void Measurer::silenceIfLive()
{
    static constexpr float kSilence[1] { 0.0f };
    if (silenced_ || phase_ != Phase::Settle || ! programLive || ! programLive()) return;
    io_.setProbe(kSilence, 1);
    silenced_ = true;
    phaseStart_ = now_();
    settleMs_ = std::max(settleMs_, drainMs_);
    attemptStart_ = phaseStart_ + settleMs_;
}

void Measurer::arm(int index, float levelDb)
{
    probeIdx_ = index;
    levelDb_ = levelDb;
    probe_.render(probeBuf_.data(), levelDb);
    io_.setProbe(probeBuf_.data(), len_);
    phase_ = Phase::Capture;
    phaseStart_ = now_();
}

void Measurer::cancel(Reason why)
{
    if (! busy()) return;
    stopProbe(true);
    phase_ = Phase::Idle;
    lastReason_ = why;
}

// Disarms the probe; the mute is released immediately, or after the return-discard drain when a probe's return may still
// be in flight (the loop's tail must never reach the host output).
void Measurer::stopProbe(bool drainBeforeUnmute)
{
    io_.setProbe(nullptr, 0);
    awaited_ = 0;
    if (drainBeforeUnmute) unmuteAt_ = now_() + drainMs_;
    else if (muted_) { io_.setMuted(false); muted_ = false; }
}

void Measurer::onCorrelation(uint64_t token, const CorrResult& l, const CorrResult& r)
{
    if (token == inFlight_) inFlight_ = 0;   // the job's buffers are free again, stale or not
    if (token != awaited_ || awaited_ == 0) return;
    pending_ = true; pendingToken_ = token; pendL_ = l; pendR_ = r;
}

void Measurer::tick()
{
    const int64_t now = now_();
    if (phase_ == Phase::Idle)
    {
        if (unmuteAt_ >= 0 && now >= unmuteAt_ && muted_) { io_.setMuted(false); muted_ = false; unmuteAt_ = -1; }
        return;
    }
    if (pending_ && pendingToken_ == awaited_) processResult();
    if (now - totalStart_ >= kTotalTimeoutMs) { attempt_ = attempts_; attemptFailed(Reason::Timeout, true); return; }
    if (now - attemptStart_ >= kAttemptTimeoutMs) { attemptFailed(Reason::Timeout, true); return; }

    switch (phase_)
    {
        case Phase::Settle:
            silenceIfLive();   // play may start during the settle
            if (now_() - phaseStart_ >= settleMs_) arm(0, levelDb_);
            break;
        case Phase::Capture:
        {
            if (inFlight_ != 0) break;
            const int32_t n = windowEnd(std::min(io_.capturedCount(), len_));
            if (n < 0) { attemptFailed(Reason::NoReturn, true); return; }
            if (n > 0)
            {
                const uint64_t t = ++nextToken_;
                awaited_ = inFlight_ = t;
                awaitedIdx_ = probeIdx_;
                worker_.submit({ t, capL_.data(), stereo_ ? capR_.data() : nullptr, n, hwBlock_ });
                if (probeIdx_ == 0) phase_ = Phase::AwaitLevel;
                else if (probeIdx_ < kLatencyProbes) arm(probeIdx_ + 1, latLevelDb_);
                else phase_ = Phase::Tail;
            }
            break;
        }
        case Phase::AwaitLevel:
            if (levelDone_) arm(1, latLevelDb_);
            break;
        case Phase::Tail:
            if (results_ == kLatencyProbes) evaluate();   // every window closed after its return arrived: nothing left in flight
            break;
        case Phase::Idle:
            break;
    }
}

int32_t Measurer::windowEnd(int32_t c) noexcept
{
    const int32_t pre = probe_.sweepStart();
    if (c <= pre || ! io_.captureReturn(capL_.data(), stereo_ ? capR_.data() : nullptr, c)) return 0;
    double e = 0.0;
    for (int32_t i = 0; i < pre; ++i)
    {
        e += static_cast<double>(capL_[static_cast<size_t>(i)]) * capL_[static_cast<size_t>(i)];
        if (stereo_) e += static_cast<double>(capR_[static_cast<size_t>(i)]) * capR_[static_cast<size_t>(i)];
    }
    const double noise = std::sqrt(e / (stereo_ ? 2.0 * pre : static_cast<double>(pre)));
    const float thr = static_cast<float>(std::max(10.0 * noise, std::pow(10.0, kLockMinReturnDbfs / 20.0)));
    for (int32_t i = pre; i < c; ++i)
        if (std::fabs(capL_[static_cast<size_t>(i)]) >= thr || (stereo_ && std::fabs(capR_[static_cast<size_t>(i)]) >= thr))
        {
            const int32_t end = i + probe_.sweepLength() + kGuardSamples;
            return end <= c ? end : (c >= len_ ? len_ : 0);
        }
    return c >= len_ ? -1 : 0;
}

void Measurer::processResult()
{
    pending_ = false;
    awaited_ = 0;
    if (awaitedIdx_ == 0)
    {
        levelL_ = pendL_; levelR_ = pendR_;
        const double pk = stereo_ ? std::max(levelL_.returnPeakDbfs, levelR_.returnPeakDbfs) : levelL_.returnPeakDbfs;
        const bool lock = evaluateLock(same(levelL_, gen_, hwBlock_)).locked
                          && (! stereo_ || evaluateLock(same(levelR_, gen_, hwBlock_)).locked);
        const ProbeObs obs { static_cast<float>(pk), static_cast<float>(pk) - levelDb_, levelL_.clipped || (stereo_ && levelR_.clipped),
                             lock, false, true };
        latLevelDb_ = LevelPolicy::nextLevelDb(levelDb_, obs, 0.0f).value_or(levelDb_);
        levelDone_ = true;
        return;
    }
    lat_[0][awaitedIdx_ - 1] = pendL_;
    lat_[1][awaitedIdx_ - 1] = stereo_ ? pendR_ : pendL_;
    ++results_;
}

int32_t Measurer::stereoLag(int32_t lagL, double posL, int32_t lagR, double posR) noexcept
{
    if (lagL == lagR) return lagL;
    if (std::abs(lagL - lagR) != 1 || std::fabs(posL - posR) > kStereoSkewMax) return -1;
    const auto m = static_cast<int32_t>(std::lround(0.5 * (posL + posR)));
    return m == lagL || m == lagR ? m : -1;
}

void Measurer::evaluate()
{
    CalibResult res {};
    res.generation = gen_;
    const int chans = stereo_ ? 2 : 1;
    Reason why = Reason::None;
    double retPk = -300.0, retRms = 0.0;
    for (int c = 0; c < chans; ++c)
    {
        LockInputs in {};
        for (int k = 0; k < kLatencyProbes; ++k) in.probe[k] = lat_[c][k];
        in.genStart = in.genNow = gen_;
        in.hwBlock = hwBlock_;
        const auto v = evaluateLock(in);
        if (! v.locked && why == Reason::None) why = v.reason;
        const auto& p = lat_[c][0];
        res.physLoop[c] = p.lag;
        res.fractional[c] = static_cast<float>(p.fractional - p.lag);
        res.negativePolarity[c] = p.negative;
        res.pbrDb[c] = static_cast<float>(std::min({ lat_[c][0].pbrDb, lat_[c][1].pbrDb, lat_[c][2].pbrDb }));
        res.snrDb[c] = static_cast<float>(std::min({ lat_[c][0].snrDb, lat_[c][1].snrDb, lat_[c][2].snrDb }));
        for (const auto& q : lat_[c]) { retPk = std::max(retPk, q.returnPeakDbfs); retRms += q.returnRmsDbfs; }
    }
    if (why == Reason::None && stereo_ && res.physLoop[0] != res.physLoop[1])
    {
        const double pos[2] = { lat_[0][0].fractional, lat_[1][0].fractional };
        const int32_t common = stereoLag(res.physLoop[0], pos[0], res.physLoop[1], pos[1]);
        if (common < 0) why = Reason::ChannelAsymmetry;
        else
            for (int c = 0; c < 2; ++c) { res.physLoop[c] = common; res.fractional[c] = static_cast<float>(pos[c] - common); }
    }
    res.sendPeakDb = latLevelDb_;
    res.sendRmsDb = latLevelDb_ + sweepRmsDb_;
    res.returnPeakDb = static_cast<float>(retPk);
    res.returnRmsDb = static_cast<float>(retRms / (kLatencyProbes * chans));
    res.gainPeakDb = res.returnPeakDb - res.sendPeakDb;
    res.reportedPdc = res.physLoop[0] + kMarginBlocks * hwBlock_;
    res.timestampUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    res.failReason = why;
    if (why == Reason::None) finish(res, false);   // the discard window has passed: unmute now
    else if (attempt_ < attempts_) { io_.setProbe(nullptr, 0); beginAttempt(kSettleMs); }   // discard window passed
    else finish(res, false);
}

void Measurer::attemptFailed(Reason why, bool drainBeforeUnmute)
{
    if (attempt_ < attempts_) { io_.setProbe(nullptr, 0); awaited_ = 0; beginAttempt(drainBeforeUnmute ? drainMs_ : kSettleMs); return; }
    CalibResult res {};
    res.generation = gen_;
    res.failReason = why;
    finish(res, drainBeforeUnmute);
}

void Measurer::finish(const CalibResult& r, bool drainBeforeUnmute)
{
    stopProbe(drainBeforeUnmute);
    phase_ = Phase::Idle;
    lastReason_ = r.failReason;
    if (onResult) onResult(gen_, r);
}

Measurer::Progress Measurer::progress() const
{
    return { attempt_, probeIdx_, levelDb_, busy() ? now_() - totalStart_ : 0 };
}

} // namespace ni::plugin
