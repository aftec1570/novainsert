#include "host/slot_name.hpp"

namespace ni::host
{

SlotSync syncSlotName(const ReaperApi& api, MediaTrack* track, int fxIndex, const std::string& wanted)
{
    if (api.TrackFX_GetNamedConfigParm == nullptr || api.TrackFX_SetNamedConfigParm == nullptr || track == nullptr || fxIndex < 0)
        return SlotSync::Unsupported;
    char buf[512] = "";
    if (! api.TrackFX_GetNamedConfigParm (track, fxIndex, "fx_name", buf, sizeof (buf)) || std::string (buf).find ("NovaInsert") == std::string::npos)
        return SlotSync::NotOurs;
    buf[0] = '\0';
    if (! api.TrackFX_GetNamedConfigParm (track, fxIndex, "renamed_name", buf, sizeof (buf))) return SlotSync::Unsupported;
    if (wanted == buf) return SlotSync::Unchanged;
    return api.TrackFX_SetNamedConfigParm (track, fxIndex, "renamed_name", wanted.c_str()) ? SlotSync::Written : SlotSync::Unsupported;
}

} // namespace ni::host
