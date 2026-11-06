// Scheduler — process-wide measurement queue (F1 §11, plan Task 8). Message thread only (tick from a 50 ms timer).
//   request(): coalesces per instance — every request restarts a 500 ms quiet window and keeps the newest generation.
//   Queue order: FIFO by first request time. running ≤ maxConcurrent (0 = unlimited); a forceSerial request
//   starts only when nothing runs and blocks every other start while it runs.
//   Policy per (instance, generation): one attempt + one retry, then no more automatic requests for that
//   generation; a result for a generation that is not the running one is ignored; Reason::Crosstalk re-queues
//   the retry with forceSerial = true.
#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>
#include "engine/types.hpp"

namespace ni::host
{

struct MeasureRequest
{
    int instanceId;
    uint32_t generation;
    engine::Reason reason;
    bool forceSerial;
};

class Scheduler
{
public:
    static constexpr int64_t kCoalesceMs = 500;
    static constexpr int kMaxAttempts = 2;

    explicit Scheduler(std::function<int64_t()> nowMs);
    void setMaxConcurrent(int n);                   // 0 = unlimited
    void request(MeasureRequest r);
    void tick();
    void finished(int instanceId, uint32_t generation, bool ok, engine::Reason reason);
    void cancel(int instanceId);                    // instance destroyed / gave up its pair: drop pending, queued and running requests
    int queuePosition(int instanceId) const;        // 0 = running, -1 = unknown, else 1-based wait position
    std::function<void(const MeasureRequest&)> onStart;

private:
    struct Pending { MeasureRequest req; int64_t firstAt; int64_t windowEnd; };
    struct Queued { MeasureRequest req; int64_t firstAt; };
    struct Attempts { uint32_t generation; int count; };

    std::function<int64_t()> now_;
    int maxConcurrent_ = 0;
    std::vector<Pending> pending_;
    std::vector<Queued> queue_;
    std::vector<MeasureRequest> running_;
    bool serialRunning_ = false;
    std::unordered_map<int, Attempts> attempts_;

    bool isRunning(int instanceId) const;
    void enqueue(MeasureRequest r, int64_t firstAt);
    void start(const Queued& q);
};

} // namespace ni::host
