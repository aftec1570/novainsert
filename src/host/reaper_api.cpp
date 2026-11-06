#include "host/reaper_api.hpp"
#include <cstdlib>
#include <cstring>

namespace ni::host
{
namespace
{
template <typename T>
void resolveInto(T& slot, const ApiResolver& r, const char* name, bool required, std::vector<std::string>& missing)
{
    void* p = r(name);
    slot = reinterpret_cast<T>(p);
    if (p == nullptr && required)
        missing.emplace_back(name);
}
} // namespace

ApiStatus loadReaperApi(const ApiResolver& resolver, ReaperApi& out, std::vector<std::string>& missing)
{
    out = ReaperApi {};
    if (!resolver)
        return ApiStatus::NotReaper;

#define NI_REQ(Name) resolveInto(out.Name, resolver, #Name, true, missing)
#define NI_OPT(Name) resolveInto(out.Name, resolver, #Name, false, missing)
    NI_REQ(Audio_RegHardwareHook);
    NI_REQ(IsInRealTimeAudio);
    NI_REQ(Audio_IsRunning);
    NI_REQ(Audio_IsPreBuffer);
    NI_REQ(GetInputOutputLatency);
    NI_REQ(GetAudioDeviceInfo);
    NI_REQ(GetNumAudioInputs);
    NI_REQ(GetNumAudioOutputs);
    NI_REQ(GetInputChannelName);
    NI_REQ(GetOutputChannelName);
    NI_REQ(GetAppVersion);
    NI_REQ(GetResourcePath);
    NI_REQ(time_precise);
    NI_REQ(plugin_register);
    NI_REQ(get_config_var);
    NI_REQ(projectconfig_var_getoffs);
    NI_REQ(projectconfig_var_addr);
    NI_REQ(EnumProjects);
    NI_REQ(GetPlayState);
    NI_REQ(GetPlayStateEx);
    NI_REQ(Main_OnCommand);
    NI_OPT(GetMainHwnd);
    NI_OPT(ShowConsoleMsg);
    NI_OPT(kbd_getTextFromCmd);
    NI_OPT(SectionFromUniqueID);
    NI_OPT(TrackFX_GetNamedConfigParm);
    NI_OPT(TrackFX_SetNamedConfigParm);
    NI_REQ(CountTracks);
    NI_REQ(GetTrack);
    NI_REQ(ValidatePtr2);
    NI_REQ(GetParentTrack);
    NI_REQ(GetMediaTrackInfo_Value);
    NI_REQ(SetMediaTrackInfo_Value);
    NI_REQ(GetSetMediaTrackInfo_String);
    NI_REQ(GetTrackName);
    NI_REQ(GetTrackGUID);
    NI_REQ(guidToString);
    NI_REQ(CSurf_TrackToID);
    NI_REQ(GetTrackNumSends);
    NI_REQ(GetTrackSendInfo_Value);
#undef NI_REQ
#undef NI_OPT

    return missing.empty() ? ApiStatus::Ok : ApiStatus::MissingSymbols;
}

DeviceKey readDeviceKey(const ReaperApi& api)
{
    DeviceKey k;
    if (api.GetAudioDeviceInfo == nullptr)
        return k;

    char buf[512];
    auto read = [&](const char* attr) -> std::string {
        buf[0] = '\0';
        if (!api.GetAudioDeviceInfo(attr, buf, (int) sizeof(buf)))
            return {};
        k.open = true;
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    };
    k.identIn = read("IDENT_IN");
    k.identOut = read("IDENT_OUT");
    k.sampleRate = std::atoi(read("SRATE").c_str());
    k.bufferSize = std::atoi(read("BSIZE").c_str());

    // REAPER 7.83 reports the system default device as "CoreAudio Default" (IDENT_IN/IDENT_OUT); older observations
    // (F0.1, attribute IDENT) saw an empty string. Both mean "not a stable identity".
    const auto unidentified = [](const std::string& s) { return s.empty() || s == "<default>" || s == "CoreAudio Default"; };
    k.isDefault = unidentified(k.identIn) || unidentified(k.identOut) || k.sampleRate <= 0 || k.bufferSize <= 0;
    return k;
}

} // namespace ni::host
