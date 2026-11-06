#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include "host/coordinator.hpp"
#include "host/render_arbiter.hpp"

using namespace ni::host;
using ni::engine::Reason;

namespace
{
int g_adds = 0, g_removes = 0;
int fakeRegHook(bool isAdd, audio_hook_register_t*) { if (isAdd) ++g_adds; else ++g_removes; return 1; }
ReaperApi fakeApi() { ReaperApi a; a.Audio_RegHardwareHook = &fakeRegHook; return a; }
}

TEST_CASE("coordinator: acquire twice returns the same object and counts two instances")
{
    g_adds = g_removes = 0;
    auto a = Coordinator::acquire(fakeApi());
    auto b = Coordinator::acquire(fakeApi());
    REQUIRE(a != nullptr);
    REQUIRE(a == b);
    REQUIRE(a->instanceCount() == 2);
    REQUIRE(a->hooks().installed());
    REQUIRE(g_adds == 1);
}

TEST_CASE("coordinator: last release destroys it and the hook is added once and removed once")
{
    g_adds = g_removes = 0;
    std::weak_ptr<Coordinator> w;
    {
        auto a = Coordinator::acquire(fakeApi());
        auto b = Coordinator::acquire(fakeApi());
        w = a;
        a.reset();
        REQUIRE_FALSE(w.expired());
        REQUIRE(b->instanceCount() == 1);
        REQUIRE(g_removes == 0);
    }
    REQUIRE(w.expired());
    REQUIRE(g_adds == 1);
    REQUIRE(g_removes == 1);
    // A fresh acquire after full release constructs a new coordinator and installs again.
    auto c = Coordinator::acquire(fakeApi());
    REQUIRE(c->instanceCount() == 1);
    REQUIRE(g_adds == 2);
}

TEST_CASE("coordinator: nextGeneration() is strictly increasing across 10k calls from 4 threads")
{
    auto c = Coordinator::acquire(fakeApi());
    constexpr int kThreads = 4, kPer = 10000;
    std::vector<std::vector<uint32_t>> seen(kThreads);
    std::vector<std::thread> th;
    for (int t = 0; t < kThreads; ++t)
        th.emplace_back([&, t] { seen[(size_t) t].reserve(kPer); for (int i = 0; i < kPer; ++i) seen[(size_t) t].push_back(c->nextGeneration()); });
    for (auto& x : th) x.join();
    std::vector<uint32_t> all;
    for (auto& s : seen)
    {
        REQUIRE(std::is_sorted(s.begin(), s.end()));
        all.insert(all.end(), s.begin(), s.end());
    }
    std::sort(all.begin(), all.end());
    REQUIRE(std::adjacent_find(all.begin(), all.end()) == all.end());
    REQUIRE(all.size() == (size_t) kThreads * kPer);
    REQUIRE(all.front() >= 1);   // generation 0 is reserved for "never measured"
}

namespace
{
audio_hook_register_t* g_reg = nullptr;
int capturingRegHook(bool isAdd, audio_hook_register_t* r) { if (isAdd) g_reg = r; return 1; }
ReaSample g_buf[4][256];
ReaSample* fakeBuf(bool, int idx) { return idx >= 0 && idx < 4 ? g_buf[idx] : nullptr; }
void feed(int len) { HookDispatcher::onAudioBuffer(false, len, 48000.0, g_reg); HookDispatcher::onAudioBuffer(true, len, 48000.0, g_reg); }
}

TEST_CASE("coordinator: tick() delivers each host event to every listener once; a removed listener gets nothing")
{
    ReaperApi a; a.Audio_RegHardwareHook = &capturingRegHook;
    auto c = Coordinator::acquire(a);
    REQUIRE(g_reg != nullptr);
    g_reg->input_nch = 4; g_reg->output_nch = 4; g_reg->GetBuffer = &fakeBuf;
    c->hooks().setExpectedBlock(64);         // device BSIZE: BufferChange = hook len != expected
    std::vector<Reason> e1, e2;
    c->addListener(1, [&](const Coordinator::Event& e) { e1.push_back(e.reason); });
    c->addListener(2, [&](const Coordinator::Event& e) { e2.push_back(e.reason); });
    feed(64); feed(64); feed(128);          // length change -> BufferChange
    c->tick(1000);
    REQUIRE(e1 == std::vector<Reason> { Reason::BufferChange });
    REQUIRE(e2 == std::vector<Reason> { Reason::BufferChange });
    c->removeListener(2);
    feed(256);                              // another change
    c->tick(1050);
    REQUIRE(e1.size() == 2);
    REQUIRE(e2.size() == 1);
    (void) c->hostSettings();
}

TEST_CASE("coordinator: pair promotion and measurement start reach only the named instance")
{
    ReaperApi a; a.Audio_RegHardwareHook = &capturingRegHook;
    auto c = Coordinator::acquire(a);
    std::vector<std::pair<int, Coordinator::Event>> got;
    c->addListener(1, [&](const Coordinator::Event& e) { got.emplace_back(1, e); });
    c->addListener(2, [&](const Coordinator::Event& e) { got.emplace_back(2, e); });
    const Pair p { { 8, 9 }, { 8, 9 }, true };
    REQUIRE(c->pairs().claim(1, p, "A") == Claim::Owner);
    REQUIRE(c->pairs().claim(2, p, "B") == Claim::Conflict);
    c->pairs().release(1);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].first == 2); REQUIRE(got[0].second.kind == Coordinator::Event::Kind::Promoted); REQUIRE(got[0].second.reason == Reason::ConflictResolved);
    c->scheduler().request({ 2, 7, Reason::ProjectLoad, false });
    for (int i = 0; i < 20; ++i) c->tick(2000 + i * 50);   // > 500 ms coalescing window with a real clock inside the scheduler
    // the scheduler's own clock is steady_clock: wait the window out
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(600)) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); c->tick(3000); }
    REQUIRE(got.size() == 2);
    REQUIRE(got[1].first == 2); REQUIRE(got[1].second.kind == Coordinator::Event::Kind::MeasureStart); REQUIRE(got[1].second.generation == 7);
    c->removeListener(1); c->removeListener(2); c->pairs().release(2);
}

