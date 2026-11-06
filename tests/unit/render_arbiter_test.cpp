// RenderArbiter (plan Task 20, F1 §10 Q14): preflight over involved instances, temporary online arming of projrenderlimit
// (2), guarded restore, render-dialog tracking, refusal of offline-only and queue paths. Fake API: one int project var.
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include "host/render_arbiter.hpp"
#include <filesystem>
#include <fstream>
#include "plugin/library.hpp"
#include "plugin/settings_apply.hpp"

using namespace ni::host;
using ni::engine::Mode;
using ni::engine::State;
using Decision = RenderArbiter::Decision;

namespace
{
int g_var = 0;
int g_work = 0;   // global preference `workrender` (&8 = limit apply FX / render stems / freeze to realtime)
bool g_projValid = true;
ReaProject* const kProj = reinterpret_cast<ReaProject*>(0x1000);
int fakeOffs(const char* name, int* sz) { if (sz) *sz = 4; return std::string(name) == "projrenderlimit" ? 7 : -1; }
void* fakeAddr(ReaProject* p, int idx) { return p == kProj && idx == 7 ? &g_var : nullptr; }
ReaProject* fakeEnum(int, char*, int) { return kProj; }
bool fakeValidate(ReaProject*, void* p, const char*) { return g_projValid && p == kProj; }
void* fakeConfig(const char* name, int* sz) { if (sz) *sz = 4; return std::string(name) == "workrender" ? &g_work : nullptr; }

struct Rig
{
    ReaperApi api;
    int64_t now = 10000;
    bool window = false;
    std::vector<RenderArbiter::InstanceView> views;
    std::vector<std::string> messages;
    RenderArbiter arb;

    Rig() : api(make()), arb(api, [this] { return now; })
    {
        g_var = 0; g_work = 0; g_projValid = true;
        arb.instances = [this] { return views; };
        arb.onMessage = [this](std::string m) { messages.push_back(std::move(m)); };
        arb.renderWindowOpen = [this] { return window; };
    }
    static ReaperApi make()
    {
        ReaperApi a;
        a.projectconfig_var_getoffs = &fakeOffs;
        a.projectconfig_var_addr = &fakeAddr;
        a.EnumProjects = &fakeEnum;
        a.ValidatePtr2 = &fakeValidate;
        a.get_config_var = &fakeConfig;
        return a;
    }
    void add(State s, std::string track, Mode m = Mode::Insert, int64_t ageMs = 10, bool bypassed = false)
    {
        views.push_back({ static_cast<int>(views.size()) + 1, s, m, now - ageMs, bypassed, std::move(track) });
    }
};
} // namespace

TEST_CASE("arbiter_arms_2_and_restores_prev")
{
    Rig r;
    r.add(State::Locked, "Bass");
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::Armed);
    REQUIRE(g_var == 2);
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 0);
}

TEST_CASE("arbiter_refuses_when_any_involved_not_locked")
{
    Rig r;
    r.add(State::Locked, "Bass");
    r.add(State::Measuring, "Vox");
    REQUIRE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::RefusedNotLocked);
    REQUIRE(g_var == 0);
    REQUIRE(r.messages.size() == 1);
    REQUIRE(r.messages[0].find("Vox") != std::string::npos);
    REQUIRE(r.messages[0].find("Bass") == std::string::npos);
}

TEST_CASE("arbiter_ignores_instance_idle_for_401ms")
{
    Rig r;
    r.add(State::Locked, "Bass");
    r.add(State::Measuring, "Old", Mode::Insert, false, 401);
    r.add(State::Failed, "Bypassed", Mode::Insert, 10, true);
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::Armed);
}

TEST_CASE("arbiter_no_involved_instance_observes_only")
{
    Rig r;
    r.add(State::Measuring, "Old", Mode::Insert, false, 500);
    g_var = 1;
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::NoActiveInstance);
    REQUIRE(g_var == 1);
}

