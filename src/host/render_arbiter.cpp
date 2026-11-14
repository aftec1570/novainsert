#include "host/render_arbiter.hpp"

namespace ni::host
{
namespace
{
using engine::Mode;
using engine::State;

RenderArbiter* g_arbiter = nullptr;   // the installed arbiter (REAPER hooks are plain function pointers)

bool hookCommand2 (KbdSectionInfo*, int cmd, int, int, int, HWND) { return g_arbiter && g_arbiter->onCommandPre (cmd, nullptr); }
bool hookCommand (int cmd, int) { return g_arbiter && g_arbiter->onCommandPre (cmd, nullptr); }
void hookPostCommand2 (KbdSectionInfo*, int cmd, int, int, int, HWND, ReaProject* p) { if (g_arbiter) g_arbiter->onCommandPost (cmd, p); }
void hookPostCommand (int cmd, int) { if (g_arbiter) g_arbiter->onCommandPost (cmd, nullptr); }

const char* stateName (State s)
{
    switch (s)
    {
        case State::Idle: return "Idle";
        case State::Queued: return "Queued";
        case State::Measuring: return "Measuring";
        case State::ApplyingLatency: return "ApplyingLatency";
        case State::Locked: return "Locked";
        case State::Stale: return "Stale";
        case State::Failed: return "Failed";
        case State::Unavailable: return "Unavailable";
        case State::UnsupportedHost: return "UnsupportedHost";
        case State::UnsupportedSampleRate: return "UnsupportedSampleRate";
    }
    return "?";
}
} // namespace

RenderArbiter::RenderArbiter (const ReaperApi& api, std::function<int64_t()> nowMs) : api_ (api), now_ (std::move (nowMs))
{
    int sz = 0;
    if (api_.projectconfig_var_getoffs) varOffs_ = api_.projectconfig_var_getoffs ("projrenderlimit", &sz);
    int wsz = 0;
    if (api_.get_config_var) { work_ = static_cast<int*> (api_.get_config_var ("workrender", &wsz)); if (wsz != static_cast<int> (sizeof (int))) work_ = nullptr; }
}

RenderArbiter::~RenderArbiter()
{
    // the last instance removed while the dialog holds the arm (S6 review I1): never leave our 2 behind
    if (armed_ && (! api_.ValidatePtr2 || api_.ValidatePtr2 (nullptr, armedProj_, "ReaProject*"))) restore ("destroy");
    restoreWork ("destroy");
    if (installed_ && api_.plugin_register)
    {
        api_.plugin_register ("-hookcommand2", reinterpret_cast<void*> (&hookCommand2));
        api_.plugin_register ("-hookcommand", reinterpret_cast<void*> (&hookCommand));
        api_.plugin_register ("-hookpostcommand2", reinterpret_cast<void*> (&hookPostCommand2));
        api_.plugin_register ("-hookpostcommand", reinterpret_cast<void*> (&hookPostCommand));
    }
    if (g_arbiter == this) g_arbiter = nullptr;
}

void RenderArbiter::install()
{
    if (installed_ || g_arbiter != nullptr || ! api_.plugin_register) return;
    g_arbiter = this;
    installed_ = true;
    api_.plugin_register ("hookcommand2", reinterpret_cast<void*> (&hookCommand2));
    api_.plugin_register ("hookcommand", reinterpret_cast<void*> (&hookCommand));
    api_.plugin_register ("hookpostcommand2", reinterpret_cast<void*> (&hookPostCommand2));
    api_.plugin_register ("hookpostcommand", reinterpret_cast<void*> (&hookPostCommand));
}

bool RenderArbiter::isRenderCmd (int c) noexcept
{
    static constexpr int ids[] = { 40015, 41824, 42230, 41855, 43349,          // render dialog / last settings / dry run
                                   41823, 41207, 40929,                        // queue: add / render all / open
                                   41223, 40877, 40901,                        // freeze stereo / multichannel / mono
                                   40209, 40361, 41993, 42685,                 // apply track/take FX to items
                                   40788, 40789, 40405, 40537, 40892, 40893,   // render tracks to stem tracks
                                   41890, 41891, 41892 };                      // region render matrix
    for (int i : ids) if (i == c) return true;
    return false;
}

bool RenderArbiter::isQueueCmd (int c) noexcept { return c == 41823 || c == 41207 || c == 40929; }   // background project

bool RenderArbiter::isApplyFxCmd (int c) noexcept { return c == 40209 || c == 40361 || c == 41993 || c == 42685; }

bool RenderArbiter::isActionCmd (int c) noexcept
{
    static constexpr int ids[] = { 41223, 40877, 40901, 40788, 40789, 40405, 40537, 40892, 40893 };   // freeze / stem tracks
    for (int i : ids) if (i == c) return true;
    return false;
}

void RenderArbiter::restoreWork (const char* by)
{
    if (! workArmed_ || work_ == nullptr) return;
    workArmed_ = false;
    if (*work_ == (workPrev_ | kWorkRealtime))
    {
        *work_ = workPrev_;   // guarded: only our own bit is undone
        trace ("restore_work|to=" + std::to_string (workPrev_) + "|by=" + by);
    }
    else trace ("left_work|at=" + std::to_string (*work_) + "|by=" + by);
}

int* RenderArbiter::var (ReaProject* p) const
{
    if (varOffs_ < 0 || ! api_.projectconfig_var_addr || p == nullptr) return nullptr;
    return static_cast<int*> (api_.projectconfig_var_addr (p, varOffs_));
}

const char* RenderArbiter::decisionName (Decision d) noexcept
{
    switch (d)
    {
        case Decision::Observe: return "Observe";
        case Decision::Armed: return "Armed";
        case Decision::RefusedNotLocked: return "RefusedNotLocked";
        case Decision::NoActiveInstance: return "NoActiveInstance";
        case Decision::RefusedOfflinePath: return "RefusedOfflinePath";
        case Decision::RefusedQueue: return "RefusedQueue";
        case Decision::DialogArmed: return "DialogArmed";
    }
    return "?";
}

bool RenderArbiter::decide (Decision d, int cmd, int involved, bool block)
{
    last_ = d;
    const bool armedNow = d == Decision::Armed || d == Decision::DialogArmed;
    trace (std::string ("decision=") + decisionName (d) + "|cmd=" + std::to_string (cmd) + "|involved=" + std::to_string (involved)
           + "|prev=" + (armedNow && ! keepOnline_ ? std::to_string (prevVal_) : std::string ("-")));
    return block;
}

bool RenderArbiter::onCommandPre (int cmd, ReaProject* proj)
{
    const int64_t t = now_();
    const bool dup = cmd == lastCmd_ && t - lastCmdMs_ < 50;   // hookcommand + hookcommand2 both fire for one action
    lastCmd_ = cmd; lastCmdMs_ = t;
    if (! isRenderCmd (cmd)) { last_ = Decision::Observe; return false; }
    if (dup) return last_ == Decision::RefusedNotLocked || last_ == Decision::RefusedOfflinePath || last_ == Decision::RefusedQueue;

    ReaProject* p = proj ? proj : (api_.EnumProjects ? api_.EnumProjects (-1, nullptr, 0) : nullptr);
    int involved = 0;
    std::string refusing;
    for (const InstanceView& v : instances ? instances() : std::vector<InstanceView> {})
    {
        // Idle = no device claimed (or released): plain pass-through, no hardware loop to carry – not in the §10 blocking list
        if (v.bypassed || v.state == State::Idle || t - v.lastProcessedMs > kInvolvedMs) continue;
        ++involved;
        // Locked only, in both modes: an Effect instance reaches Locked through the driver path (no probe), so the mode
        // is no substitute – a Failed / Unavailable / Stale Effect instance is dry pass-through (§10)
        const bool ok = v.state == State::Locked;
        if (! ok) refusing += (refusing.empty() ? "" : ", ") + v.track + " (" + stateName (v.state) + ")";
    }
    if (! refusing.empty())
    {
        message ("render refused, not locked: " + refusing);
        return decide (Decision::RefusedNotLocked, cmd, involved, true);
    }
    if (involved == 0) return decide (Decision::NoActiveInstance, cmd, 0, false);
    // freeze / render to stem tracks run online (user 2026-11-06): armed below. Apply FX runs with the audio device closed even
    // when limited to realtime (bench 2026-11-06): a hardware insert cannot take part and every instance would lose its lock
    if (isApplyFxCmd (cmd))
    {
        message ("render refused: REAPER stops the audio interface for Apply FX, so the outboard gear on NovaInsert tracks "
                 "cannot be included. Use Freeze or Render tracks to stem tracks instead - they run in realtime through the hardware.");
        return decide (Decision::RefusedOfflinePath, cmd, involved, true);
    }
    if (isQueueCmd (cmd))
    {
        message ("render refused: the render queue loads uncalibrated instances");
        return decide (Decision::RefusedQueue, cmd, involved, true);
    }
    // freeze / apply FX / stem tracks run at the global "limit apply FX/render stems to realtime" speed, not the project's
    // render speed (bench 2026-11-06): offline they close the audio device and the hardware loop cannot carry the render
    if (isActionCmd (cmd) && work_ != nullptr && ! workArmed_ && (*work_ & kWorkRealtime) == 0)
    {
        workPrev_ = *work_; workArmed_ = true;
        *work_ = workPrev_ | kWorkRealtime;
        trace ("arm_work|prev=" + std::to_string (workPrev_) + "|cmd=" + std::to_string (cmd));
    }
    int* v = var (p);
    if (v == nullptr) return decide (Decision::Observe, cmd, involved, false);
    if (keepOnline_) { *v = kOnline; return decide (Decision::Armed, cmd, involved, false); }
    if (! armed_) { prevVal_ = *v; armedProj_ = p; ++armCount_; }   // never re-remember our own 2
    // sticky: a render started while the dialog is open does not end the dialog's ownership (S6 review C1)
    armed_ = true; armedCmd_ = cmd; dialog_ = dialog_ || cmd == kDialogCmd;
    if (cmd == kDialogCmd && dialogOpening) dialogOpening();
    *v = kOnline;
    return decide (cmd == kDialogCmd ? Decision::DialogArmed : Decision::Armed, cmd, involved, false);
}

void RenderArbiter::restore (const char* by)
{
    armed_ = false; dialog_ = false;
    int* v = var (armedProj_);
    armedProj_ = nullptr;
    if (v == nullptr) return;
    if (*v == kOnline)
    {
        *v = prevVal_;   // guarded: only our own value is undone
        trace ("restore|to=" + std::to_string (prevVal_) + "|by=" + by);
    }
    else
    {
        trace ("left|at=" + std::to_string (*v) + "|by=" + by);
        message ("render speed left at " + std::to_string (*v));
    }
}

void RenderArbiter::onCommandPost (int cmd, ReaProject*)
{
    if (isActionCmd (cmd)) restoreWork ("post");
    if (! armed_ || ! isRenderCmd (cmd)) return;
    if (cmd == kDialogCmd && dialogOpened) dialogOpened();
    // The open dialog owns the settings until it closes: it pushes its own copy (2) into the project at every render it
    // starts (F0.4), so while it is open every render post-hook holds and only the tick restores (S6 review C1).
    if (dialog_ && renderWindowOpen && renderWindowOpen())
    {
        trace ("hold|cmd=" + std::to_string (cmd) + "|dialog_open=1");
        return;
    }
    if (cmd == kDialogCmd) { trace ("hold|cmd=40015|dialog_unseen"); return; }   // the tick decides on its next run
    restore ("post");
}

void RenderArbiter::tick()
{
    if (! armed_) return;
    if (api_.ValidatePtr2 && ! api_.ValidatePtr2 (nullptr, armedProj_, "ReaProject*"))
    {
        armed_ = false; dialog_ = false; armedProj_ = nullptr;   // project closed: nothing left to restore into
        trace ("dropped|project_closed");
        return;
    }
    if (dialog_ && ! (renderWindowOpen && renderWindowOpen())) restore ("tick");
}
} // namespace ni::host
