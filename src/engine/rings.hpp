// NovaInsert engine core – bounded lock-free rings (F1 §1 thread model, §14 RT-safety; v3 §19 queue contracts).
// Header-only, dependency-free. Ported from the F0.6 spike (0 lost/torn records in every gated cell at 32–2048).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ni::engine
{
// Spsc<T, CAP> – single-producer / single-consumer ring.
//   producer: exactly one thread calls push();  consumer: exactly one thread calls pop()/peek()/drop(); size() any thread
//   (a snapshot: exact on the producer/consumer thread, bounded to [0, CAP] elsewhere). drop() on an empty ring is a no-op.
//   capacity: CAP elements (power of two), storage inline (no heap after construction).
//   overflow: push() returns false and writes nothing – the caller counts the drop.
//   memory order: head/tail are release-stored by their owner and acquire-loaded by the other side, so the element write
//   happens-before the consumer's read of head and the element read happens-before the producer's read of tail.
//   lifecycle: trivially constructible T; no destructor work; no reset – construct a new ring when both sides are quiescent.
template <typename T, std::size_t CAP>
class Spsc
{
    static_assert((CAP & (CAP - 1)) == 0 && CAP > 0, "CAP must be a power of two");
public:
    bool push(const T& v) noexcept
    {
        const auto h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= CAP) return false;
        buf_[h & (CAP - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& v) noexcept
    {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return false;
        v = buf_[t & (CAP - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    const T* peek() const noexcept
    {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == t) return nullptr;
        return &buf_[t & (CAP - 1)];
    }
    void drop() noexcept   // consumer only; no-op when empty (safe without a preceding peek())
    {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) != t) tail_.store(t + 1, std::memory_order_release);
    }
    std::size_t size() const noexcept   // any thread: tail first (so h >= t, no wrap), clamped (a concurrent pop between
    {                                   // the two loads can make h - t exceed CAP by the number of elements popped)
        const auto t = tail_.load(std::memory_order_acquire);
        const auto h = head_.load(std::memory_order_acquire);
        const auto n = h - t;
        return n > CAP ? CAP : static_cast<std::size_t>(n);
    }
    static constexpr std::size_t capacity() noexcept { return CAP; }

private:
    // The producer writes head_ and the consumer writes tail_ once per element from different threads (hook / processBlock):
    // each index on its own cache line, and the buffer off both, so neither side's release-store invalidates the other's line.
    alignas(64) T buf_[CAP] {};
    alignas(64) std::atomic<uint64_t> head_ { 0 };
    alignas(64) std::atomic<uint64_t> tail_ { 0 };
};

// Mpsc<T, CAP> – multi-producer / single-consumer bounded ring (Vyukov sequence-per-cell design).
//   producers: any number of threads call push() concurrently (CAS on head_);  consumer: exactly one thread calls pop().
//   capacity: CAP elements (power of two), inline storage.
//   overflow: push() returns false when the ring is full (no wait, no overwrite) – the caller counts the drop.
//   memory order: a producer claims a cell with a relaxed CAS, writes the element, then release-stores seq = pos + 1; the
//   consumer acquire-loads seq, reads the element, then release-stores seq = pos + CAP to hand the cell back.
//   lifecycle: trivially constructible T; construction initialises every cell's seq; no reset – construct a new ring.
template <typename T, std::size_t CAP>
class Mpsc
{
    static_assert((CAP & (CAP - 1)) == 0 && CAP > 0, "CAP must be a power of two");
public:
    Mpsc() noexcept { for (std::size_t i = 0; i < CAP; ++i) buf_[i].seq.store(i, std::memory_order_relaxed); }

    bool push(const T& v) noexcept
    {
        auto pos = head_.load(std::memory_order_relaxed);
        for (;;)
        {
            Cell& c = buf_[pos & (CAP - 1)];
            const auto seq = c.seq.load(std::memory_order_acquire);
            const auto dif = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
            if (dif == 0)
            {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    c.v = v;
                    c.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0) return false;          // full
            else pos = head_.load(std::memory_order_relaxed);
        }
    }
    bool pop(T& v) noexcept
    {
        Cell& c = buf_[tail_ & (CAP - 1)];
        const auto seq = c.seq.load(std::memory_order_acquire);
        if (static_cast<int64_t>(seq) - static_cast<int64_t>(tail_ + 1) < 0) return false;
        v = c.v;
        c.seq.store(tail_ + CAP, std::memory_order_release);
        ++tail_;
        return true;
    }
    static constexpr std::size_t capacity() noexcept { return CAP; }

private:
    struct Cell { std::atomic<uint64_t> seq; T v; };
    Cell buf_[CAP];
    std::atomic<uint64_t> head_ { 0 };
    uint64_t tail_ = 0;
};
} // namespace ni::engine
