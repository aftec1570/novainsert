// settings.json -> per-process host objects (final review I1: spec §10 keep-online fallback). Message thread only.
#pragma once
#include "host/render_arbiter.hpp"
#include "plugin/library.hpp"

namespace ni::plugin
{
inline void applySettings(const Settings& s, host::RenderArbiter& arb) noexcept
{
    arb.setKeepOnlineFallback(s.keepOnlineFallback);
}
} // namespace ni::plugin
