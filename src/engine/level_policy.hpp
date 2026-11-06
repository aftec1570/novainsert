// NovaInsert probe level policy (F1 §8 "Levels", v3 §7.3/§7.4; plan Task 14). Start at −36 dBFS peak on an unknown path,
// −24 dBFS peak when the stored record's last return peak ≤ H_ret and G_peak is known. Next level
// P = min(−12 − sendTrim, T_ret − G_peak, current + 12) so the predicted return peak P + G_peak lands on T_ret = −12 with
// 6 dB margin to H_ret = −6. No raise (nullopt) when G_peak is not estimable or the previous probe was unsafe.
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace ni::engine
{

struct LevelHistory { bool known; float lastReturnPeakDb; float gainPeakDb; };
struct ProbeObs { float returnPeakDb; float gainPeakDb; bool clipped; bool lockPresent; bool overload; bool genStable; };

class LevelPolicy
{
public:
    static constexpr float kStartUnknownDb = -36.0f, kStartKnownDb = -24.0f;
    static constexpr float kSendCeilingDb = -12.0f, kTargetReturnDb = -12.0f, kReturnCeilingDb = -6.0f;
    static constexpr float kMaxStepDb = 12.0f, kClipDb = -0.1f, kMinReturnDb = -60.0f;

    static float startLevelDb(const LevelHistory& h) noexcept
    {
        return h.known && std::isfinite(h.gainPeakDb) && h.lastReturnPeakDb <= kReturnCeilingDb ? kStartKnownDb : kStartUnknownDb;
    }

    static std::optional<float> nextLevelDb(float current, const ProbeObs& o, float sendTrimDb) noexcept
    {
        if (o.clipped || o.returnPeakDb >= kClipDb || o.overload || !o.genStable) return std::nullopt;
        if (!o.lockPresent || !(o.returnPeakDb >= kMinReturnDb) || !std::isfinite(o.gainPeakDb)) return std::nullopt;   // G_peak not estimable
        const float next = std::min({ kSendCeilingDb - sendTrimDb, kTargetReturnDb - o.gainPeakDb, current + kMaxStepDb });
        if (next + o.gainPeakDb > kReturnCeilingDb) return std::nullopt;
        return next;
    }
};

} // namespace ni::engine
