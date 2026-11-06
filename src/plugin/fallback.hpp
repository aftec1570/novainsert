// processBlock fallback when the instance did not process the block (no device, inactive, offline/non-realtime render):
// Insert mode outputs silence (the loop is the signal path), Effect mode passes the input through unchanged (F1 §5).
// RT-safe: no allocation, no branches on host state.
#pragma once
#include <cstring>
#include "engine/types.hpp"

namespace ni::plugin
{
inline void applyFallback(engine::Mode mode, float* const* io, int nch, int n) noexcept
{
    if (mode == engine::Mode::Effect) return;
    for (int ch = 0; ch < nch; ++ch)
        if (io[ch] != nullptr) std::memset(io[ch], 0, sizeof(float) * static_cast<size_t>(n));
}
} // namespace ni::plugin
