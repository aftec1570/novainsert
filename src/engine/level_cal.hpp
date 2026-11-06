// Level CAL maths (VU GUI spec 2026-11-02 §2/§7). Header-only, dependency-free, allocation-free; called on the message thread.
//   SEND CAL:   a 0 VU program (vuRefDbfs) leaves the converter at +4 dBu → sendTrim = 4 − sendDbuAt0dBFS − vuRefDbfs.
//   RETURN CAL: plugin output = plugin input for the reference tone → returnTrimEff_ch = −(sendTrimEff_ch + G_ch), where G_ch is
//               the measured loop gain (return level before the return trim − sent level after the send trim, both AES17).
//   LINK:       one trim moving by d moves the other by −d; both stop at the range edges so unity is never silently broken.
#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ni::engine::cal
{
constexpr int kToneFs = 48000;
constexpr int kTonePeriod = 48;          // 1 kHz at 48 kHz: any multiple of 48 samples loops seamlessly
constexpr int kSettleSamples = 24000;    // 0.5 s after the tone's arrival
constexpr int kWindowSamples = 144000;   // 3 s measuring window
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

// The reference tone at an AES17 level: a sine whose peak is levelDbfs (a −20 dBFS tone peaks at −20 dBFS and leaves a
// +24 dBu-aligned converter at +4 dBu). Its plain RMS is 3.01 dB lower.
inline void makeTone(float* dst, int n, float levelDbfs) noexcept
{
    const double a = std::pow(10.0, levelDbfs / 20.0);
    for (int i = 0; i < n; ++i)
        dst[i] = static_cast<float>(a * std::sin(2.0 * std::numbers::pi * static_cast<double>(i % kTonePeriod) / kTonePeriod));
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
