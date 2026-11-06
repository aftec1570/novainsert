#include "host/scheduler.hpp"
#include <algorithm>

namespace ni::host
{

Scheduler::Scheduler(std::function<int64_t()> nowMs) : now_(std::move(nowMs)) {}

void Scheduler::setMaxConcurrent(int n) { maxConcurrent_ = n < 0 ? 0 : n; }

bool Scheduler::isRunning(int instanceId) const
{
    return std::any_of(running_.begin(), running_.end(), [&](const MeasureRequest& r) { return r.instanceId == instanceId; });
}

void Scheduler::request(MeasureRequest r)
{
    const int64_t now = now_();
    int64_t firstAt = now;
    // A request already waiting in the queue goes back into the coalescing window (newest generation wins).
    const auto q = std::find_if(queue_.begin(), queue_.end(), [&](const Queued& e) { return e.req.instanceId == r.instanceId; });
    if (q != queue_.end()) { firstAt = q->firstAt; queue_.erase(q); }
    for (auto& p : pending_)
    {
        if (p.req.instanceId != r.instanceId) continue;
        p.req = r; p.windowEnd = now + kCoalesceMs;
        return;
    }
    pending_.push_back(Pending { r, firstAt, now + kCoalesceMs });
}

void Scheduler::enqueue(MeasureRequest r, int64_t firstAt)
{
    queue_.push_back(Queued { r, firstAt });
    std::stable_sort(queue_.begin(), queue_.end(), [](const Queued& a, const Queued& b) { return a.firstAt < b.firstAt; });
}

void Scheduler::start(const Queued& q)
{
    auto& a = attempts_[q.req.instanceId];
    if (a.generation != q.req.generation) a = Attempts { q.req.generation, 0 };
    ++a.count;
    running_.push_back(q.req);
    if (q.req.forceSerial) serialRunning_ = true;
    if (onStart) onStart(q.req);
}

void Scheduler::tick()
{
    const int64_t now = now_();
    // Quiet window elapsed and the instance is not measuring: move to the FIFO queue.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (it->windowEnd <= now && !isRunning(it->req.instanceId)) { enqueue(it->req, it->firstAt); it = pending_.erase(it); }
        else ++it;
    }
    while (!queue_.empty())
    {
        if (serialRunning_) break;
        const Queued head = queue_.front();
        if (head.req.forceSerial && !running_.empty()) break;
        if (maxConcurrent_ > 0 && static_cast<int>(running_.size()) >= maxConcurrent_) break;
        queue_.erase(queue_.begin());
        start(head);
    }
}

void Scheduler::finished(int instanceId, uint32_t generation, bool ok, engine::Reason reason)
{
    const auto r = std::find_if(running_.begin(), running_.end(), [&](const MeasureRequest& e) { return e.instanceId == instanceId; });
    if (r == running_.end() || r->generation != generation) return;   // stale or unknown result
    MeasureRequest req = *r;
    if (req.forceSerial) serialRunning_ = false;
    running_.erase(r);
    if (ok) return;
    const bool newerPending = std::any_of(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.req.instanceId == instanceId; });
    if (newerPending) return;                                           // the newer generation supersedes any retry
    const auto a = attempts_.find(instanceId);
    if (a == attempts_.end() || a->second.generation != generation || a->second.count >= kMaxAttempts) return;
    req.reason = reason;
    if (reason == engine::Reason::Crosstalk) req.forceSerial = true;
    enqueue(req, now_());                                               // retry: back of the queue, no coalescing window
}

void Scheduler::cancel(int instanceId)
{
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.req.instanceId == instanceId; }), pending_.end());
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Queued& q) { return q.req.instanceId == instanceId; }), queue_.end());
    const auto r = std::find_if(running_.begin(), running_.end(), [&](const MeasureRequest& e) { return e.instanceId == instanceId; });
    if (r != running_.end()) { if (r->forceSerial) serialRunning_ = false; running_.erase(r); }
    attempts_.erase(instanceId);
}

int Scheduler::queuePosition(int instanceId) const
{
    if (isRunning(instanceId)) return 0;
    int pos = 0;
    for (const auto& q : queue_) { ++pos; if (q.req.instanceId == instanceId) return pos; }
    for (const auto& p : pending_) { ++pos; if (p.req.instanceId == instanceId) return pos; }
    return -1;
}

} // namespace ni::host
