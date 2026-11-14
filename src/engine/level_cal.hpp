// Level CAL maths (VU GUI spec 2026-11-02 §2/§7). Header-only, dependency-free, allocation-free; called on the message thread.
//   SEND CAL:   a 0 VU program (vuRefDbfs) leaves the converter at +4 dBu → sendTrim = 4 − sendDbuAt0dBFS − vuRefDbfs.
//   RETURN CAL: plugin output = plugin input for the reference tone → returnTrimEff_ch = −(sendTrimEff_ch + G_ch), where G_ch is
//               the measured loop gain (return level before the return trim − sent level after the send trim, both AES17).
//   LINK:       one trim moving by d moves the other by −d; both stop at the range edges so unity is never silently broken.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ni::engine::cal
{
constexpr int kToneHz = 1000;
constexpr double kSettleSeconds = 0.5;   // after the tone's arrival
constexpr double kWindowSeconds = 3.0;   // measuring window, shortened at high rates to fit the capture (windowSamples)

// The looping tone: 100 ms = exactly 100 cycles of 1 kHz at every supported rate (4410 … 19200 samples), so it loops seamlessly.
constexpr int toneLoopSamples(int fs) noexcept { return fs / 10; }
constexpr int kMaxToneLoopSamples = 192000 / 10;
constexpr int settleSamples(int fs) noexcept { return static_cast<int>(kSettleSeconds * fs); }
// 3 s, or less when L_max + settle + 3 s would not fit captureCap samples; always whole tone loops (exact-cycle RMS).
// 44.1 kHz 3.0 s, 48 kHz 3.0 s, 88.2 kHz 1.7 s, 96 kHz 1.5 s, 192 kHz 0.5 s with the engine's 262 144-sample capture.
constexpr int windowSamples(int fs, int32_t lMax, int captureCap) noexcept
{
    const int loop = toneLoopSamples(fs) > 0 ? toneLoopSamples(fs) : 1;
    const int want = static_cast<int>(kWindowSeconds * fs);
    const int room = captureCap - static_cast<int>(lMax) - settleSamples(fs);
    const int w = (want < room ? want : room) / loop * loop;
    return w > 0 ? w : 0;
}
constexpr float kClipDbfs = -0.1f;       // parent §7 clip rule: sample peak ≥ −0.1 dBFS
constexpr float kNoSignalBelowDb = 40.0f;
constexpr float kAes17Db = 3.0103f;      // AES17: a sine's level = its peak = plain RMS + 3.01 dB

inline float sendTrimForPlus4(float sendDbuAt0dBFS, float vuRefDbfs) noexcept { return 4.0f - sendDbuAt0dBFS - vuRefDbfs; }

inline float returnTrimEff(float sendTrimEffDb, float loopGainDb) noexcept { return -(sendTrimEffDb + loopGainDb); }

struct LinkResult { float sendDb, returnDb; };
constexpr float kMovedDb = 1e-4f;   // below this a trim has not moved (parameter normalisation round-off)
inline bool moved(float a, float b) noexcept { return std::fabs(a - b) > kMovedDb; }

// prev* = the values last seen, send/ret = the values now. Only one side moved and link on → the other follows, clamped.
inline LinkResult linkFollow(float prevSend, float prevRet, float send, float ret, bool link, float lo, float hi) noexcept
{
    const bool sMoved = moved(send, prevSend), rMoved = moved(ret, prevRet);
    if (! link || sMoved == rMoved) return { send, ret };
    if (sMoved)
    {
        const float d = std::clamp(send - prevSend, prevRet - hi, prevRet - lo);
        return { prevSend + d, prevRet - d };
    }
    const float d = std::clamp(ret - prevRet, prevSend - hi, prevSend - lo);
    return { prevSend - d, prevRet + d };
}

// Where a trim being dragged may go: its own range, and with LINK on also the room the other trim has left (the other moves by
// the opposite amount, so self + other stays constant and the other stays inside [lo, hi]). The knob stops here, so LINK never
// has to pull it back (user 2026-11-06: the knob glitched on low settings).
inline float linkLimit(float attempted, float self, float other, bool link, float lo, float hi) noexcept
{
    if (! link) return std::clamp(attempted, lo, hi);
    const float sum = self + other;
    const float mn = std::max(lo, sum - hi), mx = std::min(hi, sum - lo);
    return mn > mx ? self : std::clamp(attempted, mn, mx);
}

// The reference tone at an AES17 level: a 1 kHz sine at rate fs whose peak is levelDbfs (a −20 dBFS tone peaks at −20 dBFS and
// leaves a +24 dBu-aligned converter at +4 dBu). Its plain RMS is 3.01 dB lower. The phase is taken modulo one second in whole
// samples, so a run of toneLoopSamples(fs) loops seamlessly.
inline void makeTone(float* dst, int n, float levelDbfs, int fs) noexcept
{
    const double a = std::pow(10.0, levelDbfs / 20.0);
    const int64_t f = fs > 0 ? fs : 1;
    for (int i = 0; i < n; ++i)
    {
        const int64_t ph = (static_cast<int64_t>(i) * kToneHz) % f;
        dst[i] = static_cast<float>(a * std::sin(2.0 * std::numbers::pi * static_cast<double>(ph) / static_cast<double>(f)));
    }
}

struct LoopGain
{
    float gainDb[2] { 0.0f, 0.0f };
    float peakDbfs[2] { -120.0f, -120.0f };
    bool clipped = false;
    bool noSignal = false;
};

// Window [start, start + len) of the captured return (clamped to n). stereo = false: channel L only, gainDb[1] = 0.
inline LoopGain measureLoopGain(const float* l, const float* r, int n, int start, int len, const float sentRmsDbfs[2], bool stereo) noexcept
{
    LoopGain g;
    const int b = std::clamp(start, 0, n), e = std::clamp(start + len, b, n);
    const float clipLin = std::pow(10.0f, kClipDbfs / 20.0f);
    const float* ch[2] = { l, r };
    for (int k = 0; k < (stereo ? 2 : 1); ++k)
    {
        double s = 0.0; float pk = 0.0f;
        for (int i = b; i < e; ++i) { const float x = ch[k][i]; s += static_cast<double>(x) * x; pk = std::max(pk, std::fabs(x)); }
        const double ms = e > b ? s / (e - b) : 0.0;
        const float rms = ms > 1e-14 ? static_cast<float>(10.0 * std::log10(ms)) + kAes17Db : -140.0f;   // AES17, like the sent level
        g.gainDb[k] = rms - sentRmsDbfs[k];
        g.peakDbfs[k] = pk > 1e-7f ? 20.0f * std::log10(pk) : -140.0f;
        g.clipped |= pk >= clipLin;
        g.noSignal |= rms < sentRmsDbfs[k] - kNoSignalBelowDb;
    }
    return g;
}
} // namespace ni::engine::cal
