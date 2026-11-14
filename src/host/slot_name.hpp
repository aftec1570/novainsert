// Own FX-slot name (user 2026-11-02): while a device is selected, REAPER's mixer/FX chain shows exactly the device's name
// instead of "NovaInsert", and the slot is not renamable – a manual rename is reverted on the next sync. No device = the plain
// plugin name (renamed_name cleared). Message thread only (REAPER API calls); never from the audio thread.
#pragma once
#include <cstdint>
#include <string>
#include "host/reaper_api.hpp"

namespace ni::host
{

// REAPER's FX context selector 6 (getReaperParent(6) for VST3) answers the position in the chain 1-based, 0 = unknown
// (Apollo bench 2026-11-02: slots 0/1/2 reported 1/2/3). Returns the 0-based FX index, -1 when unknown.
inline int chainIndexFromContext(std::intptr_t raw) { return raw > 0 ? static_cast<int> (raw - 1) : -1; }

enum class SlotSync { Unsupported, NotOurs, Unchanged, Written };

// `fxIndex` = this instance's position in `track`'s FX chain. A slot whose original name (fx_name) is not a NovaInsert is
// never touched (a wrong index must not rename another plug-in). `wanted` empty = restore the default name.
SlotSync syncSlotName(const ReaperApi& api, MediaTrack* track, int fxIndex, const std::string& wanted);

} // namespace ni::host
