// BARS meter view scale maths (user request 2026-11-05: switchable VU / ReaInsert-style level bars). JUCE-free so it is unit
// tested directly (tests/unit/bar_scale_test.cpp). The bar spans −60 … 0 dBFS linearly (REAPER's track meter range); the
// dBu scale above a bar group is the same axis through the converter alignment (dBu = dBFS + dBuAt0dBFS), so the +4 dBu
// reference tick lands where the SEND VU's 0 VU is. The peak bar rises at once and falls at kBarFallDbPerS (decision
// 2026-11-05: 24 dB/s ≈ 2.5 s over the whole scale, the feel of REAPER's own peak meters); RMS and the peak hold come
// from the meter engine as they are.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace ni::gui
{
constexpr float kBarMinDb = -60.0f, kBarMaxDb = 0.0f, kBarFloorDb = -120.0f, kBarFallDbPerS = 24.0f;

inline float barPosition(float dbfs) noexcept { return std::clamp((dbfs - kBarMinDb) / (kBarMaxDb - kBarMinDb), 0.0f, 1.0f); }

struct DbuTick { float dbu, dbfs; bool reference; };   // reference = the +4 dBu program-level tick (bold, amber)

inline std::vector<DbuTick> dbuTicks(float dbuAt0dBFS)
{
    std::vector<DbuTick> out;
    for (float dbu : { -30.0f, -24.0f, -18.0f, -12.0f, -6.0f, 0.0f, 4.0f, 12.0f, 18.0f, 24.0f })
    {
        const float dbfs = dbu - dbuAt0dBFS;
        if (dbfs >= kBarMinDb - 1e-4f && dbfs <= kBarMaxDb + 1e-4f) out.push_back({ dbu, dbfs, dbu > 3.5f && dbu < 4.5f });   // the +4 dBu tick (no float ==)
    }
    return out;
}

inline std::vector<float> dbfsTicks()
{
    std::vector<float> out;
    for (float v = kBarMinDb; v <= kBarMaxDb + 1e-4f; v += 6.0f) out.push_back(v);
    return out;
}

struct BarFall
{
    float shownDb = kBarFloorDb;
    float advance(double dt, float peakDb) noexcept
    {
        const float fallen = shownDb - kBarFallDbPerS * static_cast<float>(std::min(dt, 10.0));
        shownDb = std::max(kBarFloorDb, std::max(peakDb, fallen));
        return shownDb;
    }
};

// Meter view rocker (user 2026-11-06: "All 3 looks cool, make all 3 of them selectable"): VU faces or one of three bar looks —
// TUBE (backlit cream face, 3D glass light-pipes), LED (smoked glass, domed LED segments), TWIN (two VU-shaped windows).
// The per-process preference is stored as the rocker index; 1 was BARS before and stays bars (TUBE).
enum class MeterView : int { Vu = 0, Tube = 1, Led = 2, Twin = 3 };
constexpr int kMeterViewCount = 4;
inline MeterView meterViewFromStored(int v) noexcept { return v >= 0 && v < kMeterViewCount ? static_cast<MeterView>(v) : MeterView::Vu; }
inline bool isBarView(MeterView v) noexcept { return v != MeterView::Vu; }

// LED look: 60 one-dB segments; segment k spans [lo, lo + 1) dBFS with lo = −60 + k. Full under the RMS and at the hold,
// the dimmer peak band from the RMS up to the peak, dark above.
constexpr float kLedPeakIntensity = 0.62f;
inline float ledIntensity(float lo, float rmsDb, float peakDb, float holdDb) noexcept
{
    if (lo < rmsDb) return 1.0f;
    if (holdDb > kBarFloorDb && holdDb >= lo && holdDb < lo + 1.0f) return 1.0f;
    if (lo < peakDb) return kLedPeakIntensity;
    return 0.0f;
}

// colour zone of a level: 0 green (working range), 1 yellow (from the −20 dBFS 0 VU reference), 2 orange (from −9), 3 red (last 3 dB)
inline int barZone(float dbfs) noexcept { return dbfs >= -3.0f ? 3 : dbfs >= -9.0f ? 2 : dbfs >= -20.0f ? 1 : 0; }
} // namespace ni::gui