TEST_CASE("arbiter_effect_mode_requires_locked")
{
    // F4 final review: an Effect instance reaches Locked without a probe (driver round-trip); the mode itself is no
    // substitute for Locked — a Failed / Unavailable (PairConflict, DeviceLost) / Stale Effect instance would otherwise
    // render dry pass-through with no refusal (spec §10 blocking list).
    Rig r;
    r.add(State::Failed, "Fx", Mode::Effect);
    REQUIRE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::RefusedNotLocked);
    Rig u;
    u.add(State::Unavailable, "Fx", Mode::Effect);
    REQUIRE(u.arb.onCommandPre(41824, kProj));
    REQUIRE(u.arb.last() == Decision::RefusedNotLocked);
    Rig q;
    q.add(State::Failed, "Fx", Mode::Effect, false);
    REQUIRE(q.arb.onCommandPre(41824, kProj));
    Rig ok;
    ok.add(State::Locked, "Fx", Mode::Effect);
    REQUIRE_FALSE(ok.arb.onCommandPre(41824, kProj));
    REQUIRE(ok.arb.last() == Decision::Armed);
}

TEST_CASE("arbiter_guarded_restore_user_changed_value")
{
    Rig r;
    r.add(State::Locked, "Bass");
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    g_var = 1;   // user changed the render speed during the render
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 1);
    REQUIRE(r.messages.back() == "render speed left at 1");
}

// user 2026-11-06: freeze, apply FX and render-to-stem-tracks honour REAPER's render speed, so they run online (armed 2,
// restored after) like any render; only the queue (a background project with uncalibrated instances) stays refused
TEST_CASE("arbiter_arms_freeze_and_stem_renders_online_refuses_queue")
{
    for (int cmd : { 41223, 40877, 40901, 40788, 40789, 40405, 40537, 40892, 40893 })
    {
        Rig r;
        g_var = 0;
        r.add(State::Locked, "Bass");
        INFO(cmd);
        REQUIRE_FALSE(r.arb.onCommandPre(cmd, kProj));
        REQUIRE(r.arb.last() == Decision::Armed);
        REQUIRE(g_var == RenderArbiter::kOnline);
        r.arb.onCommandPost(cmd, kProj);
        REQUIRE(g_var == 0);
        REQUIRE(r.messages.empty());
    }
    Rig q;
    q.add(State::Locked, "Bass");
    REQUIRE(q.arb.onCommandPre(41207, kProj));
    REQUIRE(q.arb.last() == Decision::RefusedQueue);
}

// bench 2026-11-06 (rn_freeze b128): REAPER's freeze / apply FX / render-to-stem-tracks ignore projrenderlimit; their speed is
// the global preference workrender &8 "Limit apply FX/render stems to realtime". Without it the action ran offline in 0.36 s
// for 4 s, REAPER closed the audio device (DeviceLost) and the instances re-measured on its return.
TEST_CASE("arbiter_limits_freeze_and_stem_actions_to_realtime_and_restores_the_preference")
{
    for (int cmd : { 41223, 40877, 40901, 40788, 40789, 40405, 40537, 40892, 40893 })
    {
        Rig r;
        g_work = 0x21;   // the user's other workrender bits survive
        r.add(State::Locked, "Gitar");
        INFO(cmd);
        REQUIRE_FALSE(r.arb.onCommandPre(cmd, kProj));
        REQUIRE(g_work == (0x21 | 8));
        r.arb.onCommandPost(cmd, kProj);
        REQUIRE(g_work == 0x21);
    }
}

