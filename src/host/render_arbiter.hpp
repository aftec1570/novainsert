// RenderArbiter (plan Task 20, F1 §10 Q14): render preflight over the involved instances, temporary online arming of the
// project's projrenderlimit (2) with guarded restore, render-dialog tracking, refusal of offline-only and queue paths.
// Command tables from the F0.6 spike (spikes/f0-coord/src/F06Coord.cpp, REAPER 7.83 ids). Message thread only.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "engine/types.hpp"
#include "host/reaper_api.hpp"

namespace ni::host
{
class RenderArbiter
{
public:
    struct InstanceView
    {
        int id;
        engine::State state;
        engine::Mode mode;
        int64_t lastProcessedMs;
        bool bypassed;
        std::string track;
    };
    enum class Decision { Observe, Armed, RefusedNotLocked, NoActiveInstance, RefusedOfflinePath /* Apply FX (device closed) since 2026-11-06 */, RefusedQueue, DialogArmed };

    static constexpr int kOnline = 2;             // projrenderlimit value for an online (realtime) render
    static constexpr int64_t kInvolvedMs = 400;   // processed within the last 400 ms = involved (§10)
    static constexpr int kDialogCmd = 40015;

    RenderArbiter (const ReaperApi& api, std::function<int64_t()> nowMs);
    ~RenderArbiter();

    void install();   // plugin_register hookcommand/hookcommand2/hookpostcommand/hookpostcommand2 (one arbiter per process)
    bool onCommandPre (int cmd, ReaProject* proj);   // true = block the action
    void onCommandPost (int cmd, ReaProject* proj);
    void tick();                                     // 250 ms: dialog window gone → restore; project closed → drop
    void setKeepOnlineFallback (bool on) noexcept { keepOnline_ = on; }
    Decision last() const noexcept { return last_; }
    int armCount() const noexcept { return armCount_; }

    std::function<std::vector<InstanceView>()> instances;
    std::function<void (std::string)> onMessage;
    std::function<void (std::string)> onTrace;   // decisions / restores for the diagnostics log (f3_6 evidence)
    std::function<bool()> renderWindowOpen;   // render dialog present (F1 §10 ASSUMPTION, validated by f3_6 dialog_close)
    std::function<void()> dialogOpening;      // 40015 pre-hook (armed): snapshot the windows
    std::function<void()> dialogOpened;       // 40015 post-hook: adopt the window that appeared as the render dialog

    static bool isRenderCmd (int c) noexcept;
    static bool isQueueCmd (int c) noexcept;
    static bool isActionCmd (int c) noexcept;
    static bool isApplyFxCmd (int c) noexcept;  // apply track/take FX to items: REAPER closes the audio device → refused   // freeze / apply FX / render to stem tracks: speed = workrender &8, not projrenderlimit
    static constexpr int kWorkRealtime = 8;     // workrender &8 "Limit apply FX/render stems to realtime" (global preference)
    static const char* decisionName (Decision d) noexcept;

private:
    int* var (ReaProject* p) const;
    void restore (const char* by);
    void restoreWork (const char* by);
    bool decide (Decision d, int cmd, int involved, bool block);
    void trace (std::string t) { if (onTrace) onTrace (std::move (t)); }
    void message (std::string m) { if (onMessage) onMessage (std::move (m)); }

    const ReaperApi& api_;
    std::function<int64_t()> now_;
    int varOffs_ = -1;
    int* work_ = nullptr;          // global workrender
    bool workArmed_ = false;
    int workPrev_ = 0;
    bool installed_ = false, keepOnline_ = false;
    bool armed_ = false, dialog_ = false;
    int armedCmd_ = 0, prevVal_ = 0, armCount_ = 0;
    ReaProject* armedProj_ = nullptr;
    int lastCmd_ = -1;
    int64_t lastCmdMs_ = -1000;
    Decision last_ = Decision::Observe;
};
} // namespace ni::host
