#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "host/scheduler.hpp"

using namespace ni::host;
using ni::engine::Reason;

namespace
{
struct Fx
{
    int64_t now = 0;
    Scheduler s { [this] { return now; } };
    std::vector<MeasureRequest> started;
    Fx() { s.onStart = [this](const MeasureRequest& r) { started.push_back(r); }; }
    void advance(int64_t ms, int64_t step = 50) { const int64_t end = now + ms; while (now < end) { now += step; s.tick(); } }
};
}

TEST_CASE("coalescing: 5 requests in 300 ms start once after 500 ms quiet with newest generation")
{
    Fx f;
    for (uint32_t g = 1; g <= 5; ++g) { f.s.request(MeasureRequest { 1, g, Reason::RoutingChange, false }); f.advance(75); }
    REQUIRE(f.started.empty());                       // still inside the quiet window
    f.advance(450);
    REQUIRE(f.started.size() == 1);
    REQUIRE(f.started[0].generation == 5);
    REQUIRE(f.started[0].instanceId == 1);
    f.advance(2000);
    REQUIRE(f.started.size() == 1);
}

TEST_CASE("fifo with max 4: 20 requests -> 4 running, positions 1..16 reported")
{
    Fx f; f.s.setMaxConcurrent(4);
    for (int i = 1; i <= 20; ++i) { f.s.request(MeasureRequest { i, 1, Reason::ProjectLoad, false }); f.advance(10, 10); }
    f.advance(600);
    REQUIRE(f.started.size() == 4);
    for (int i = 0; i < 4; ++i) REQUIRE(f.started[(size_t) i].instanceId == i + 1);
    for (int i = 5; i <= 20; ++i) REQUIRE(f.s.queuePosition(i) == i - 4);
    REQUIRE(f.s.queuePosition(1) == 0);               // running
    REQUIRE(f.s.queuePosition(99) == -1);             // unknown
    f.s.finished(1, 1, true, Reason::None);
    f.advance(100);
    REQUIRE(f.started.size() == 5);
    REQUIRE(f.started[4].instanceId == 5);
    REQUIRE(f.s.queuePosition(6) == 1);
}

TEST_CASE("unlimited (0): 20 requests start in one tick")
{
    Fx f; f.s.setMaxConcurrent(0);
    for (int i = 1; i <= 20; ++i) f.s.request(MeasureRequest { i, 1, Reason::ProjectLoad, false });
    f.advance(600);
    REQUIRE(f.started.size() == 20);
}

TEST_CASE("retry once per generation then stop")
{
    Fx f;
    f.s.request(MeasureRequest { 1, 7, Reason::DeviceSelection, false });
    f.advance(600);
    REQUIRE(f.started.size() == 1);
    f.s.finished(1, 7, false, Reason::LowSnr);        // attempt failed -> one automatic retry
    f.advance(600);
    REQUIRE(f.started.size() == 2);
    REQUIRE(f.started[1].generation == 7);
    f.s.finished(1, 7, false, Reason::LowSnr);        // retry failed -> no more automatic requests for gen 7
    f.advance(2000);
    REQUIRE(f.started.size() == 2);
    f.s.request(MeasureRequest { 1, 8, Reason::DeviceSelection, false });   // a new generation starts fresh
    f.advance(600);
    REQUIRE(f.started.size() == 3);
}

TEST_CASE("stale generation result ignored")
{
    Fx f;
    f.s.request(MeasureRequest { 1, 3, Reason::RoutingChange, false });
    f.advance(600);
    REQUIRE(f.started.size() == 1);
    f.s.request(MeasureRequest { 1, 4, Reason::RoutingChange, false });     // newer generation queued while 3 runs
    f.s.finished(1, 3, false, Reason::Timeout);       // result for the stale generation: no retry of 3
    f.advance(600);
    REQUIRE(f.started.size() == 2);
    REQUIRE(f.started[1].generation == 4);
    f.s.finished(1, 2, false, Reason::Timeout);       // a result for a generation that never ran: ignored
    f.advance(2000);
    REQUIRE(f.started.size() == 2);
}