// bench 2026-11-06 (rn_apply_fx b64): Apply FX ran realtime-paced (5.2 s for 4 s) but REAPER closed the audio device for it:
// the hardware loop cannot take part (silent result) and every instance lost its lock. Refused with a plain message.
TEST_CASE("arbiter_refuses_apply_fx_with_a_message_and_touches_nothing")
{
    for (int cmd : { 40209, 40361, 41993, 42685 })
    {
        Rig r;
        g_work = 0x11;
        r.add(State::Locked, "Gitar");
        INFO(cmd);
        REQUIRE(r.arb.onCommandPre(cmd, kProj));
        REQUIRE(r.arb.last() == Decision::RefusedOfflinePath);
        REQUIRE(g_var == 0);
        REQUIRE(g_work == 0x11);
        REQUIRE(r.messages.size() == 1);
        REQUIRE(r.messages[0].find("Apply FX") != std::string::npos);
        REQUIRE(r.messages[0].find("Freeze") != std::string::npos);
    }
    Rig none;   // no NovaInsert in use: REAPER's own Apply FX, untouched
    REQUIRE_FALSE(none.arb.onCommandPre(40209, kProj));
}

TEST_CASE("arbiter_leaves_workrender_alone_for_file_renders_and_when_already_realtime")
{
    for (int cmd : { 40015, 41824, 42230, 41890 })
    {
        Rig r;
        g_work = 0x20;
        r.add(State::Locked, "Gitar");
        INFO(cmd);
        r.arb.onCommandPre(cmd, kProj);
        REQUIRE(g_work == 0x20);
        r.window = false;
        r.arb.onCommandPost(cmd, kProj);
        r.arb.tick();
        REQUIRE(g_work == 0x20);
    }
    Rig a;
    g_work = 0x28;   // the user already limits these actions to realtime: kept, never cleared after
    a.add(State::Locked, "Gitar");
    a.arb.onCommandPre(40901, kProj);
    REQUIRE(g_work == 0x28);
    a.arb.onCommandPost(40901, kProj);
    REQUIRE(g_work == 0x28);
}

TEST_CASE("arbiter_realtime_actions_also_in_keep_online_mode_and_not_when_refused")
{
    Rig r;
    r.arb.setKeepOnlineFallback(true);
    r.add(State::Locked, "Gitar");
    r.arb.onCommandPre(40537, kProj);
    REQUIRE(g_work == 8);
    r.arb.onCommandPost(40537, kProj);
    REQUIRE(g_work == 0);
    Rig n;
    n.add(State::ApplyingLatency, "Gitar");
    REQUIRE(n.arb.onCommandPre(40537, kProj));
    REQUIRE(g_work == 0);
}

TEST_CASE("arbiter_destroyed_mid_action_restores_workrender")
{
    {
        Rig r;
        r.add(State::Locked, "Gitar");
        r.arb.onCommandPre(41223, kProj);
        REQUIRE(g_work == 8);
    }
    REQUIRE(g_work == 0);
}

TEST_CASE("arbiter_refuses_freeze_while_not_locked")
{
    Rig r;
    r.add(State::ApplyingLatency, "Gitar");
    REQUIRE(r.arb.onCommandPre(40901, kProj));
    REQUIRE(r.arb.last() == Decision::RefusedNotLocked);
    REQUIRE(g_var == 0);
}

TEST_CASE("arbiter_dialog_arm_on_open_restore_on_close_once")
{
    Rig r;
    r.add(State::Locked, "Bass");
    r.window = true;
    REQUIRE_FALSE(r.arb.onCommandPre(40015, kProj));
    REQUIRE(r.arb.last() == Decision::DialogArmed);
    REQUIRE(g_var == 2);
    r.arb.onCommandPost(40015, kProj);   // dialog still open: it owns the settings, nothing restored
    REQUIRE(g_var == 2);
    r.arb.tick();
    REQUIRE(g_var == 2);
    r.window = false;
    r.arb.tick();
    REQUIRE(g_var == 0);
    g_var = 5;   // a later user value is never touched again
    r.arb.tick();
    r.arb.tick();
    REQUIRE(g_var == 5);
}

TEST_CASE("arbiter_dialog_project_closed_drops_arming_without_write")
{
    Rig r;
    r.add(State::Locked, "Bass");
    r.window = true;
    REQUIRE_FALSE(r.arb.onCommandPre(40015, kProj));
    g_projValid = false;
    r.arb.tick();
    g_projValid = true;
    r.window = false;
    r.arb.tick();
    REQUIRE(g_var == 2);   // the project was gone: no write into it afterwards
}

