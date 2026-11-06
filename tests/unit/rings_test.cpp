#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "engine/rings.hpp"
#include "engine/types.hpp"
#include "rt/alloc_guard.hpp"

TEST_CASE("spsc push/pop order and overflow")
{
    ni::engine::Spsc<int, 4> q;
    for (int i = 0; i < 4; ++i) REQUIRE(q.push(i));
    REQUIRE_FALSE(q.push(99));
    REQUIRE(q.size() == 4);
    int v = -1;
    for (int i = 0; i < 4; ++i) { REQUIRE(q.pop(v)); REQUIRE(v == i); }
    REQUIRE_FALSE(q.pop(v));
    REQUIRE(q.size() == 0);
}

TEST_CASE("spsc peek and drop")
{
    ni::engine::Spsc<int, 4> q;
    REQUIRE(q.peek() == nullptr);
    q.push(7); q.push(8);
    REQUIRE(*q.peek() == 7);
    q.drop();
    REQUIRE(*q.peek() == 8);
}

TEST_CASE("mpsc 4 producers x 100k no lost no torn")
{
    struct Rec { int tid; int seq; };
    static ni::engine::Mpsc<Rec, 1 << 16> q;   // static: 64k cells of Rec live outside the stack
    constexpr int kProducers = 4, kPerProducer = 100000;
    std::vector<std::thread> producers;
    for (int t = 0; t < kProducers; ++t)
        producers.emplace_back([t] { for (int s = 0; s < kPerProducer; ++s) while (!q.push({t, s})) std::this_thread::yield(); });
    int next[kProducers] = {0, 0, 0, 0}; long total = 0; bool torn = false;
    while (total < (long) kProducers * kPerProducer)
    {
        Rec r{};
        if (!q.pop(r)) { std::this_thread::yield(); continue; }
        if (r.tid < 0 || r.tid >= kProducers || r.seq != next[r.tid]) { torn = true; break; }
        ++next[r.tid]; ++total;
    }
    for (auto& p : producers) p.join();
    REQUIRE_FALSE(torn);
    REQUIRE(total == (long) kProducers * kPerProducer);
    for (int t = 0; t < kProducers; ++t) REQUIRE(next[t] == kPerProducer);
}

TEST_CASE("mpsc overflow returns false and keeps earlier records")
{
    ni::engine::Mpsc<int, 4> q;
    for (int i = 0; i < 4; ++i) REQUIRE(q.push(i));
    REQUIRE_FALSE(q.push(99));
    int v = -1; for (int i = 0; i < 4; ++i) { REQUIRE(q.pop(v)); REQUIRE(v == i); }
    REQUIRE_FALSE(q.pop(v));
}

TEST_CASE("rings do not allocate after construction")
{
    static ni::engine::Spsc<ni::engine::Frame, 1024> s;
    static ni::engine::Mpsc<ni::engine::Frame, 1024> m;
    AllocGuard g;
    ni::engine::Frame f{};
    REQUIRE_NO_ALLOC(s.push(f));
    REQUIRE_NO_ALLOC(s.pop(f));
    REQUIRE_NO_ALLOC(m.push(f));
    REQUIRE_NO_ALLOC(m.pop(f));
}

TEST_CASE("alloc guard counts a heap allocation")
{
    AllocGuard g;
    std::vector<int>* v = new std::vector<int>(16);
    REQUIRE(AllocGuard::count() >= 1);
    delete v;
}

// ---- S0 review fix pass (H1, H2, M1, M4) ----

TEST_CASE("spsc size() observed from a third thread never exceeds capacity")
{
    static ni::engine::Spsc<int, 16> q;
    std::atomic<bool> stop { false };
    std::atomic<int> bad { 0 };
    std::thread producer([&] { int i = 0; while (!stop.load()) { q.push(i++); } });
    std::thread consumer([&] { int v; while (!stop.load()) { q.pop(v); } });
    std::thread observer([&] { while (!stop.load()) { if (q.size() > q.capacity()) bad.fetch_add(1); } });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true);
    producer.join(); consumer.join(); observer.join();
    REQUIRE(bad.load() == 0);
}

TEST_CASE("spsc drop() on an empty ring is a no-op and does not brick the ring")
{
    ni::engine::Spsc<int, 4> q;
    q.drop();
    REQUIRE(q.size() == 0);
    REQUIRE(q.push(1));
    int v = 0;
    REQUIRE(q.pop(v));
    REQUIRE(v == 1);
    q.push(2); q.drop(); q.drop();
    REQUIRE(q.size() == 0);
    REQUIRE(q.push(3));
}

TEST_CASE("spsc two threads, many laps, no lost or reordered elements")
{
    static ni::engine::Spsc<int, 8> q;
    constexpr int N = 200000;
    std::thread producer([&] { for (int i = 0; i < N;) if (q.push(i)) ++i; });
    int expect = 0, v = 0;
    while (expect < N) if (q.pop(v)) { REQUIRE(v == expect); ++expect; }
    producer.join();
    REQUIRE(q.size() == 0);
}

TEST_CASE("mpsc many laps single producer keeps order")
{
    ni::engine::Mpsc<int, 4> q;
    int v = 0;
    for (int i = 0; i < 400; ++i) { REQUIRE(q.push(i)); REQUIRE(q.pop(v)); REQUIRE(v == i); }
}

TEST_CASE("alloc guard reports whether a guard is active")
{
    REQUIRE_FALSE(AllocGuard::active());
    { AllocGuard g; REQUIRE(AllocGuard::active()); }
    REQUIRE_FALSE(AllocGuard::active());
}

TEST_CASE("spsc: producer and consumer indices live on separate cache lines")
{
    // The hook thread writes head_ (push) while processBlock writes tail_ (pop) for every sample; sharing one line makes
    // each release-store invalidate the other core's line (review 2026-11-04).
    using Q = ni::engine::Spsc<int, 4>;
    STATIC_REQUIRE(alignof(Q) >= 64);
    STATIC_REQUIRE(sizeof(Q) >= 3 * 64);   // buf_, head_, tail_ each start a new line
}