TEST_CASE("crosstalk reason requeues serial and waits for running probes to finish")
{
    Fx f; f.s.setMaxConcurrent(4);
    for (int i = 1; i <= 3; ++i) f.s.request(MeasureRequest { i, 1, Reason::ProjectLoad, false });
    f.advance(600);
    REQUIRE(f.started.size() == 3);
    f.s.finished(1, 1, false, Reason::Crosstalk);     // instance 1 saw crosstalk: re-queued once, serial
    f.advance(600);
    REQUIRE(f.started.size() == 3);                   // 2 and 3 still running -> the serial request waits
    f.s.finished(2, 1, true, Reason::None);
    f.advance(100);
    REQUIRE(f.started.size() == 3);
    f.s.finished(3, 1, true, Reason::None);
    f.advance(100);
    REQUIRE(f.started.size() == 4);
    REQUIRE(f.started[3].instanceId == 1);
    REQUIRE(f.started[3].forceSerial);
    f.s.request(MeasureRequest { 4, 1, Reason::ProjectLoad, false });      // nothing else starts while a serial probe runs
    f.advance(600);
    REQUIRE(f.started.size() == 4);
    f.s.finished(1, 1, false, Reason::Crosstalk);     // crosstalk again on the serial attempt: no second re-queue
    f.advance(600);
    REQUIRE(f.started.size() == 5);
    REQUIRE(f.started[4].instanceId == 4);
    f.advance(2000);
    REQUIRE(f.started.size() == 5);
}

// S2 review I1: an instance that is destroyed or gives up its pair withdraws its request; a running request
// frees its maxConcurrent slot, a queued or pending request is dropped.
TEST_CASE("cancel of a running request frees a maxConcurrent slot for the next queued instance")
{
    Fx f; f.s.setMaxConcurrent(2);
    for (int i = 1; i <= 3; ++i) { f.s.request(MeasureRequest { i, 1, Reason::ProjectLoad, false }); f.advance(10, 10); }
    f.advance(600);
    REQUIRE(f.started.size() == 2);
    REQUIRE(f.s.queuePosition(3) == 1);
    f.s.cancel(1);
    REQUIRE(f.s.queuePosition(1) == -1);
    f.advance(50);
    REQUIRE(f.started.size() == 3);
    REQUIRE(f.started[2].instanceId == 3);
}

TEST_CASE("cancel of a pending or queued request removes it and a later finished() for it is ignored")
{
    Fx f; f.s.setMaxConcurrent(1);
    f.s.request(MeasureRequest { 1, 1, Reason::ProjectLoad, false });
    f.s.request(MeasureRequest { 2, 1, Reason::ProjectLoad, false });
    f.s.cancel(2);                                    // still pending (inside the quiet window)
    REQUIRE(f.s.queuePosition(2) == -1);
    f.advance(600);
    REQUIRE(f.started.size() == 1);
    f.s.request(MeasureRequest { 3, 1, Reason::ProjectLoad, false });
    f.advance(600);
    REQUIRE(f.s.queuePosition(3) == 1);               // queued behind the running instance 1
    f.s.cancel(3);
    REQUIRE(f.s.queuePosition(3) == -1);
    f.s.finished(1, 1, true, Reason::None);
    f.advance(100);
    REQUIRE(f.started.size() == 1);                   // nothing left to start
    f.s.finished(3, 1, false, Reason::Timeout);       // stale: no retry enqueued
    f.advance(100);
    REQUIRE(f.started.size() == 1);
}

TEST_CASE("cancel of a running forceSerial request lifts the serial block")
{
    Fx f;
    f.s.request(MeasureRequest { 1, 1, Reason::ProjectLoad, true });
    f.advance(600);
    REQUIRE(f.started.size() == 1);
    f.s.request(MeasureRequest { 2, 1, Reason::ProjectLoad, false });
    f.advance(600);
    REQUIRE(f.started.size() == 1);                   // blocked by the serial probe
    f.s.cancel(1);
    f.advance(50);
    REQUIRE(f.started.size() == 2);
}
