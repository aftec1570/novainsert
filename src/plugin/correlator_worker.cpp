// CorrelatorWorker (plan Task 16): see correlator_worker.hpp.
#include "plugin/correlator_worker.hpp"
#include "host/platform.hpp"

namespace ni::plugin
{

CorrelatorWorker::CorrelatorWorker(Post post) : post_(std::move(post)), thread_([this] { run(); }) {}

CorrelatorWorker::~CorrelatorWorker()
{
    { std::lock_guard<std::mutex> g(m_); stop_ = true; }
    cv_.notify_all();
    thread_.join();
}

std::shared_ptr<CorrelatorWorker> CorrelatorWorker::acquire(Post post)
{
    static std::mutex m;
    static std::weak_ptr<CorrelatorWorker> w;
    std::lock_guard<std::mutex> g(m);
    if (auto s = w.lock()) return s;
    auto s = std::make_shared<CorrelatorWorker>(std::move(post));
    w = s;
    return s;
}

size_t CorrelatorWorker::pendingJobs() const
{
    std::lock_guard<std::mutex> g(m_);
    return q_.size() + (running_ != nullptr ? 1u : 0u);
}

void CorrelatorWorker::run()
{
    ni::host::platform::setLowPriorityWorker("NovaInsert corr");   // low priority: never competes with audio or UI
    for (;;)
    {
        Item it;
        {
            std::unique_lock<std::mutex> g(m_);
            cv_.wait(g, [this] { return stop_ || ! q_.empty(); });
            if (stop_) return;
            it = std::move(q_.front());
            q_.pop_front();
            running_ = it.owner;
        }
        const auto l = corr_.analyse(it.job.l, it.job.n, it.job.hwBlock);
        const auto r = it.job.r != nullptr ? corr_.analyse(it.job.r, it.job.n, it.job.hwBlock) : l;
        const uint64_t token = it.job.token;
        auto alive = std::move(it.alive);
        post_([alive, token, l, r] { if (alive->alive) alive->deliver(token, l, r); });   // runs on the message thread
        {
            std::lock_guard<std::mutex> g(m_);
            running_ = nullptr;
        }
        idle_.notify_all();
    }
}

void CorrelatorWorker::dropAndWait(const void* owner)
{
    std::unique_lock<std::mutex> g(m_);
    for (auto i = q_.begin(); i != q_.end();) i = i->owner == owner ? q_.erase(i) : i + 1;
    idle_.wait(g, [this, owner] { return running_ != owner; });
}

CorrelatorWorker::Client::Client(std::shared_ptr<CorrelatorWorker> worker, Deliver deliver)
    : worker_(std::move(worker)), alive_(std::make_shared<Alive>(Alive { std::move(deliver), true })) {}

CorrelatorWorker::Client::~Client()
{
    worker_->dropAndWait(this);
    alive_->alive = false;
}

void CorrelatorWorker::Client::submit(const CorrJob& job) noexcept
{
    try
    {
        {
            std::lock_guard<std::mutex> g(worker_->m_);
            worker_->q_.push_back({ job, this, alive_ });
        }
        worker_->cv_.notify_one();
    }
    catch (...) {}   // allocation failure: the job is lost, the Measurer's attempt times out
}

} // namespace ni::plugin
