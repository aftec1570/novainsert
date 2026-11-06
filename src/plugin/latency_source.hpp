#pragma once
// Where an instance's loop latency comes from (Effect PDC spec 2026-11-04 §2; F1 §3/§8/§20): Insert measures it (probe); Effect
// never probes and takes the driver's own round-trip — GetInputOutputLatency in + out at the current buffer size — as physLoop,
// reported with the same margin rule as a measured loop (Instance::reportedFor). A negative driver value or a round-trip beyond
// kLMax − 2B: no probe, OutOfRange. Read on the message thread only.
#include <cstdint>
#include "engine/types.hpp"

namespace ni::plugin {

struct LatencySource
{
    bool probe = false;
    int32_t phys = -1;
    engine::Reason fail = engine::Reason::None;
};

inline LatencySource latencySource(engine::Mode mode, int32_t driverIn, int32_t driverOut, int B) noexcept
{
    if (mode == engine::Mode::Insert) return { true, -1, engine::Reason::None };
    if (driverIn < 0 || driverOut < 0) return { false, -1, engine::Reason::OutOfRange };
    const int64_t phys = static_cast<int64_t>(driverIn) + driverOut;
    if (phys > static_cast<int64_t>(engine::kLMax) - 2 * static_cast<int64_t>(B)) return { false, -1, engine::Reason::OutOfRange };
    return { false, static_cast<int32_t>(phys), engine::Reason::None };
}

}   // namespace ni::plugin
