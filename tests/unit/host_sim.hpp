// Test utility: simulated REAPER device + PDC host for SlotLoopEngine tests (F0.4 §6 lag model, 48 kHz). Never part of the
// product. Model, per device cycle c (hook slots base = c·B .. c·B + B − 1):
//   PRE(c) for every instance (hwIn = what that instance's hwOut carried physLoop slots earlier: an ideal cable, or with
//     Inst::ringA > 0 an analog loop with memory: ret[k] = send[k] + ringA · ret[k − 1], the outboard gear's decay)
//   → host processBlock calls, in place on one buffer (JUCE semantics), each chunk through the whole chain in order:
//       first cycle of a segment: a PDC priming burst of R_total + B input samples (labels P .. P + R_total + B − 1),
//       afterwards B samples per cycle; chunks are min(n, remaining) samples (n = host block size, may differ from B);
//       the host's compensated output for project position q is the chain output at label q + R_total (q < P is dropped);
//       while stopped the host calls processBlock with playing = false at position 0 (pre-roll / idle blocks)
//   → POST(c) for every instance (hwOut pre-filled with the instance's pattern, then recorded as the "wire" by slot).
// runCycles() never allocates (every buffer is sized in the constructor) so it can run under AllocGuard.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>
#include "engine/slot_loop.hpp"

namespace ni::test
{
// Deterministic, non-zero, channel-distinct content; every value is exactly representable (integer / 2^19).
inline float content(int64_t q, int ch) noexcept
{
    return static_cast<float>((q % 100003) + 1 + ch * 200000) * (1.0f / 524288.0f);
}

class HostSim
{
public:
    struct Inst
    {
        engine::SlotLoopEngine* eng = nullptr;
        int ch = 2;                          // physical channels of this instance's hardware pair (1 = mono)
        int32_t physLoop = 0, R = 0;
        float outPrefill = 0.0f;             // value the host leaves in hwOut before POST (fence test)
        float ringA = 0.0f;                  // return memory (one-pole ring of earlier sends); 0 = ideal digital loop
        float ringState[2] { 0.0f, 0.0f };
        std::vector<float> wire[2];          // hwOut recorded by slot
        std::vector<float> preBuf[2], postBuf[2];
    };

    HostSim(int B, int n, int64_t maxSlots, int64_t maxPos) : B_(B), n_(n), maxSlots_(maxSlots), maxPos_(maxPos)
    {
        outL.assign(static_cast<size_t>(maxPos), 0.0f);
        outR.assign(static_cast<size_t>(maxPos), 0.0f);
        written.assign(static_cast<size_t>(maxPos), 0);
        for (auto& b : buf_) b.assign(static_cast<size_t>(B + 4 * engine::kLMax), 0.0f);   // largest burst: R_total + B
    }

    // Instances are processed in the order they were added (upstream first). The sim owns its copy of R for its PDC model.
    int add(engine::SlotLoopEngine& e, int channels, int32_t physLoop, int32_t R, float outPrefill = 0.0f)
    {
        auto in = std::make_unique<Inst>();
        in->eng = &e; in->ch = channels; in->physLoop = physLoop; in->R = R; in->outPrefill = outPrefill;
        for (int c = 0; c < 2; ++c)
        {
            in->wire[c].assign(static_cast<size_t>(maxSlots_), 0.0f);
            in->preBuf[c].assign(static_cast<size_t>(B_), 0.0f);
            in->postBuf[c].assign(static_cast<size_t>(B_), 0.0f);
        }
        inst_.push_back(std::move(in));
        return static_cast<int>(inst_.size()) - 1;
    }

    void play(int64_t P) noexcept { playing_ = true; P_ = P; nextLabel_ = P; segStart_ = true; ++segGen_; playCycle_ = cycle_; }
    void stop() noexcept { playing_ = false; }
    // REAPER loop wrap (f3_3 heard capture): the position jumps but the host does NOT re-prime PDC (no burst, same caller
    // generation). The host's compensated stream stays continuous, so outputs for labels P .. P + R_total − 1 are heard in
    // place of the previous pass's last R_total samples; the sim records them in `wrapTail` (index = label − P).
    void jumpUnprimed(int64_t P) noexcept { P_ = P; nextLabel_ = P; wrapTailFrom_ = P; }
    // REAPER loop wrap inside a device cycle (loop length not a multiple of B): the NEXT cycle's host calls are s samples
    // continuing the current pass (ending at the loop end) and then B − s samples from P, both before that cycle's POST.
    // No PDC burst, same caller generation; the engine sees the discontinuity only through playPos. 0 <= s < B.
    void jumpUnprimedAt(int s, int64_t P) noexcept { splitS_ = s; splitP_ = P; }

    void runCycles(int count) noexcept
    {
        for (int c = 0; c < count; ++c) step();
    }

    int32_t rTotal() const noexcept { int32_t r = 0; for (auto& i : inst_) r += i->R; return r; }
    int64_t nextLabel() const noexcept { return nextLabel_; }
    int64_t cycle() const noexcept { return cycle_; }
    int64_t playSlot() const noexcept { return playCycle_ * B_; }   // fs: slot base of the cycle the segment started in
    float wire(int inst, int ch, int64_t slot) const { return inst_[static_cast<size_t>(inst)]->wire[ch][static_cast<size_t>(slot)]; }
    void setRing(int inst, float a) noexcept { inst_[static_cast<size_t>(inst)]->ringA = a; }