TEST_CASE("arbiter_abort_still_restores")
{
    Rig r;
    r.add(State::Locked, "Bass");
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    r.now += 50;   // user aborts the render; REAPER still fires the post-hook
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 0);
}

TEST_CASE("arbiter_duplicate_hookcommand_and_hookcommand2_counted_once")
{
    Rig r;
    r.add(State::Locked, "Bass");
    g_var = 1;
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));   // the second hook for the same action: must not re-remember 2
    r.arb.onCommandPost(41824, kProj);
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 1);
    REQUIRE(r.arb.armCount() == 1);
}

TEST_CASE("arbiter_keep_online_fallback_off_by_default")
{
    Rig r;
    r.add(State::Locked, "Bass");
    g_var = 1;
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 1);
    r.arb.setKeepOnlineFallback(true);
    r.now += 1000;
    r.views[0].lastProcessedMs = r.now;   // still playing through the instance
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 2);   // keep-online: written and left online
}

TEST_CASE("arbiter_non_render_command_ignored")
{
    Rig r;
    r.add(State::Measuring, "Vox");
    REQUIRE_FALSE(r.arb.onCommandPre(40044, kProj));   // transport play
    REQUIRE(r.arb.last() == Decision::Observe);
    REQUIRE(r.messages.empty());
}

TEST_CASE("arbiter_traces_decisions_and_restores_for_the_diagnostics")
{
    Rig r;
    std::vector<std::string> tr;
    r.arb.onTrace = [&tr](std::string t) { tr.push_back(std::move(t)); };
    r.add(State::Locked, "Bass");
    r.arb.onCommandPre(42230, kProj);
    r.arb.onCommandPost(42230, kProj);
    r.window = true;
    r.arb.onCommandPre(40015, kProj);
    r.arb.onCommandPost(40015, kProj);
    r.window = false;
    r.arb.tick();
    r.add(State::Measuring, "Vox");
    r.arb.onCommandPre(41223, kProj);
    REQUIRE(tr == std::vector<std::string> { "decision=Armed|cmd=42230|involved=1|prev=0", "restore|to=0|by=post",
                                             "decision=DialogArmed|cmd=40015|involved=1|prev=0", "hold|cmd=40015|dialog_open=1",
                                             "restore|to=0|by=tick", "decision=RefusedNotLocked|cmd=41223|involved=2|prev=-" });
}

TEST_CASE("arbiter_traces_guarded_skip_and_dropped_project")
{
    Rig r;
    std::vector<std::string> tr;
    r.arb.onTrace = [&tr](std::string t) { tr.push_back(std::move(t)); };
    r.add(State::Locked, "Bass");
    r.arb.onCommandPre(42230, kProj);
    g_var = 1;
    r.arb.onCommandPost(42230, kProj);
    r.window = true;
    r.arb.onCommandPre(40015, kProj);
    g_projValid = false;
    r.arb.tick();
    REQUIRE(tr == std::vector<std::string> { "decision=Armed|cmd=42230|involved=1|prev=0", "left|at=1|by=post",
                                             "decision=DialogArmed|cmd=40015|involved=1|prev=1", "dropped|project_closed" });
}

TEST_CASE("arbiter_idle_instance_without_a_device_neither_blocks_nor_counts")
{
    Rig r;
    r.add(State::Idle, "Unassigned");
    REQUIRE_FALSE(r.arb.onCommandPre(42230, kProj));
    REQUIRE(r.arb.last() == Decision::NoActiveInstance);
    REQUIRE(g_var == 0);
    r.add(State::Locked, "Bass");
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    REQUIRE(r.arb.last() == Decision::Armed);
    REQUIRE(r.messages.empty());
}

