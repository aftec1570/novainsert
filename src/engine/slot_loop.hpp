// NovaInsert engine core – slot-locked hardware loop (F1 §3 "slot-locked loop engine", F0.4 §6/§7; §8 mute; §14 RT; §20
// routing). Header-only, dependency-free. Ported from the anchoring logic of the F0.4 spike (spikes/f0-render/src/F04Render.cpp,
// slot-locked branch of processBlock + hook PRE/POST).
//
// Model: every input sample at project position p owns the hardware output slot p + S0. S0 is fixed once per segment (a play
// start, a position discontinuity, a new caller segment generation or a latency change) at the hook's first POST after the
// segment start:  S0 = firstSlot − p0 − physLoop + R + len − burst  (burst = samples the host pushed before that POST,
// = R_total + len; for a chain the downstream latencies are absorbed by the burst). POST sends input slot − S0 (older frames
// dropped, missing → silence); PRE tags returns pos = sendSlot − S0; process() outputs for want = p − R the return with exactly
// that pos, or zero – and never a return tagged before the segment's start p0 (a hardware loop with memory, e.g. analog
// outboard, keeps ringing with the previous segment's sends: those slots are output as exact 0). The first physLoop samples
// of every segment (chain: physLoop₁ + Σ downstream R) cannot come back: the inherent gap (LIMITATION, F1 §3), counted in
// Status::inherentGap and never aligned away. An unprimed segment (REAPER loop
// wrap, no PDC burst) instead plays its first R outputs (want < p0) as silence over the previous pass's tail; inherentGap
// counts those R heard samples, whether the wrap falls on a device-block boundary or splits the block (see anchor()).
//
// Threads and contracts:
//   prepare()                         message thread, while neither process() nor the hook runs (sizes every buffer).
//   setRouting/setLatency/setFenced/setMuted/setProbe/cutProbe   any thread, RT-safe (atomics); take effect at the next block/cycle.
//   setTrims                          processBlock, every block, before process(): the trim parameters at block accuracy; a
//                                     changed gain ramps linearly across that block (no zipper step), exact from the next block.
//   process()                         the host's processBlock thread (one at a time). In-place (in == out) is supported.
//   onPre()/onPost()                  the device thread (audio hook), PRE → processBlock calls → POST per cycle.
//   captureReturn()/capturedCount()   the measurement worker; reads a capture only after the hook published it.
//   send ring  Spsc<Frame, 2·kLMax>   producer process(), consumer onPost(); overflow → counted (Status::sendOverflows).
//   return ring Spsc<RFrame, 2·kLMax> producer onPre(), consumer process(); overflow while nobody consumes (transport
//                                     stopped) is dropped silently – process() flushes the ring at every segment start.
//   segment snapshot                  process() → onPost() through a sequence lock (writer: odd while writing).
// The engine never gates the return on input level: Effect mode passes tails unchanged (F1 §20); Insert mode likewise.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include "engine/meters.hpp"
#include "engine/rings.hpp"
#include "engine/types.hpp"