    // options
    bool callWhenStopped = true;
    int64_t skipPreAtCycle = -1;                                   // device overload: that cycle's PRE never reaches the hook
    void (*inputOverride)(int64_t q, float& l, float& r) = nullptr; // fault injection on the chain input
    // results
    std::vector<float> outL, outR;                                 // host-compensated output by project position
    std::vector<uint8_t> written;
    static constexpr int64_t kWrapTailMax = 4 * engine::kLMax;
    std::vector<float> wrapTailL = std::vector<float>(static_cast<size_t>(kWrapTailMax), 0.0f), wrapTailR = wrapTailL;
    uint64_t stoppedNonZero = 0;                                   // non-zero samples produced by playing == false blocks

private:
    void step() noexcept
    {
        const int64_t base = cycle_ * B_;
        for (auto& ip : inst_)
        {
            Inst& in = *ip;
            for (int ch = 0; ch < in.ch; ++ch)
                for (int k = 0; k < B_; ++k)
                {
                    const int64_t src = base + k - in.physLoop;
                    float v = (src >= 0 && src < maxSlots_) ? in.wire[ch][static_cast<size_t>(src)] : 0.0f;
                    if (in.ringA != 0.0f) { v += in.ringA * in.ringState[ch]; in.ringState[ch] = v; }
                    in.preBuf[ch][static_cast<size_t>(k)] = v;
                }
            const float* hwIn[2] { in.preBuf[0].data(), in.preBuf[1].data() };
            if (cycle_ != skipPreAtCycle) in.eng->onPre(base, B_, hwIn);
        }
        float* io[2] { buf_[0].data(), buf_[1].data() };
        if (playing_)
        {
            const int32_t rt = rTotal();
            int64_t total = segStart_ ? rt + B_ : B_;
            int64_t untilSplit = splitS_ >= 0 ? splitS_ : -1;   // pending split wrap: the host jumps after these samples
            while (total > 0)
            {
                if (untilSplit == 0) { jumpUnprimed(splitP_); untilSplit = -1; splitS_ = -1; }
                int m = static_cast<int>(std::min<int64_t>(n_, total));
                if (untilSplit > 0) m = static_cast<int>(std::min<int64_t>(m, untilSplit));
                for (int k = 0; k < m; ++k)
                {
                    float l = content(nextLabel_ + k, 0), r = content(nextLabel_ + k, 1);
                    if (inputOverride != nullptr) inputOverride(nextLabel_ + k, l, r);
                    io[0][k] = l; io[1][k] = r;
                }
                for (auto& ip : inst_) ip->eng->process(io, io, m, nextLabel_, true, segGen_);
                for (int k = 0; k < m; ++k)
                {
                    const int64_t q = nextLabel_ + k - rt;
                    if (q >= P_ && q < maxPos_) { outL[static_cast<size_t>(q)] = io[0][k]; outR[static_cast<size_t>(q)] = io[1][k]; written[static_cast<size_t>(q)] = 1; }
                    const int64_t t = nextLabel_ + k - wrapTailFrom_;
                    if (wrapTailFrom_ >= 0 && t >= 0 && t < rt && t < kWrapTailMax) { wrapTailL[static_cast<size_t>(t)] = io[0][k]; wrapTailR[static_cast<size_t>(t)] = io[1][k]; }
                }
                nextLabel_ += m; total -= m;
                if (untilSplit > 0) untilSplit -= m;
            }
            segStart_ = false;
        }
        else if (callWhenStopped)
        {
            for (int k = 0; k < B_; ++k) { io[0][k] = 0.25f; io[1][k] = -0.25f; }   // non-zero input: the engine must not pass it
            for (auto& ip : inst_) ip->eng->process(io, io, B_, 0, false, segGen_);
            for (int k = 0; k < B_; ++k) stoppedNonZero += (io[0][k] != 0.0f) + (io[1][k] != 0.0f);
        }
        for (auto& ip : inst_)
        {
            Inst& in = *ip;
            for (int ch = 0; ch < 2; ++ch) std::fill(in.postBuf[ch].begin(), in.postBuf[ch].end(), in.outPrefill);
            float* hwOut[2] { in.postBuf[0].data(), in.postBuf[1].data() };
            in.eng->onPost(base, B_, hwOut);
            for (int ch = 0; ch < in.ch; ++ch)
                for (int k = 0; k < B_; ++k)
                    if (base + k < maxSlots_) in.wire[ch][static_cast<size_t>(base + k)] = in.postBuf[ch][static_cast<size_t>(k)];
        }
        ++cycle_;
    }

    int B_, n_;
    int64_t maxSlots_, maxPos_;
    std::vector<std::unique_ptr<Inst>> inst_;
    bool playing_ = false, segStart_ = false;
    int64_t P_ = 0, nextLabel_ = 0, cycle_ = 0, playCycle_ = 0, wrapTailFrom_ = -1, splitP_ = 0;
    int splitS_ = -1;
    int32_t segGen_ = 0;
    std::vector<float> buf_[2];
};
} // namespace ni::test