// S6 review C1: the open dialog keeps its own copy of the settings (2) and pushes it into the project whenever a render starts
// (F0.4); a render run from a shortcut while it is open must not restore under it — only the tick, once the window is gone.
TEST_CASE("arbiter_render_inside_the_open_dialog_keeps_the_arm_until_the_window_closes")
{
    Rig r;
    r.add(State::Locked, "Bass");
    r.window = true;
    REQUIRE_FALSE(r.arb.onCommandPre(40015, kProj));
    r.arb.onCommandPost(40015, kProj);
    REQUIRE(g_var == 2);
    r.now += 1000; for (auto& v : r.views) v.lastProcessedMs = r.now;   // still processing during the dialog
    REQUIRE_FALSE(r.arb.onCommandPre(42230, kProj));
    REQUIRE(r.arb.last() == Decision::Armed);
    r.arb.onCommandPost(42230, kProj);
    REQUIRE(g_var == 2);                  // the dialog is still open: hold
    r.arb.tick();
    REQUIRE(g_var == 2);
    r.window = false;
    r.arb.tick();
    REQUIRE(g_var == 0);
    REQUIRE(r.arb.armCount() == 1);
}

// S6 review C1: the dialog is found by identity (the window that appears across 40015), not only by its English title
TEST_CASE("arbiter_brackets_the_render_dialog_command_for_window_tracking")
{
    Rig r;
    r.add(State::Locked, "Bass");
    std::vector<std::string> calls;
    r.arb.dialogOpening = [&] { calls.push_back("opening"); };
    r.arb.dialogOpened = [&] { calls.push_back("opened"); };
    r.arb.onCommandPre(41824, kProj); r.arb.onCommandPost(41824, kProj);
    REQUIRE(calls.empty());
    r.now += 1000; r.window = true; for (auto& v : r.views) v.lastProcessedMs = r.now;
    r.arb.onCommandPre(40015, kProj); r.arb.onCommandPost(40015, kProj);
    REQUIRE(calls == std::vector<std::string> { "opening", "opened" });
}

// S6 review I1: the last instance removed while the dialog holds the arm — the arbiter's end restores
TEST_CASE("arbiter_destroyed_while_armed_restores_the_value")
{
    {
        Rig r;
        r.add(State::Locked, "Bass");
        r.window = true;
        r.arb.onCommandPre(40015, kProj); r.arb.onCommandPost(40015, kProj);
        REQUIRE(g_var == 2);
    }
    REQUIRE(g_var == 0);
}

TEST_CASE("arbiter_destroyed_while_armed_leaves_a_user_changed_value")
{
    {
        Rig r;
        r.add(State::Locked, "Bass");
        r.window = true;
        r.arb.onCommandPre(40015, kProj); r.arb.onCommandPost(40015, kProj);
        g_var = 1;
    }
    REQUIRE(g_var == 1);
}

// Final review I1: settings.json "keepOnlineFallback" must reach the per-process arbiter (spec §10 implemented fallback).
TEST_CASE("arbiter_keep_online_fallback_from_settings_json")
{
    namespace fs = std::filesystem;
    const fs::path d = fs::temp_directory_path() / "ni_ut_keep_online";
    fs::remove_all(d);
    fs::create_directories(d);
    { std::ofstream f(d / "settings.json"); f << R"({"keepOnlineFallback":true})"; }
    ni::plugin::DeviceLibrary lib(d);
    lib.load();
    Rig r;
    r.add(State::Locked, "Bass");
    ni::plugin::applySettings(lib.settings(), r.arb);
    g_var = 1;
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 2);   // keep-online fallback active from the settings file
    ni::plugin::applySettings(ni::plugin::Settings{}, r.arb);   // default file: fallback off again
    g_var = 1;
    r.now += 1000;
    r.views[0].lastProcessedMs = r.now;
    REQUIRE_FALSE(r.arb.onCommandPre(41824, kProj));
    r.arb.onCommandPost(41824, kProj);
    REQUIRE(g_var == 1);
    fs::remove_all(d);
}