#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace ni::engine
{
struct Routing
{
    // Effective widths (mono/stereo routing design 2026-11-05): a mono send is (L + R) / 2 on the one output (identical track
    // channels leave at their original level), a mono return is the one input on both track channels.
    bool sendStereo = true, retStereo = true;
    float sendTrimDb = 0.0f, returnTrimDb = 0.0f;
    // per-channel offsets on top of the ganged trims (VU GUI spec: per-channel CAL); effective gain = trim + offset_ch.
    // Mono uses [0] for both the send and the return.
    float sendOffsetDb[2] = { 0.0f, 0.0f }, returnOffsetDb[2] = { 0.0f, 0.0f };
};

// Flush-to-zero / denormals-are-zero for the scope (restores the caller's mode on exit).
class ScopedFtz
{
public:
#if defined(__aarch64__)
    ScopedFtz() noexcept { asm volatile("mrs %0, fpcr" : "=r"(old_)); asm volatile("msr fpcr, %0" : : "r"(old_ | (uint64_t { 1 } << 24))); }
    ~ScopedFtz() { asm volatile("msr fpcr, %0" : : "r"(old_)); }
private:
    uint64_t old_ = 0;
#elif defined(__SSE__) || defined(__x86_64__) || defined(_M_X64)   // MSVC x64 defines neither __SSE__ nor __x86_64__
    ScopedFtz() noexcept : old_(_mm_getcsr()) { _mm_setcsr(old_ | 0x8040u); }
    ~ScopedFtz() { _mm_setcsr(old_); }
private:
    unsigned old_;
#else
    ScopedFtz() noexcept = default;
#endif
};

class SlotLoopEngine
{
public:
    // 5 ms raised cosine (F1 §8): 240 samples at 48 kHz, 221 at 44.1 kHz, 960 at 192 kHz (setSampleRate).
    static constexpr int32_t fadeLenFor(int fs) noexcept { return std::max(1, (fs + 100) / 200); }
    static constexpr int32_t kCaptureCap = 4 * kLMax;        // return capture per probe: probe + post-silence + L_max
    using SendRing = Spsc<Frame, 2 * static_cast<std::size_t>(kLMax)>;
    using ReturnRing = Spsc<RFrame, 2 * static_cast<std::size_t>(kLMax)>;

    struct Status
    {
        int64_t inherentGap;          // current segment: samples with no possible return (slot before the first POST)
        uint64_t holdUnderruns;       // current segment: returns missing although their slot was inside the segment
        uint64_t lateDrops;           // cumulative: input frames that reached the hook after their slot had passed
        uint64_t nanFlushed;          // cumulative: NaN/Inf samples flushed to 0 (input + hardware return)
        bool continuityOk;            // current segment: hook positions contiguous (no missing return block, F1 §22 row 1)
        int32_t lagObserved;          // last block: p − tag of its first output sample (== R when locked), −1 when unmatched
        int32_t stableCycles;         // consecutive cycles in which every returned tag == p − R (F1 §9 Q9)
        uint64_t sendOverflows;       // cumulative: send frames rejected by a full ring
        uint64_t segments;            // cumulative: segments started (play start, seek, loop wrap, latency change)
    };

    SlotLoopEngine() noexcept = default;

    // Any thread: the device rate. The fade length follows at the next fade that starts from open or closed (a fade in
    // progress keeps its length).
    void setSampleRate(int fs) noexcept { fadeLenReq_.store(fadeLenFor(fs), std::memory_order_release); }
    int32_t fadeLength() const noexcept { return fadeLenReq_.load(std::memory_order_acquire); }

    void prepare(int hwBlock, int32_t lMax = kLMax) noexcept
    {
        B_ = hwBlock; lMax_ = std::min(lMax, kLMax);
        send_ = std::make_unique<SendRing>();
        ret_ = std::make_unique<ReturnRing>();
        capL_ = std::make_unique<float[]>(kCaptureCap);
        capR_ = std::make_unique<float[]>(kCaptureCap);
        snapGains();
        lastPlaying_ = false; expectPos_ = 0; curGen_ = 0; curP0_ = 0; curR_ = 0; fadeStage_ = Fade::Open; fadeIdx_ = 0;
        hookGen_ = 0; hookS0_ = 0; hookFs_ = 0; hookPhys_ = 0; hookFirstWant_ = 0; lastPreEnd_ = -1;
        hookProbeSeq_ = probeSeq_.load(std::memory_order_acquire); probe_ = nullptr; probeN_ = 0; probeIdx_ = 0; capActive_ = false;
    }

    void setRouting(Routing r) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            sendOffDb_[c].store(r.sendOffsetDb[c], std::memory_order_relaxed);
            retOffDb_[c].store(r.returnOffsetDb[c], std::memory_order_relaxed);
        }
        setTrims(r.sendTrimDb, r.returnTrimDb);
        route_.store((r.sendStereo ? 1u : 0u) | (r.retStereo ? 2u : 0u), std::memory_order_release);
    }

    // The ganged trims (dB); effective gain per channel = trim + offset. RT-safe: processBlock calls it every block.
    void setTrims(float sendDb, float returnDb) noexcept
    {
        for (int c = 0; c < 2; ++c)
        {
            sendGain_[c].store(dbToGain(sendDb + sendOffDb_[c].load(std::memory_order_relaxed)), std::memory_order_relaxed);
            retGain_[c].store(dbToGain(returnDb + retOffDb_[c].load(std::memory_order_relaxed)), std::memory_order_relaxed);
        }
    }

    // R >= physLoop + 2 blocks (F1 §3 margin) and R <= L_max (F1 §23 Q8). A change starts a new segment at the next block.
    bool setLatency(int32_t physLoop, int32_t reportedR) noexcept
    {
        if (B_ <= 0 || physLoop < 0 || reportedR < physLoop + kMarginBlocks * B_ || reportedR > lMax_) return false;
        physLoop_.store(physLoop, std::memory_order_relaxed);
        R_.store(reportedR, std::memory_order_relaxed);
        latencySeq_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Message thread only (same thread as setLatency): the device block changed; the rings are sized for kLMax and stay.
    void setHwBlock(int hwBlock) noexcept { B_ = hwBlock; }

    void setFenced(bool f) noexcept { fenced_.store(f, std::memory_order_release); }
    void setMuted(bool m) noexcept { muted_.store(m, std::memory_order_release); }

    // Arms (probe != nullptr, n > 0) or disarms the probe. While armed the hardware send carries probe[0..n) from the next
    // POST, then silence – never the program – and the return is captured from the probe's first slot (captureReturn).
    // loop: probe[0..n) repeats until disarmed (CAL reference tone); applySendGain: each channel is scaled by its effective
    // send gain (the CAL tone leaves the converter as the trimmed program would). The latency probe uses neither.
    void setProbe(const float* probe, int32_t n, bool loop = false, bool applySendGain = false) noexcept
    {
        probeFlags_.store((loop ? 1u : 0u) | (applySendGain ? 2u : 0u), std::memory_order_relaxed);
        probePtr_.store(n > 0 ? probe : nullptr, std::memory_order_relaxed);
        probeLen_.store(probe != nullptr ? std::max(n, 0) : 0, std::memory_order_relaxed);
        probeSeq_.fetch_add(1, std::memory_order_release);
    }

    // RT-safe from any thread (processBlock on a record start): the probe armed now sends silence from the next POST on, the
    // program does not come back until the message thread disarms (setProbe(nullptr)); a later setProbe is not affected.
    void cutProbe() noexcept { cutSeq_.store(probeSeq_.load(std::memory_order_acquire), std::memory_order_release); }

    // Hook-side meter taps (VU GUI spec §7): send = the hardware output after the send trim (onPost), return = the hardware
    // input before the return trim (onPre); skipped while fenced. Message thread, before the hook runs; null = none.
    void setMeters(MeterEngine* send, MeterEngine* ret) noexcept
    {
        sendMeter_.store(send, std::memory_order_release);
        retMeter_.store(ret, std::memory_order_release);
    }

    // ---- processBlock side --------------------------------------------------------------------------------------------
    // in[1]/out[1] may be null (single-channel host bus): in[0] is used for both inputs, out[1] is not written.
    void process(const float* const* in, float* const* out, int n, int64_t playPos, bool playing, int32_t segmentGen) noexcept
    {
        ScopedFtz ftz;
        if (n <= 0) return;
        float* o0 = out[0];
        float* o1 = out[1];
        if (! playing || ! send_)
        {
            snapGains();
            std::memset(o0, 0, sizeof(float) * static_cast<size_t>(n));
            if (o1 != nullptr) std::memset(o1, 0, sizeof(float) * static_cast<size_t>(n));
            lastPlaying_ = false;
            fadeStage_ = muted_.load(std::memory_order_acquire) ? Fade::Closed : Fade::Open; fadeIdx_ = 0;
            cycleSeen_.store(true, std::memory_order_relaxed); cycleBad_.store(true, std::memory_order_relaxed);
            lag_.store(-1, std::memory_order_relaxed);
            return;
        }
        const float* i0 = in[0];
        const float* i1 = in[1] != nullptr ? in[1] : in[0];
        const uint32_t ls = latencySeq_.load(std::memory_order_acquire);
        if (! lastPlaying_ || playPos != expectPos_ || segmentGen != lastSegArg_ || ls != lastLatSeq_)
            startSegment(playPos, segmentGen, ls);

        const uint32_t rt = route_.load(std::memory_order_acquire);
        const bool sendStereo = (rt & 1u) != 0, retStereo = (rt & 2u) != 0;
        // gains: from the applied value to the target across this block (a linear ramp), constant (bit-exact) when unchanged
        struct Ramp { float from, to, step; bool on; };
        Ramp sr[2], rr[2];
        auto ramp = [n] (float& applied, float target) noexcept {
            const bool on = target < applied || target > applied;
            Ramp r { applied, target, on ? (target - applied) / static_cast<float>(n) : 0.0f, on };
            applied = target;
            return r;
        };
        for (int c = 0; c < 2; ++c)
        {
            sr[c] = ramp(appliedSend_[c], sendGain_[c].load(std::memory_order_relaxed));
            rr[c] = ramp(appliedRet_[c], retGain_[c].load(std::memory_order_relaxed));
        }
        // the gain at sample k: constant when unchanged; else it reaches the target exactly on the block's last sample
        auto gainAt = [n] (const Ramp& r, int k) noexcept { return ! r.on ? r.from : k == n - 1 ? r.to : r.from + r.step * static_cast<float>(k + 1); };

        // 1) every input sample of the block into the send ring (all reads before any write: in == out is allowed)
        uint64_t nan = 0, ovf = 0;
        for (int k = 0; k < n; ++k)
        {
            float l = i0[k], r = i1[k];
            if (! std::isfinite(l)) { l = 0.0f; ++nan; }
            if (! std::isfinite(r)) { r = 0.0f; ++nan; }
            const float sgL = gainAt(sr[0], k), sgR = gainAt(sr[1], k);
            Frame f {};
            if (sendStereo) { f.l = l * sgL; f.r = r * sgR; }
            else { f.l = (l + r) * 0.5f * sgL; f.r = 0.0f; }
            f.gen = curGen_; f.pos = playPos + k;
            if (! send_->push(f)) ++ovf;
        }
        if (nan != 0) nanIn_.store(nanIn_.load(std::memory_order_relaxed) + nan, std::memory_order_relaxed);
        if (ovf != 0) sendOverflow_.store(sendOverflow_.load(std::memory_order_relaxed) + ovf, std::memory_order_relaxed);
        expectPos_ = playPos + n; lastPlaying_ = true;
        segPushed_.store(expectPos_ - curP0_, std::memory_order_release);

        // 2) output: the return tagged exactly want = p − R, or zero. A matched frame with want < p0 is consumed (it keeps
        //    the ring in order and the lock accounting: its tag is right) but its value is discarded: no input of this
        //    segment exists for it, and an analog loop's return there still rings with the previous segment's last sends
        //    (Apollo/SSL finding: heard for R − physLoop samples after every unprimed wrap). Exact 0 in [p0, p0 + R).
        const bool fwValid = firstWantGen_.load(std::memory_order_acquire) == curGen_;
        const int64_t fw = firstWant_.load(std::memory_order_relaxed);
        updateFade();
        bool bad = false;
        for (int k = 0; k < n; ++k)
        {
            const int64_t want = playPos + k - curR_;
            float a = 0.0f, b = 0.0f; bool hit = false;
            const RFrame* f;
            while ((f = ret_->peek()) != nullptr && (f->gen < curGen_ || (f->gen == curGen_ && f->pos < want))) ret_->drop();
            if (f != nullptr && f->gen == curGen_ && f->pos == want)
            {
                if (want >= curP0_) { a = f->l; b = f->r; }
                hit = true; ret_->drop();
            }
            else if (fwValid && want >= fw) ++underruns_;
            else if (want >= curP0_) ++gap_;
            bad |= ! hit;
            if (k == 0) lag_.store(hit ? static_cast<int32_t>(playPos - want) : -1, std::memory_order_relaxed);
            const float rgL = gainAt(rr[0], k), rgR = gainAt(rr[1], k);
            float L, Rr;
            if (retStereo) { L = a * rgL; Rr = b * rgR; }
            else { L = Rr = a * rgL; }
            applyFade(L, Rr);
            o0[k] = L;
            if (o1 != nullptr) o1[k] = Rr;
        }
        gapOut_.store(gap_ + (fwValid ? heardPre_.load(std::memory_order_relaxed) : 0), std::memory_order_relaxed);
        underrunsOut_.store(underruns_, std::memory_order_relaxed);
        cycleSeen_.store(true, std::memory_order_relaxed);
        if (bad) cycleBad_.store(true, std::memory_order_relaxed);
    }

    // ---- hook side (device thread) ------------------------------------------------------------------------------------
    void onPre(int64_t cyclePos, int len, const float* const* hwIn) noexcept
    {
        if (! send_ || len <= 0) return;
        preBase_.store(cyclePos, std::memory_order_release);
        if (lastPreEnd_ >= 0 && cyclePos != lastPreEnd_)
        {
            discont_.fetch_add(1, std::memory_order_relaxed);
            cycleBad_.store(true, std::memory_order_relaxed);
        }
        lastPreEnd_ = cyclePos + len;
        if (fenced_.load(std::memory_order_acquire)) return;
        const bool stereo = (route_.load(std::memory_order_acquire) & 2u) != 0;   // return width
        if (MeterEngine* m = retMeter_.load(std::memory_order_acquire))   // after the return trim (clip is judged on the raw sample)
            m->feed(hwIn[0], stereo ? hwIn[1] : nullptr, len, retGain_[0].load(std::memory_order_relaxed), retGain_[1].load(std::memory_order_relaxed));
        uint64_t nan = 0;
        for (int k = 0; k < len; ++k)
        {
            float a = hwIn[0][k], b = stereo ? hwIn[1][k] : 0.0f;
            if (! std::isfinite(a)) { a = 0.0f; ++nan; }
            if (! std::isfinite(b)) { b = 0.0f; ++nan; }
            const int64_t sendSlot = cyclePos + k - hookPhys_;
            const RFrame f { a, b, sendSlot - hookS0_, sendSlot >= hookFs_ ? hookGen_ : hookGen_ - 1 };
            (void) ret_->push(f);   // full only while nobody consumes (stopped): flushed at the next segment start
            if (capActive_)
            {
                const int64_t i = cyclePos + k - capStart_;
                if (i >= 0 && i < kCaptureCap) { capL_[static_cast<size_t>(i)] = a; capR_[static_cast<size_t>(i)] = b; }
            }
        }
        if (nan != 0) nanRet_.store(nanRet_.load(std::memory_order_relaxed) + nan, std::memory_order_relaxed);
        if (capActive_)
            capEnd_.store(static_cast<int32_t>(std::min<int64_t>(kCaptureCap, cyclePos + len - capStart_)), std::memory_order_release);
    }

    // Returns true when hwOut[0] (and hwOut[1] for a stereo send) was written for this cycle; false = left untouched (fenced/unprepared).
    bool onPost(int64_t cyclePos, int len, float* const* hwOut) noexcept
    {
        if (! send_ || len <= 0) return false;
        const bool bad = cycleBad_.exchange(false, std::memory_order_relaxed);
        const bool seen = cycleSeen_.exchange(false, std::memory_order_relaxed);
        if (bad) stable_.store(0, std::memory_order_relaxed);
        else if (seen) stable_.store(stable_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        anchor(cyclePos, len);
        if (fenced_.load(std::memory_order_acquire))
        {
            Frame f;
            while (send_->pop(f)) {}   // fence: no hook I/O, the send ring is drained so nothing stale leaves later
            return false;
        }
        latchProbe(cyclePos, len);
        if (probe_ != nullptr && cutSeq_.load(std::memory_order_acquire) == hookProbeSeq_) probeIdx_ = probeN_;   // cut: silence
        const bool stereo = (route_.load(std::memory_order_acquire) & 1u) != 0;   // send width
        const float pgL = probeGain_ ? sendGain_[0].load(std::memory_order_relaxed) : 1.0f;
        const float pgR = probeGain_ ? sendGain_[1].load(std::memory_order_relaxed) : 1.0f;
        uint64_t late = 0;
        for (int k = 0; k < len; ++k)
        {
            const int64_t want = cyclePos + k - hookS0_;   // this slot carries input sample `want`
            float a = 0.0f, b = 0.0f;
            const Frame* q;
            while ((q = send_->peek()) != nullptr && (q->gen < hookGen_ || (q->gen == hookGen_ && q->pos < want)))
            {
                if (q->gen == hookGen_ && q->pos >= hookFirstWant_) ++late;
                send_->drop();
            }
            if (q != nullptr && q->gen == hookGen_ && q->pos == want) { a = q->l; b = q->r; send_->drop(); }
            if (probe_ != nullptr)
            {
                const float p = probeIdx_ < probeN_ ? probe_[probeIdx_] : 0.0f;
                if (probeIdx_ < probeN_ && ++probeIdx_ == probeN_ && probeLoop_) probeIdx_ = 0;
                a = p * pgL; b = p * pgR;
            }
            hwOut[0][k] = a;
            if (stereo) hwOut[1][k] = b;
        }
        if (late != 0) lateDrops_.store(lateDrops_.load(std::memory_order_relaxed) + late, std::memory_order_relaxed);
        if (MeterEngine* m = sendMeter_.load(std::memory_order_acquire)) m->feed(hwOut[0], stereo ? hwOut[1] : nullptr, len);   // what leaves the converter
        return true;
    }

    Status status() const noexcept
    {
        return { gapOut_.load(std::memory_order_relaxed), underrunsOut_.load(std::memory_order_relaxed),
                 lateDrops_.load(std::memory_order_relaxed),
                 nanIn_.load(std::memory_order_relaxed) + nanRet_.load(std::memory_order_relaxed),
                 discont_.load(std::memory_order_relaxed) == discontBase_.load(std::memory_order_relaxed),
                 lag_.load(std::memory_order_relaxed), stable_.load(std::memory_order_relaxed),
                 sendOverflow_.load(std::memory_order_relaxed), segments_.load(std::memory_order_relaxed) };
    }

    // Samples of the current probe's return captured so far (index 0 = the probe's first slot); 0 when no probe is armed or
    // the hook has not latched the latest setProbe yet. Measurement worker / message thread.
    int32_t capturedCount() const noexcept
    {
        const uint32_t seq = probeSeq_.load(std::memory_order_acquire);
        if (capSeq_.load(std::memory_order_acquire) != seq || probePtr_.load(std::memory_order_relaxed) == nullptr) return 0;
        return capEnd_.load(std::memory_order_acquire);
    }

    // Copies the first n captured return samples of the current probe (index 0 = the probe's first slot, so the probe's
    // position in dst is physLoop). False while fewer than n are captured, when no probe is armed, or n is out of range.
    bool captureReturn(float* dstL, float* dstR, int32_t n) noexcept
    {
        if (! capL_ || n <= 0 || n > kCaptureCap) return false;
        const uint32_t seq = probeSeq_.load(std::memory_order_acquire);
        if (capSeq_.load(std::memory_order_acquire) != seq || capEnd_.load(std::memory_order_acquire) < n) return false;
        std::memcpy(dstL, capL_.get(), sizeof(float) * static_cast<size_t>(n));
        if (dstR != nullptr) std::memcpy(dstR, capR_.get(), sizeof(float) * static_cast<size_t>(n));
        return capSeq_.load(std::memory_order_acquire) == seq;
    }

private:
    enum class Fade : uint8_t { Open, Closing, Closed, Opening };

    // |db| below 0.001 dB is unity: a 0 dB host parameter round-trips through its normalised value to ~1e-6 dB,
    // and that 1-ulp gain would break bit-exactness (f3_3 finding, F1 §3 exact-loop requirement).
    static constexpr float kUnityDbEps = 0.001f;
    static float dbToGain(float db) noexcept { return std::fabs(db) < kUnityDbEps ? 1.0f : std::pow(10.0f, db / 20.0f); }
    void snapGains() noexcept   // no audio in flight (stopped, new segment): the next block starts at the target, no ramp
    {
        for (int c = 0; c < 2; ++c)
        {
            appliedSend_[c] = sendGain_[c].load(std::memory_order_relaxed);
            appliedRet_[c] = retGain_[c].load(std::memory_order_relaxed);
        }
    }

    void startSegment(int64_t pos, int32_t segmentGen, uint32_t latSeq) noexcept
    {
        ++curGen_; curP0_ = pos; curR_ = R_.load(std::memory_order_relaxed);
        segments_.store(segments_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        lastSegArg_ = segmentGen; lastLatSeq_ = latSeq;
        gap_ = 0; underruns_ = 0;
        const int64_t fs = preBase_.load(std::memory_order_acquire);
        segSeq_.fetch_add(1, std::memory_order_acq_rel);                // odd: writing
        segGen_.store(curGen_, std::memory_order_relaxed);
        segP0_.store(pos, std::memory_order_relaxed);
        segFs_.store(fs, std::memory_order_relaxed);
        segR_.store(curR_, std::memory_order_relaxed);
        segPushed_.store(0, std::memory_order_relaxed);
        segSeq_.fetch_add(1, std::memory_order_release);                // even: published
        RFrame f;
        while (ret_->pop(f)) {}
    }

    // First POST of a new segment: fix S0 (F0.4 §7). The device cycle length `len` stands in for the spike's first block
    // size n (equal whenever processBlock n == B, the only case REAPER showed); burst = samples pushed before this POST.
    void anchor(int64_t cyclePos, int len) noexcept
    {
        const uint32_t s1 = segSeq_.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0) return;
        const int32_t g = segGen_.load(std::memory_order_relaxed);
        if (g == hookGen_) return;
        const int64_t p0 = segP0_.load(std::memory_order_relaxed), fs = segFs_.load(std::memory_order_relaxed);
        const int32_t R = segR_.load(std::memory_order_relaxed);
        const int64_t burst = segPushed_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (segSeq_.load(std::memory_order_relaxed) != s1) return;     // torn: anchor at the next POST
        hookGen_ = g; hookFs_ = fs; hookPhys_ = physLoop_.load(std::memory_order_relaxed);
        hookS0_ = fs - p0 - hookPhys_ + R + len - burst;
        hookFirstWant_ = cyclePos - hookS0_;
        // Outputs for want < p0 (the first R of the segment) never have a return. A primed host (burst = R + len, play/seek)
        // discards them; an unprimed one (REAPER loop wrap: burst = len, or len − s when the wrap splits the device block
        // into s old + len − s new samples) plays them in place of the previous pass's last R samples. Only the heard part
        // belongs to the inherent gap (f3_3 heard capture, LIMITATION F1 §3), and there are never more than R of them: a
        // short burst (split wrap) does not add its shortfall to the heard silence, the loop start stays exact from P.
        heardPre_.store(std::clamp<int64_t>(static_cast<int64_t>(R) - (burst - len), 0, R), std::memory_order_relaxed);
        firstWant_.store(hookFirstWant_, std::memory_order_relaxed);
        firstWantGen_.store(g, std::memory_order_release);
        discontBase_.store(discont_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void latchProbe(int64_t cyclePos, int len) noexcept
    {
        const uint32_t ps = probeSeq_.load(std::memory_order_acquire);
        if (ps == hookProbeSeq_) return;
        hookProbeSeq_ = ps;
        probe_ = probePtr_.load(std::memory_order_relaxed);
        probeN_ = probeLen_.load(std::memory_order_relaxed);
        probeIdx_ = 0;
        const uint32_t pf = probeFlags_.load(std::memory_order_relaxed);
        probeLoop_ = (pf & 1u) != 0; probeGain_ = (pf & 2u) != 0;
        capActive_ = probe_ != nullptr;
        capEnd_.store(0, std::memory_order_relaxed);
        if (capActive_)
        {
            capStart_ = cyclePos;   // this cycle's PRE already ran: its slots cannot hold the probe's return → zeros
            const auto z = static_cast<size_t>(std::min(len, kCaptureCap));
            std::memset(capL_.get(), 0, sizeof(float) * z);
            std::memset(capR_.get(), 0, sizeof(float) * z);
            capEnd_.store(static_cast<int32_t>(z), std::memory_order_relaxed);
        }
        capSeq_.store(ps, std::memory_order_release);
    }

    void updateFade() noexcept
    {
        const bool m = muted_.load(std::memory_order_acquire);
        if (m && (fadeStage_ == Fade::Open || fadeStage_ == Fade::Opening))
        {
            if (fadeStage_ == Fade::Open) { fadeLen_ = fadeLenReq_.load(std::memory_order_acquire); fadeIdx_ = 0; }
            else fadeIdx_ = fadeLen_ - fadeIdx_;
            fadeStage_ = Fade::Closing;
        }
        else if (! m && (fadeStage_ == Fade::Closed || fadeStage_ == Fade::Closing))
        {
            if (fadeStage_ == Fade::Closed) { fadeLen_ = fadeLenReq_.load(std::memory_order_acquire); fadeIdx_ = 0; }
            else fadeIdx_ = fadeLen_ - fadeIdx_;
            fadeStage_ = Fade::Opening;
        }
    }

    // Raised cosine: 0.5 (1 ∓ cos(π i / len)); computed per sample (fades only, ≤ 960 samples at 192 kHz).
    static float fadeGain(int32_t i, int32_t len, bool in) noexcept
    {
        const double c = std::cos(3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(len));
        return static_cast<float>(in ? 0.5 * (1.0 - c) : 0.5 * (1.0 + c));
    }

    void applyFade(float& l, float& r) noexcept
    {
        switch (fadeStage_)
        {
            case Fade::Open: return;
            case Fade::Closed: l = 0.0f; r = 0.0f; return;
            case Fade::Closing:
            {
                const float g = fadeGain(fadeIdx_, fadeLen_, false);
                l *= g; r *= g;
                if (++fadeIdx_ >= fadeLen_) fadeStage_ = Fade::Closed;
                return;
            }
            case Fade::Opening:
            {
                const float g = fadeGain(fadeIdx_, fadeLen_, true);
                l *= g; r *= g;
                if (++fadeIdx_ >= fadeLen_) fadeStage_ = Fade::Open;
                return;
            }
        }
    }

    // configuration (any thread)
    int B_ = 0;
    int32_t lMax_ = kLMax;
    std::atomic<int32_t> physLoop_ { 0 }, R_ { 0 };
    std::atomic<uint32_t> latencySeq_ { 0 }, route_ { 3u };
    std::atomic<float> sendGain_[2] { 1.0f, 1.0f }, retGain_[2] { 1.0f, 1.0f };   // targets (trim + offset, linear)
    std::atomic<float> sendOffDb_[2] { 0.0f, 0.0f }, retOffDb_[2] { 0.0f, 0.0f };
    std::atomic<bool> fenced_ { false }, muted_ { false };
    std::atomic<const float*> probePtr_ { nullptr };
    std::atomic<uint32_t> probeFlags_ { 0 };
    std::atomic<MeterEngine*> sendMeter_ { nullptr }, retMeter_ { nullptr };
    std::atomic<int32_t> probeLen_ { 0 };
    std::atomic<uint32_t> probeSeq_ { 0 }, cutSeq_ { ~0u };

    // buffers (sized at prepare)
    std::unique_ptr<SendRing> send_;
    std::unique_ptr<ReturnRing> ret_;
    std::unique_ptr<float[]> capL_, capR_;
    std::atomic<int32_t> fadeLenReq_ { fadeLenFor(kDefaultSampleRate) };
    int32_t fadeLen_ = fadeLenFor(kDefaultSampleRate);   // RT: length of the fade in progress

    // process() thread
    bool lastPlaying_ = false;
    float appliedSend_[2] { 1.0f, 1.0f }, appliedRet_[2] { 1.0f, 1.0f };   // processBlock-side: gain applied at the end of the last block
    int64_t expectPos_ = 0, curP0_ = 0;
    int32_t curGen_ = 0, curR_ = 0, lastSegArg_ = 0;
    uint32_t lastLatSeq_ = 0;
    int64_t gap_ = 0;
    uint64_t underruns_ = 0;
    Fade fadeStage_ = Fade::Open;
    int32_t fadeIdx_ = 0;

    // segment snapshot process() → onPost()
    std::atomic<uint32_t> segSeq_ { 0 };
    std::atomic<int32_t> segGen_ { 0 }, segR_ { 0 };
    std::atomic<int64_t> segP0_ { 0 }, segFs_ { 0 }, segPushed_ { 0 };
    std::atomic<int64_t> preBase_ { 0 };

    // hook thread
    int32_t hookGen_ = 0, hookPhys_ = 0;
    int64_t hookS0_ = 0, hookFs_ = 0, hookFirstWant_ = 0, lastPreEnd_ = -1;
    uint32_t hookProbeSeq_ = 0;
    const float* probe_ = nullptr;
    int32_t probeN_ = 0, probeIdx_ = 0;
    bool probeLoop_ = false, probeGain_ = false;
    bool capActive_ = false;
    int64_t capStart_ = 0;
    std::atomic<int64_t> firstWant_ { 0 };
    std::atomic<int32_t> firstWantGen_ { -1 };
    std::atomic<int32_t> capEnd_ { 0 };
    std::atomic<uint32_t> capSeq_ { ~0u };

    // status
    std::atomic<int64_t> gapOut_ { 0 }, heardPre_ { 0 };
    std::atomic<uint64_t> underrunsOut_ { 0 }, lateDrops_ { 0 }, nanIn_ { 0 }, nanRet_ { 0 }, sendOverflow_ { 0 }, segments_ { 0 };
    std::atomic<uint32_t> discont_ { 0 }, discontBase_ { 0 };
    std::atomic<int32_t> lag_ { -1 }, stable_ { 0 };
    std::atomic<bool> cycleSeen_ { false }, cycleBad_ { false };
};
} // namespace ni::engine