namespace
{
int fakeNumIn() { return 4; }
int fakeNumOut() { return 4; }
}

TEST_CASE("coordinator: hook channel counts come from GetNumAudioInputs/Outputs and a zeroed register raises no DeviceLost")
{
    ReaperApi a; a.Audio_RegHardwareHook = &capturingRegHook; a.GetNumAudioInputs = &fakeNumIn; a.GetNumAudioOutputs = &fakeNumOut;
    auto c = Coordinator::acquire(a);
    REQUIRE(g_reg != nullptr);
    g_reg->input_nch = 0; g_reg->output_nch = 0; g_reg->GetBuffer = &fakeBuf;   // what REAPER 7.83 hands us
    std::vector<Reason> got;
    c->addListener(1, [&](const Coordinator::Event& e) { got.push_back(e.reason); });
    for (int i = 0; i < 6; ++i) feed(64);
    c->tick(1000);
    REQUIRE(got.empty());
    REQUIRE(c->hooks().stats().nullBufferStreak == 0);
    REQUIRE(c->hooks().stats().inputChannels == 4);
    c->removeListener(1);
}

TEST_CASE("coordinator: a cleared anticipative violation reaches only the named instance as ViolationCleared")
{
    ReaperApi a; a.Audio_RegHardwareHook = &capturingRegHook;
    auto c = Coordinator::acquire(a);
    std::vector<std::pair<int, Coordinator::Event::Kind>> got;
    c->addListener(1, [&](const Coordinator::Event& e) { got.emplace_back(1, e.kind); });
    c->addListener(2, [&](const Coordinator::Event& e) { got.emplace_back(2, e.kind); });
    REQUIRE(c->hostSettings().onViolationCleared);
    c->hostSettings().onViolationCleared(2);
    REQUIRE(got.size() == 1);
    REQUIRE(got[0].first == 2);
    REQUIRE(got[0].second == Coordinator::Event::Kind::ViolationCleared);
    c->removeListener(1); c->removeListener(2);
}

// Task 21 (F1 §10): the Coordinator owns the one RenderArbiter; its preflight sees every registered instance view, and the
// 250 ms message-thread tick drives the dialog/project restore.
namespace
{
int g_renderVar = 0;
ReaProject* const kRenderProj = reinterpret_cast<ReaProject*>(0x2000);
int renderOffs(const char* name, int* sz) { if (sz) *sz = 4; return std::string(name) == "projrenderlimit" ? 3 : -1; }
void* renderAddr(ReaProject* p, int idx) { return p == kRenderProj && idx == 3 ? &g_renderVar : nullptr; }
ReaProject* renderEnum(int, char*, int) { return kRenderProj; }
bool renderValid(ReaProject*, void* p, const char*) { return p == kRenderProj; }
ReaperApi renderApi()
{
    ReaperApi a = fakeApi();
    a.projectconfig_var_getoffs = &renderOffs; a.projectconfig_var_addr = &renderAddr; a.EnumProjects = &renderEnum; a.ValidatePtr2 = &renderValid;
    return a;
}
RenderArbiter::InstanceView view(int id, ni::engine::State s)
{
    return { id, s, ni::engine::Mode::Insert, ni::host::Coordinator::steadyMs(), false, "T" + std::to_string(id) };
}
} // namespace

TEST_CASE("coordinator: render preflight sees registered instance views; a cleared view is no longer involved")
{
    auto c = Coordinator::acquire(renderApi());
    g_renderVar = 0;
    c->setRenderView(1, [] { return view(1, ni::engine::State::Locked); });
    c->setRenderView(2, [] { return view(2, ni::engine::State::Measuring); });
    REQUIRE(c->render().onCommandPre(42230, nullptr));            // T2 not locked -> blocked
    REQUIRE(c->render().last() == RenderArbiter::Decision::RefusedNotLocked);
    c->clearRenderView(2);
    REQUIRE_FALSE(c->render().onCommandPre(41824, nullptr));      // only the locked one left -> armed
    REQUIRE(g_renderVar == RenderArbiter::kOnline);
    c->render().onCommandPost(41824, nullptr);
    REQUIRE(g_renderVar == 0);
    c->clearRenderView(1);
}

TEST_CASE("coordinator: tick restores a dialog arming once the render window is gone")
{
    auto c = Coordinator::acquire(renderApi());
    g_renderVar = 1;
    bool window = true;
    c->render().renderWindowOpen = [&window] { return window; };
    c->setRenderView(1, [] { return view(1, ni::engine::State::Locked); });
    REQUIRE_FALSE(c->render().onCommandPre(RenderArbiter::kDialogCmd, nullptr));
    REQUIRE(g_renderVar == RenderArbiter::kOnline);
    c->tick(1000);
    REQUIRE(g_renderVar == RenderArbiter::kOnline);   // dialog still open
    window = false;
    c->tick(1300);                                    // >= 250 ms later
    REQUIRE(g_renderVar == 1);
    c->clearRenderView(1);
    c->render().renderWindowOpen = nullptr;
}
