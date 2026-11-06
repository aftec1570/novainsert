#include "plugin/instance.hpp"
#include <chrono>
#include <cstring>

namespace ni::plugin
{

uint64_t Instance::pack(const Map& m) noexcept
{
    // one byte per channel, stored as channel + 1 (0 = none); channels < HookDispatcher::kMaxChannels (128)
    auto b = [](int16_t c) { return static_cast<uint64_t>(c < 0 ? 0 : c + 1) & 0xffu; };
    return b(m.out[0]) | (b(m.out[1]) << 8) | (b(m.in[0]) << 16) | (b(m.in[1]) << 24) | (m.sendStereo ? (1ull << 32) : 0ull) | (m.retStereo ? (1ull << 33) : 0ull);
}

Instance::Map Instance::unpack(uint64_t v) noexcept
{
    auto c = [v](int shift) { return static_cast<int16_t>(static_cast<int>((v >> shift) & 0xffu) - 1); };
    return Map { { c(0), c(8) }, { c(16), c(24) }, ((v >> 32) & 1u) != 0, ((v >> 33) & 1u) != 0 };
}

Instance::Instance(host::HookDispatcher& hooks) : hooks_(hooks)
{
    engine_.prepare(engine::kBMax, engine::kLMax);
    inScratch_ = std::make_unique<float[]>(2 * static_cast<size_t>(engine::kBMax));
    outScratch_ = std::make_unique<float[]>(2 * static_cast<size_t>(engine::kBMax));
}

Instance::~Instance() { release(); }

bool Instance::configure(const host::Pair& pair, const engine::Routing& routing, int hwBlock, int loopExtra)
{
    // Every rejection releases: the previous pair's channel map must never keep writing hardware outputs (S3 review I1).
    bool ok = hwBlock > 0 && hwBlock <= engine::kBMax && loopExtra >= 0;
    for (int c = 0; ok && c < (pair.sendStereo ? 2 : 1); ++c) if (pair.out[c] < 0 || pair.out[c] >= host::HookDispatcher::kMaxChannels) ok = false;
    for (int c = 0; ok && c < (pair.retStereo ? 2 : 1); ++c) if (pair.in[c] < 0 || pair.in[c] >= host::HookDispatcher::kMaxChannels) ok = false;
    if (! ok) { release(); return false; }
    // R = physLoop + 2B rounded UP to whole blocks: REAPER feeds the PDC priming burst in whole blocks (F0.3), so a
    // non-multiple R would turn the ceil(R/B)·B − R remainder into extra gap at play start (Apollo bench: 512 vs 473).
    const int32_t phys = kFixedPhysBlocks * hwBlock + loopExtra, R = reportedFor(phys, hwBlock);
    engine_.setHwBlock(hwBlock);
    hwBlock_ = hwBlock;
    if (! engine_.setLatency(phys, R)) { release(); return false; }
    engine::Routing r = routing;
    r.sendStereo = pair.sendStereo; r.retStereo = pair.retStereo;   // the physical pair decides the widths (F1 §20: mono hardware path inside a stereo host bus)
    engine_.setRouting(r);
    Map m {};
    m.out[0] = static_cast<int16_t>(pair.out[0]); m.in[0] = static_cast<int16_t>(pair.in[0]);
    m.out[1] = pair.sendStereo ? static_cast<int16_t>(pair.out[1]) : int16_t { -1 };
    m.in[1] = pair.retStereo ? static_cast<int16_t>(pair.in[1]) : int16_t { -1 };
    m.sendStereo = pair.sendStereo; m.retStereo = pair.retStereo;
    map_.store(pack(m), std::memory_order_release);
    physLoop_.store(phys, std::memory_order_relaxed);
    reported_.store(R, std::memory_order_relaxed);
    configGen_.fetch_add(1, std::memory_order_release);
    if (slot_ < 0)
    {
        preSeen_.store(false, std::memory_order_relaxed);   // published by the active_ release below
        slot_ = hooks_.registerClient(this);
    }
    active_.store(slot_ >= 0, std::memory_order_release);
    return slot_ >= 0;
}

bool Instance::applyMeasured(int32_t phys)
{
    if (! active_.load(std::memory_order_acquire) || hwBlock_ <= 0 || phys < 0 || phys > engine::kLMax - 2 * hwBlock_) return false;
    const int32_t R = reportedFor(phys, hwBlock_);
    if (! engine_.setLatency(phys, R)) return false;
    physLoop_.store(phys, std::memory_order_relaxed);
    reported_.store(R, std::memory_order_relaxed);
    configGen_.fetch_add(1, std::memory_order_release);
    return true;
}

void Instance::release()
{
    active_.store(false, std::memory_order_release);
    if (slot_ >= 0) { hooks_.unregisterClient(slot_); slot_ = -1; }   // returns after any in-flight hook cycle
}

int64_t Instance::steadyMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Instance::process(float* const* io, int nch, int n, int64_t playPos, bool playing) noexcept
{
    lastProcessedMs_.store(steadyMs(), std::memory_order_relaxed);   // RT: clock read + relaxed store (F1 §10 involved)
    if (! active_.load(std::memory_order_acquire)) return false;
    float* p[2] { io[0], nch > 1 ? io[1] : nullptr };
    // Activated after this cycle's PRE fan-out (S3 review I-1): the engine's preBase_ is stale (or 0), and a segment started
    // now would anchor on it and stay silent until the next stop/seek. Output silence until the hook's PRE has run; the
    // next processBlock then starts the segment on a fresh cycle position.
    if (! preSeen_.load(std::memory_order_acquire))
    {
        for (int c = 0; c < 2; ++c)
            if (p[c] != nullptr) std::memset(p[c], 0, sizeof(float) * static_cast<size_t>(n > 0 ? n : 0));
        return true;
    }
    segmentGen_ = configGen_.load(std::memory_order_acquire);
    engine_.process(p, p, n, playPos, playing, segmentGen_);
    if (playing) pdcObs_.onProcessBlock(n, playPos, hookCycles_.load(std::memory_order_acquire));   // stopped blocks: no catch-up event
    if (playing && n > 0) sendFrames_.store(sendFrames_.load(std::memory_order_relaxed) + static_cast<uint64_t>(n), std::memory_order_relaxed);
    return true;
}

void Instance::onPre(int64_t cyclePos, int len, const ReaSample* const* in, int nIn) noexcept
{
    if (len <= 0 || len > engine::kBMax || ! active_.load(std::memory_order_acquire)) return;
    const Map m = unpack(map_.load(std::memory_order_acquire));
    float* s[2] { inScratch_.get(), inScratch_.get() + engine::kBMax };
    for (int c = 0; c < 2; ++c)
    {
        const int ch = m.in[c];
        const ReaSample* src = (ch >= 0 && ch < nIn) ? in[ch] : nullptr;
        if (src == nullptr || (c == 1 && ! m.retStereo)) { std::memset(s[c], 0, sizeof(float) * static_cast<size_t>(len)); continue; }
        for (int k = 0; k < len; ++k) s[c][k] = static_cast<float>(src[k]);
    }
    const float* hw[2] { s[0], s[1] };
    engine_.onPre(cyclePos, len, hw);
    hookCycles_.fetch_add(1, std::memory_order_acq_rel);
    preSeen_.store(true, std::memory_order_release);
}

void Instance::onPost(int64_t cyclePos, int len, ReaSample* const* out, int nOut) noexcept
{
    if (len <= 0 || len > engine::kBMax || ! active_.load(std::memory_order_acquire)) return;
    const Map m = unpack(map_.load(std::memory_order_acquire));
    float* s[2] { outScratch_.get(), outScratch_.get() + engine::kBMax };
    if (! engine_.onPost(cyclePos, len, s)) return;   // fenced / unprepared: the hardware outputs stay untouched
    {
        // F1 §9 Q9: this cycle counts as stable when the engine's stable-cycle run grew (every returned tag == p − R, none bad)
        const auto st = engine_.status();
        pdcObs_.onCycle(st.stableCycles > lastStable_, st.continuityOk);
        lastStable_ = st.stableCycles;
    }
    for (int c = 0; c < (m.sendStereo ? 2 : 1); ++c)
    {
        const int ch = m.out[c];
        ReaSample* dst = (ch >= 0 && ch < nOut) ? out[ch] : nullptr;
        if (dst == nullptr) continue;
        for (int k = 0; k < len; ++k) dst[k] = static_cast<ReaSample>(s[c][k]);
    }
}

} // namespace ni::plugin
