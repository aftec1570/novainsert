// MeterEngine (plan Task 24, R-GUI-1 meter part): per channel peak (20 dB/s fall), peak hold (10 s, then the same fall),
// RMS (50 ms one-pole on x²), VU (300 ms one-pole on x², read relative to the device VU reference), both in AES17 dBFS: a
// full-scale sine reads 0 dBFS (plain RMS + 3.01 dB) — the convention of converter alignments ("+24 dBu at 0 dBFS") and of the
// 0 VU references (−20 / −18 dBFS), so dBFS + alignment = dBu at the converter (user 2026-11-05: levels must be right), clip (any sample
// > 0 dBFS) and near-full-scale (≥ −0.1 dBFS), each latched 2 s; rectified average (10 ms) for the VU needle. Time = samples fed.
// feed(l, nullptr, n) feeds channel 0 only (mono): channel 1 reads the floor. The optional gains scale what the levels
// (peak, hold, RMS, VU, rectified) show, so a meter can read after a trim; clip and near-full-scale always judge the raw
// sample, i.e. the converter (RETURN tap after the return trim, user 2026-11-05).
// Thread contract: prepare — message thread before feeding; feed — one RT thread, allocation/lock-free; read/resetHold —
// GUI thread, reads relaxed atomics published once per feed (a reading may mix two consecutive blocks: display only).
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ni::engine
{
class MeterEngine
{
public:
    struct Reading
    {
        float peakDb[2];
        float peakHoldDb[2];
        float rmsDb[2];
        float vu[2];            // 0 VU = vuRefDbfs
        bool clip[2];           // any sample > 0 dBFS, latched 2 s
        bool nearFullScale[2];  // ≥ −0.1 dBFS, latched 2 s
        float rect[2];          // full-wave rectified average (one-pole τ 10 ms on |x|, linear): the VU needle's drive
    };
    static constexpr float kFloorDb = -120.0f;

    void prepare (int fs) noexcept
    {
        fs_ = std::max (1, fs);
        fall_ = std::pow (10.0f, -1.0f / static_cast<float> (fs_));   // 20 dB/s in amplitude per sample
        rmsK_ = 1.0f - std::exp (-1.0f / (0.05f * static_cast<float> (fs_)));
        vuK_ = 1.0f - std::exp (-1.0f / (0.3f * static_cast<float> (fs_)));
        rectK_ = 1.0f - std::exp (-1.0f / (0.01f * static_cast<float> (fs_)));
        for (Chan& c : ch_) c = Chan {};
        now_ = 0;
        publish();
    }

    void feed (const float* l, const float* r, int n, float gainL = 1.0f, float gainR = 1.0f) noexcept
    {
        const float gain[2] = { gainL, gainR };
        if (resetHold_.exchange (false, std::memory_order_acq_rel))
            for (Chan& c : ch_) { c.hold = 0.0f; c.holdAt = now_; }
        const float* in[2] = { l, r };
        const int64_t holdLen = 10LL * fs_;
        for (int k = 0; k < 2; ++k)
        {
            Chan& c = ch_[k];
            if (in[k] == nullptr) continue;
            for (int i = 0; i < n; ++i)
            {
                const float raw = std::fabs (in[k][i]);
                const float x = in[k][i] * gain[k];
                const float a = std::fabs (x);
                const int64_t t = now_ + i;
                c.peak = std::max (a, c.peak * fall_);
                if (a >= c.hold) { c.hold = a; c.holdAt = t; }
                else if (t - c.holdAt > holdLen) c.hold *= fall_;
                c.ms += rmsK_ * (x * x - c.ms);
                c.vu += vuK_ * (x * x - c.vu);
                c.rect += rectK_ * (a - c.rect);
                if (raw > 1.0f) c.clipAt = t;
                if (raw >= kNearFull) c.nearAt = t;
            }
        }
        now_ += n;
        publish();
    }

    Reading read (float vuRefDbfs) const noexcept
    {
        Reading r {};
        const int64_t now = now_pub_.load (std::memory_order_acquire);
        const int64_t latch = 2LL * fs_;
        for (int k = 0; k < 2; ++k)
        {
            const Pub& p = pub_[k];
            r.peakDb[k] = db (p.peak.load (std::memory_order_relaxed));
            r.peakHoldDb[k] = db (p.hold.load (std::memory_order_relaxed));
            r.rmsDb[k] = powDb (p.ms.load (std::memory_order_relaxed));
            r.vu[k] = powDb (p.vu.load (std::memory_order_relaxed)) - vuRefDbfs;
            r.rect[k] = p.rect.load (std::memory_order_relaxed);
            const int64_t ca = p.clipAt.load (std::memory_order_relaxed), na = p.nearAt.load (std::memory_order_relaxed);
            r.clip[k] = ca >= 0 && now - ca <= latch;
            r.nearFullScale[k] = na >= 0 && now - na <= latch;
        }
        return r;
    }

    void resetHold() noexcept
    {
        resetHold_.store (true, std::memory_order_release);
        for (Pub& p : pub_) p.hold.store (0.0f, std::memory_order_relaxed);   // immediate on screen; feed clears its copy
    }

private:
    static constexpr float kNearFull = 0.98855309f;   // −0.1 dBFS
    struct Chan { float peak = 0, hold = 0, ms = 0, vu = 0, rect = 0; int64_t holdAt = 0, clipAt = -1, nearAt = -1; };
    struct Pub { std::atomic<float> peak { 0 }, hold { 0 }, ms { 0 }, vu { 0 }, rect { 0 }; std::atomic<int64_t> clipAt { -1 }, nearAt { -1 }; };

    static float db (float a) noexcept { return a > 1e-6f ? 20.0f * std::log10 (a) : kFloorDb; }
    static constexpr float kAes17Db = 3.0103f;   // 20·log10(√2): mean-square of a full-scale sine is 1/2
    static float powDb (float p) noexcept { return p > 1e-12f ? 10.0f * std::log10 (p) + kAes17Db : kFloorDb; }

    void publish() noexcept
    {
        for (int k = 0; k < 2; ++k)
        {
            pub_[k].peak.store (ch_[k].peak, std::memory_order_relaxed);
            pub_[k].hold.store (ch_[k].hold, std::memory_order_relaxed);
            pub_[k].ms.store (ch_[k].ms, std::memory_order_relaxed);
            pub_[k].vu.store (ch_[k].vu, std::memory_order_relaxed);
            pub_[k].rect.store (ch_[k].rect, std::memory_order_relaxed);
            pub_[k].clipAt.store (ch_[k].clipAt, std::memory_order_relaxed);
            pub_[k].nearAt.store (ch_[k].nearAt, std::memory_order_relaxed);
        }
        now_pub_.store (now_, std::memory_order_release);
    }

    int fs_ = 48000;
    float fall_ = 1.0f, rmsK_ = 1.0f, vuK_ = 1.0f, rectK_ = 1.0f;
    Chan ch_[2];
    int64_t now_ = 0;
    Pub pub_[2];
    std::atomic<int64_t> now_pub_ { 0 };
    std::atomic<bool> resetHold_ { false };
};
} // namespace ni::engine
