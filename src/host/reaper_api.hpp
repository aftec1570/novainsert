// ReaperApi: the REAPER extension-API function-pointer table NovaInsert needs,
// resolved once at plugin load through the host's getReaperApi() lookup
// (REAPER's IReaperHostApplication). Non-RT, called from the message thread only.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "host/reaper_types.hpp"

namespace ni::host
{

using ApiResolver = std::function<void*(const char* name)>;   // null/empty = host is not REAPER

struct ReaperApi
{
    // Audio engine
    int (*Audio_RegHardwareHook)(bool isAdd, audio_hook_register_t* reg) = nullptr;
    int (*IsInRealTimeAudio)() = nullptr;
    int (*Audio_IsRunning)() = nullptr;
    int (*Audio_IsPreBuffer)() = nullptr;
    void (*GetInputOutputLatency)(int* inputlatencyOut, int* outputLatencyOut) = nullptr;
    bool (*GetAudioDeviceInfo)(const char* attribute, char* descOut, int descOut_sz) = nullptr;
    int (*GetNumAudioInputs)() = nullptr;
    int (*GetNumAudioOutputs)() = nullptr;
    const char* (*GetInputChannelName)(int channelIndex) = nullptr;
    const char* (*GetOutputChannelName)(int channelIndex) = nullptr;
    // Host / misc
    const char* (*GetAppVersion)() = nullptr;
    const char* (*GetResourcePath)() = nullptr;
    double (*time_precise)() = nullptr;
    int (*plugin_register)(const char* name, void* infostruct) = nullptr;
    void* (*get_config_var)(const char* name, int* szOut) = nullptr;
    int (*projectconfig_var_getoffs)(const char* name, int* szOut) = nullptr;
    void* (*projectconfig_var_addr)(ReaProject* proj, int idx) = nullptr;
    ReaProject* (*EnumProjects)(int idx, char* projfnOutOptional, int projfnOutOptional_sz) = nullptr;
    int (*GetPlayState)() = nullptr;
    int (*GetPlayStateEx)(ReaProject* proj) = nullptr;
    void (*Main_OnCommand)(int command, int flag) = nullptr;
    HWND (*GetMainHwnd)() = nullptr;                                             // optional
    void (*ShowConsoleMsg)(const char* msg) = nullptr;                           // optional (render refusal message, Task 21)
    const char* (*kbd_getTextFromCmd)(int cmd, KbdSectionInfo* section) = nullptr; // optional
    KbdSectionInfo* (*SectionFromUniqueID)(int uniqueID) = nullptr;              // optional
    // Tracks
    int (*CountTracks)(ReaProject* projOptional) = nullptr;
    MediaTrack* (*GetTrack)(ReaProject* proj, int trackidx) = nullptr;
    bool (*ValidatePtr2)(ReaProject* proj, void* pointer, const char* ctypename) = nullptr;
    MediaTrack* (*GetParentTrack)(MediaTrack* track) = nullptr;
    double (*GetMediaTrackInfo_Value)(MediaTrack* tr, const char* parmname) = nullptr;
    bool (*SetMediaTrackInfo_Value)(MediaTrack* tr, const char* parmname, double newvalue) = nullptr;
    bool (*GetSetMediaTrackInfo_String)(MediaTrack* tr, const char* parmname, char* stringNeedBig, bool setNewValue) = nullptr;
    bool (*GetTrackName)(MediaTrack* track, char* bufOut, int bufOut_sz) = nullptr;
    GUID* (*GetTrackGUID)(MediaTrack* tr) = nullptr;
    void (*guidToString)(const GUID* g, char* destNeed64) = nullptr;
    int (*CSurf_TrackToID)(MediaTrack* track, bool mcpView) = nullptr;
    int (*GetTrackNumSends)(MediaTrack* tr, int category) = nullptr;
    double (*GetTrackSendInfo_Value)(MediaTrack* tr, int category, int sendidx, const char* parmname) = nullptr;
    // Track FX (optional: own slot name, host/slot_name.hpp)
    bool (*TrackFX_GetNamedConfigParm)(MediaTrack* track, int fx, const char* parmname, char* bufOut, int bufOut_sz) = nullptr;
    bool (*TrackFX_SetNamedConfigParm)(MediaTrack* track, int fx, const char* parmname, const char* value) = nullptr;
};

enum class ApiStatus { Ok, NotReaper, MissingSymbols };

// Resolves every entry of `out`. Returns NotReaper when `resolver` is null (host is not
// REAPER), MissingSymbols when any required name resolves to null (names appended to
// `missing`, in table order). Optional entries (GetMainHwnd, kbd_getTextFromCmd,
// SectionFromUniqueID) may stay null with status Ok.
ApiStatus loadReaperApi(const ApiResolver& resolver, ReaperApi& out, std::vector<std::string>& missing);

// Snapshot of the current audio device identity, read via GetAudioDeviceInfo.
// open      false when no attribute is readable (device closed / not yet opened / API missing).
// isDefault true when the device cannot be identified (closed, empty or "<default>" identity):
//           such a key must never be used to persist calibration.
struct DeviceKey
{
    std::string identIn;
    std::string identOut;
    int sampleRate = 0;
    int bufferSize = 0;
    bool open = false;
    bool isDefault = true;
    std::string mode;   // GetAudioDeviceInfo("MODE"), e.g. "ASIO", "ALSA 0,0 16-bit"; empty on macOS (docs/port/driver-identity.md)
};

DeviceKey readDeviceKey(const ReaperApi& api);

// Driver gate (spec 2026-11-09 §4): the first whitespace-delimited token of MODE against platform::kSupportedDriver,
// case-insensitive. An empty MODE (macOS reports none) is Unknown and passes.
enum class DriverVerdict { Supported, Unknown, Unsupported };
DriverVerdict driverVerdict(const std::string& mode) noexcept;

// IDENT_IN / IDENT_OUT values that name no concrete device (docs/port/driver-identity.md): such a key is never persisted.
bool isDefaultDeviceName(const std::string& ident) noexcept;

} // namespace ni::host
