// CorrelatorWorker (plan Task 16): one process-wide low-priority thread analyses measurement captures; results return on
// the message thread through the injected post function; a client's destruction drops its queued jobs, waits for its
// running job (the capture buffers are the Measurer's) and makes any already-posted result a no-op.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "plugin/correlator_worker.hpp"

using namespace ni::engine;
using ni::plugin::CorrJob;
using ni::plugin::CorrelatorWorker;

namespace
{
struct MessageQueue   // stands in for MessageManager::callAsync
{
    std::mutex m;
    std::deque<std::function<void()>> q;
    void post(std::function<void()> f) { std::lock_guard<std::mutex> g(m); q.push_back(std::move(f)); }
    int drain()
    {
        int n = 0;
        for (;;)
        {
            std::function<void()> f;
            { std::lock_guard<std::mutex> g(m); if (q.empty()) return n; f = std::move(q.front()); q.pop_front(); }
            f(); ++n;
        }
    }
    bool waitFor(int count, int ms = 5000)
    {
        for (int t = 0; t < ms; t += 5)
        {
            { std::lock_guard<std::mutex> g(m); if (static_cast<int>(q.size()) >= count) return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

std::vector<float> returnAt(int32_t D)
{
    const ProbeGenerator g { ProbeSpec::click() };   // the probe the worker correlates against
    std::vector<float> clean(static_cast<size_t>(g.length())), x(clean.size(), 0.0f);
    g.render(clean.data(), -12.0f);
    for (size_t n = 0; n + static_cast<size_t>(D) < x.size(); ++n) x[n + static_cast<size_t>(D)] = clean[n];
    return x;
}

struct Sink { uint64_t token = 0; int32_t lagL = -1, lagR = -1; int calls = 0; };
} // namespace

TEST_CASE("correlator_worker_delivers_on_the_message_thread")
{
    MessageQueue mq;
    auto w = std::make_shared<CorrelatorWorker>([&mq](std::function<void()> f) { mq.post(std::move(f)); });
    Sink a, b;
    CorrelatorWorker::Client ca(w, [&a](uint64_t t, const CorrResult& l, const CorrResult& r) { a.token = t; a.lagL = l.lag; a.lagR = r.lag; ++a.calls; });
    CorrelatorWorker::Client cb(w, [&b](uint64_t t, const CorrResult& l, const CorrResult&) { b.token = t; b.lagL = l.lag; ++b.calls; });
    const auto x1 = returnAt(1000), x2 = returnAt(2000), x3 = returnAt(333);
    ca.submit({ 11, x1.data(), x2.data(), static_cast<int32_t>(x1.size()), 64 });
    cb.submit({ 22, x3.data(), nullptr, static_cast<int32_t>(x3.size()), 64 });
    REQUIRE(mq.waitFor(2));
    REQUIRE(a.calls == 0);   // nothing runs off the message thread
    REQUIRE(mq.drain() == 2);
    REQUIRE(a.calls == 1); REQUIRE(a.token == 11); REQUIRE(a.lagL == 1000); REQUIRE(a.lagR == 2000);
    REQUIRE(b.calls == 1); REQUIRE(b.token == 22); REQUIRE(b.lagL == 333);
}

TEST_CASE("correlator_worker_client_destruction_drops_and_waits")
{
    MessageQueue mq;
    auto w = std::make_shared<CorrelatorWorker>([&mq](std::function<void()> f) { mq.post(std::move(f)); });
    Sink a;
    const auto x = returnAt(500);
    {
        CorrelatorWorker::Client ca(w, [&a](uint64_t, const CorrResult&, const CorrResult&) { ++a.calls; });
        for (uint64_t t = 1; t <= 5; ++t) ca.submit({ t, x.data(), nullptr, static_cast<int32_t>(x.size()), 64 });
    }   // returns only once no job of this client is running; queued ones are dropped
    const auto busyAfter = w->pendingJobs();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    mq.drain();
    REQUIRE(busyAfter == 0);
    REQUIRE(a.calls == 0);   // results posted before the destruction are no-ops
}

TEST_CASE("correlator_worker_shutdown_joins")
{
    MessageQueue mq;
    auto w = std::make_shared<CorrelatorWorker>([&mq](std::function<void()> f) { mq.post(std::move(f)); });
    const auto x = returnAt(10);
    {
        CorrelatorWorker::Client c(w, [](uint64_t, const CorrResult&, const CorrResult&) {});
        c.submit({ 1, x.data(), nullptr, static_cast<int32_t>(x.size()), 64 });
    }
    w.reset();   // joins the thread
    SUCCEED();
}
