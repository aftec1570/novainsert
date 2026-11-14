#include "plugin/cal_controller.hpp"
#include <algorithm>
#include <cmath>
#include "engine/level_cal.hpp"
#include "engine/types.hpp"
#include "plugin/parameters.hpp"

namespace ni::plugin
{
namespace cal = engine::cal;
static_assert(static_cast<int>(CalStatus::ProgramOutOfRange) == kCalStatusMax);

CalController::CalController(CalPort& port)
    : port_(port), tone_(cal::kMaxToneLoopSamples), capL_(static_cast<size_t>(kCaptureCap)), capR_(capL_.size())
{
}

bool CalController::active() const
{
    return status_ == CalStatus::SendRunning || status_ == CalStatus::ReturnSettling || status_ == CalStatus::ReturnMeasuring;
}

std::optional<CalStatus> CalController::refusal(const CalContext& c) const
{
    if (c.effectMode) return CalStatus::RefusedEffect;
    if (c.recording) return CalStatus::RefusedRecording;
    if (! c.pairOwned) return CalStatus::RefusedNoPair;
    if (c.measuring) return CalStatus::RefusedMeasuring;
    return std::nullopt;
}

CalStatus CalController::pressSend(const CalContext& c)
{
    if (status_ == CalStatus::SendRunning) { cancel(); return status_; }   // second press: cancel, nothing written
    if (active()) return status_;   // a RETURN CAL runs: cancel it first
    if (c.recording) return status_ = CalStatus::RefusedRecording;   // Effect mode is fine: only the track is read
    if (! c.pairOwned) return status_ = CalStatus::RefusedNoPair;
    if (c.measuring) return status_ = CalStatus::RefusedMeasuring;
    result_.reset();
    startedAt_.reset();
    lastTick_.reset();
    progSeconds_ = 0.0; progPow_[0] = progPow_[1] = 0.0;
    return status_ = CalStatus::SendRunning;
}

CalStatus CalController::pressReturn(const CalContext& c)
{
    if (active()) return status_;
    if (const auto r = refusal(c)) return status_ = *r;
    fs_ = engine::isSupportedSampleRate(c.sampleRate) ? c.sampleRate : engine::kDefaultSampleRate;
    const int toneN = cal::toneLoopSamples(fs_);
    window_ = cal::windowSamples(fs_, engine::kLMax, kCaptureCap);
    cal::makeTone(tone_.data(), toneN, c.vuRefDbfs, fs_);
    need_ = std::clamp(port_.physLoop(), 0, static_cast<int>(engine::kLMax)) + cal::settleSamples(fs_) + window_;
    result_.reset();
    startedAt_.reset();
    port_.startTone(tone_.data(), toneN);
    return status_ = CalStatus::ReturnSettling;
}

void CalController::cancel()
{
    if (status_ == CalStatus::ReturnSettling || status_ == CalStatus::ReturnMeasuring) port_.stopTone();   // SEND CAL plays nothing
    result_.reset();
    status_ = CalStatus::Idle;
}

void CalController::finish(CalStatus s)
{
    if (status_ != CalStatus::SendRunning) port_.stopTone();   // SEND CAL started no tone
    status_ = s;
}

CalStatus CalController::tick(const CalContext& c, double now)
{
    if (! active()) return status_;
    if (c.recording)   // record start aborts: no write
    {
        result_.reset();
        finish(CalStatus::RefusedRecording);
        return status_;
    }
    if (! startedAt_) startedAt_ = now;
    const double elapsed = now - *startedAt_;
    if (status_ == CalStatus::SendRunning)
    {
        if (c.measuring) { result_.reset(); finish(CalStatus::RefusedMeasuring); return status_; }   // the send carries clicks now
        return program(c, now);
    }
    const int got = port_.captured();
    if (got >= need_) return measure(c);
    if (got >= need_ - window_) status_ = CalStatus::ReturnMeasuring;
    if (elapsed > static_cast<double>(need_) / fs_ + kReturnSlackSeconds) finish(CalStatus::NoReturn);
    return status_;
}

CalStatus CalController::measure(const CalContext& c)
{
    const bool ok = port_.capture(capL_.data(), capR_.data(), need_);
    port_.stopTone();
    if (! ok) return status_ = CalStatus::NoReturn;
    // a mono send leaves on one output at trim + offset[0]; a stereo return then measures both inputs against that one level
    const float sendEff[2] = { c.sendTrimDb + c.sendOffsetDb[0], c.sendTrimDb + (c.sendStereo ? c.sendOffsetDb[1] : c.sendOffsetDb[0]) };
    const float sent[2] = { c.vuRefDbfs + sendEff[0], c.vuRefDbfs + sendEff[1] };
    const auto g = cal::measureLoopGain(capL_.data(), capR_.data(), need_, need_ - window_, window_,
                                        sent, c.retStereo);
    if (g.clipped) return status_ = CalStatus::ReturnClipped;
    if (g.noSignal) return status_ = CalStatus::NoReturn;
    float eff[2] = { cal::returnTrimEff(sendEff[0], g.gainDb[0]), 0.0f };
    eff[1] = c.retStereo ? cal::returnTrimEff(sendEff[1], g.gainDb[1]) : eff[0];
    for (const float e : eff)
        if (e < kTrimMinDb || e > kTrimMaxDb) return status_ = CalStatus::OutOfRange;
    const float ganged = c.retStereo ? 0.5f * (eff[0] + eff[1]) : eff[0];
    std::array<float, 2> off { 0.0f, 0.0f };
    if (c.retStereo) off = { eff[0] - ganged, eff[1] - ganged };
    result_ = CalWrite { std::nullopt, ganged, off };
    return status_ = CalStatus::Done;
}

CalStatus CalController::program(const CalContext& c, double now)
{
    const double dt = lastTick_ ? std::clamp(now - *lastTick_, 0.0, 0.1) : 0.0;   // a stalled timer never counts as program
    lastTick_ = now;
    const int chans = c.sendStereo ? 2 : 1;   // a mono send leaves on one output only
    float pre[2] { -120.0f, -120.0f };
    for (int ch = 0; ch < chans; ++ch) pre[ch] = c.sendLevelDbfs[ch] - c.sendTrimDb;   // the program before the trim (offsets stay)
    if (dt > 0.0 && std::max(pre[0], pre[1]) > kProgramGateDbfs)
    {
        for (int ch = 0; ch < chans; ++ch) progPow_[ch] += dt * std::pow(10.0, pre[ch] / 10.0);
        progSeconds_ += dt;
    }
    if (progSeconds_ >= kProgramSeconds - 1e-6)
    {
        double loud = -1e9;
        for (int ch = 0; ch < chans; ++ch) loud = std::max(loud, 10.0 * std::log10(std::max(progPow_[ch] / progSeconds_, 1e-30)));
        const float trim = static_cast<float>(4.0 - c.sendDbuAt0dBFS - loud);   // 0 VU = +4 dBu at the converter
        if (trim < kTrimMinDb || trim > kTrimMaxDb) { status_ = CalStatus::ProgramOutOfRange; return status_; }
        result_ = CalWrite { trim, std::nullopt, std::nullopt };
        return status_ = CalStatus::Done;
    }
    if (now - *startedAt_ > kSendMaxSeconds) return status_ = CalStatus::NoProgram;
    return status_;
}

std::optional<CalWrite> CalController::takeResult()
{
    auto r = std::move(result_);
    result_.reset();
    return r;
}
} // namespace ni::plugin
