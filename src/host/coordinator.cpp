#include "host/coordinator.hpp"
#include "host/diagnostics.hpp"
#include <chrono>
#include <mutex>

namespace ni::host
{
namespace
{
std::mutex& coordMutex() { static std::mutex m; return m; }
std::weak_ptr<Coordinator>& coordWeak() { static std::weak_ptr<Coordinator> w; return w; }
HookDispatcher& dispatcher() { static HookDispatcher* d = new HookDispatcher(); return *d; }   // never destroyed
}

int64_t Coordinator::steadyMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Coordinator::Coordinator(const ReaperApi& api) : api_(api), scheduler_(&steadyMs), hostSettings_(api_)
{
    dispatcher().onHostEvent = [this](engine::Reason r) { deliver(-1, Event { Event::Kind::Host, r, 0, {} }); };
    pairs_.onPromoted = [this](int id) { deliver(id, Event { Event::Kind::Promoted, engine::Reason::ConflictResolved, 0, {} }); };
    scheduler_.onStart = [this](const MeasureRequest& r) { deliver(r.instanceId, Event { Event::Kind::MeasureStart, r.reason, r.generation, {} }); };
    hostSettings_.onViolation = [this](int id, std::string track) { deliver(id, Event { Event::Kind::Violation, engine::Reason::AnticipativeOn, 0, std::move(track) }); };
    hostSettings_.onViolationCleared = [this](int id) { deliver(id, Event { Event::Kind::ViolationCleared, engine::Reason::AnticipativeOn, 0, {} }); };
    arbiter_ = std::make_unique<RenderArbiter>(api_, &Coordinator::steadyMs);
    arbiter_->instances = [this] {
        std::vector<RenderArbiter::InstanceView> v;
        v.reserve(renderViews_.size());
        for (auto& r : renderViews_) v.push_back(r.second());
        return v;
    };
    arbiter_->onMessage = [this](std::string m) {
        DiagLog::get().text("RENDER", -1, "%s", m.c_str());
        if (api_.ShowConsoleMsg != nullptr) api_.ShowConsoleMsg(("NovaInsert: " + m + "\n").c_str());   // F1 §10 message
    };
    arbiter_->onTrace = [](std::string t) { DiagLog::get().text("RARB", -1, "%s", t.c_str()); };   // f3_6 evidence
}

void Coordinator::setRenderView(int instanceId, std::function<RenderArbiter::InstanceView()> view)
{
    clearRenderView(instanceId);
    renderViews_.emplace_back(instanceId, std::move(view));
}

void Coordinator::clearRenderView(int instanceId)
{
    for (auto it = renderViews_.begin(); it != renderViews_.end();) it = it->first == instanceId ? renderViews_.erase(it) : it + 1;
}

void Coordinator::deliver(int instanceId, const Event& e)
{
    const auto copy = listeners_;   // a listener may add/remove listeners while being called
    for (auto& l : copy) if (instanceId < 0 || l.first == instanceId) l.second(e);
}

std::shared_ptr<Coordinator> Coordinator::acquire(const ReaperApi& api)
{
    std::lock_guard<std::mutex> l(coordMutex());
    if (auto sp = coordWeak().lock()) return sp;
    std::shared_ptr<Coordinator> sp(new Coordinator(api));
    coordWeak() = sp;
    sp->refreshChannelCounts();
    const bool ok = dispatcher().install(sp->api_);
    DiagLog::get().text(ok ? "HOOK_INSTALL" : "HOOK_INSTALL_FAIL", -1, "pid=%ld|instances=1", static_cast<long>(processId()));
    sp->arbiter_->install();   // hookcommand/hookpostcommand (one arbiter per process; no-op without plugin_register)
    return sp;
}

Coordinator::~Coordinator()
{
    // Last instance released (message thread). The weak_ptr is already expired, so a concurrent acquire()
    // constructs a fresh coordinator after we return; the dispatcher is shared and merely re-installed.
    std::lock_guard<std::mutex> l(coordMutex());
    if (!coordWeak().expired()) return;
    dispatcher().onHostEvent = nullptr;   // captured `this` dies with us   // a newer coordinator was acquired while we waited for the lock: it owns the hook now
    if (dispatcher().installed())
    {
        dispatcher().uninstall();
        DiagLog::get().text("HOOK_UNINSTALL", -1, "pid=%ld|instances=0", static_cast<long>(processId()));
    }
    DiagLog::get().disable();   // joins the writer thread: no thread may outlive the last instance (module unload at quit)
}

HookDispatcher& Coordinator::hooks() noexcept { return dispatcher(); }

void Coordinator::addListener(int instanceId, std::function<void(const Event&)> onEvent)
{
    removeListener(instanceId);
    listeners_.emplace_back(instanceId, std::move(onEvent));
}

void Coordinator::removeListener(int instanceId)
{
    for (auto it = listeners_.begin(); it != listeners_.end();) it = it->first == instanceId ? listeners_.erase(it) : it + 1;
}

void Coordinator::refreshChannelCounts()
{
    const int in = api_.GetNumAudioInputs != nullptr ? api_.GetNumAudioInputs() : 0;
    const int out = api_.GetNumAudioOutputs != nullptr ? api_.GetNumAudioOutputs() : 0;
    dispatcher().setChannelCounts(in, out);
}

void Coordinator::tick(int64_t nowMs)
{
    refreshChannelCounts();   // device re-init (Audio_Init / preferences) changes the counts
    dispatcher().drainEvents();
    scheduler_.tick();
    if (nowMs - lastRecheckMs_ >= kRecheckMs) { lastRecheckMs_ = nowMs; hostSettings_.recheck(); }
    if (nowMs - lastRenderTickMs_ >= kRenderTickMs) { lastRenderTickMs_ = nowMs; arbiter_->tick(); }
}

uint32_t Coordinator::nextGeneration() noexcept
{
    uint32_t g = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g == 0) g = generation_.fetch_add(1, std::memory_order_relaxed) + 1;   // skip 0 on wrap
    return g;
}

int Coordinator::instanceCount() const
{
    std::lock_guard<std::mutex> l(coordMutex());
    return static_cast<int>(coordWeak().use_count());
}

} // namespace ni::host
