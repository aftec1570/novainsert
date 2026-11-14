// CorrelatorWorker – the process-wide measurement correlation thread (F1 §8 "no correlation on the audio thread"; plan
// Task 16). One low-priority std::thread ("NovaInsert correlator") owns the Correlator and analyses jobs FIFO; each result is
// handed to the message thread through the injected post function (production: juce::MessageManager::callAsync), never
// delivered from the worker itself. JUCE-free so it is unit tested directly.
// Client: one per Measurer (CorrelationWorker). Its destructor (message thread) drops its queued jobs, waits for its running
// job – the capture buffers are the Measurer's – and turns every already-posted result into a no-op.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include "engine/correlator.hpp"
#include "engine/probe.hpp"
#include "plugin/measurer.hpp"

namespace ni::plugin
{

class CorrelatorWorker
{
public:
    using Post = std::function<void(std::function<void()>)>;
    using Deliver = std::function<void(uint64_t token, const engine::CorrResult& l, const engine::CorrResult& r)>;

    explicit CorrelatorWorker(Post post);
    ~CorrelatorWorker();   // joins; every Client must be gone (each holds a shared_ptr)
    CorrelatorWorker(const CorrelatorWorker&) = delete;
    CorrelatorWorker& operator=(const CorrelatorWorker&) = delete;

    // Process-wide instance: the first caller's post function is used; released with the last holder.
    static std::shared_ptr<CorrelatorWorker> acquire(Post post);
    size_t pendingJobs() const;   // queued + running

private:
    struct Alive { Deliver deliver; bool alive = true; };   // message thread only

public:
    class Client final : public CorrelationWorker
    {
    public:
        Client(std::shared_ptr<CorrelatorWorker> worker, Deliver deliver);   // deliver runs on the message thread
        ~Client() override;
        void submit(const CorrJob& job) noexcept override;
    private:
        std::shared_ptr<CorrelatorWorker> worker_;
        std::shared_ptr<Alive> alive_;
    };

private:
    struct Item { CorrJob job; const void* owner; std::shared_ptr<Alive> alive; };
    void run();
    void dropAndWait(const void* owner);

    Post post_;
    const engine::ProbeGenerator probe_ { engine::ProbeSpec::click() };   // must match the Measurer's probe
    engine::Correlator corr_ { probe_ };
    mutable std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<Item> q_;
    const void* running_ = nullptr;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace ni::plugin
