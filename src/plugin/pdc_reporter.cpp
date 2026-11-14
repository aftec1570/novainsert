#include "plugin/pdc_reporter.hpp"

namespace ni::plugin
{
using ni::engine::Reason;
using ni::engine::State;
using Phase = ni::engine::HostTransitionObserver::Phase;

void PdcReporter::request (int32_t R, bool deferToStop, uint32_t gen, int hwBlock, int64_t nowMs, Reason why)
{
    gen_ = gen; block_ = hwBlock;
    awaiting_ = false; timedOut_ = false; stoppedAfterTimeout_ = false; pendingR_ = -1; muted_ = false;
    if (R < 0 || R > ni::engine::kLMax)   // F1 §23 Q8 cap: no host call
    {
        applyState_ (State::Failed, Reason::OutOfRange, gen, nowMs, "R over L_max");
        return;
    }
    retryLeft_ = true;
    const bool defer = deferToStop && playing_;
    applyState_ (State::ApplyingLatency, why, gen, nowMs, defer ? "deferred to stop" : "");
    if (defer) pendingR_ = R;
    else apply (R, nowMs);
}

void PdcReporter::request (int32_t R, bool deferToStop, bool playingNow, uint32_t gen, int hwBlock, int64_t nowMs, Reason why)
{
    playing_ = playingNow;
    request (R, deferToStop, gen, hwBlock, nowMs, why);
}

void PdcReporter::cancel() noexcept
{
    pendingR_ = -1; awaiting_ = false; timedOut_ = false; retryLeft_ = false; stoppedAfterTimeout_ = false; muted_ = false;
}

void PdcReporter::apply (int32_t R, int64_t nowMs)
{
    obs_.begin (reported_, R, block_, playing_, nowMs, fs_);
    set_ (R);
    reported_ = R;
    pendingR_ = -1;
    awaiting_ = true;
}

void PdcReporter::onTransport (bool playing, int64_t nowMs)
{
    playing_ = playing;
    if (awaiting_) obs_.onTransport (playing, nowMs);
    if (! playing && pendingR_ >= 0) { apply (pendingR_, nowMs); return; }
    if (! playing && timedOut_ && ! retryLeft_) muted_ = false;   // final timeout: muted only until the user stops
    if (! timedOut_ || ! retryLeft_) return;
    if (! playing) { stoppedAfterTimeout_ = true; return; }
    if (! stoppedAfterTimeout_) return;
    // The one automatic retry (Q10): R is already reported, so re-observe the transition from this play segment.
    retryLeft_ = false; timedOut_ = false; stoppedAfterTimeout_ = false;
    if (! applyState_ (State::ApplyingLatency, Reason::None, gen_, nowMs, "retry")) return;
    obs_.begin (reported_, reported_, block_, false, nowMs, fs_);
    obs_.onTransport (true, nowMs);
    awaiting_ = true;
}

void PdcReporter::tick (int64_t nowMs)
{
    if (! awaiting_) return;
    const Phase p = obs_.poll (nowMs);
    if (p == Phase::StableLatencyAccepted)
    {
        awaiting_ = false; muted_ = false;
        applyState_ (State::Locked, Reason::None, gen_, nowMs, "");
    }
    else if (p == Phase::TimedOut)
    {
        awaiting_ = false; timedOut_ = true; muted_ = true;
        applyState_ (lkg_() ? State::Stale : State::Failed, Reason::PdcNotObserved, gen_, nowMs, "");
    }
}
} // namespace ni::plugin
