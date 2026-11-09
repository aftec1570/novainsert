#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include "host/platform.hpp"
#include "host/reaper_api.hpp"

using namespace ni::host;

namespace
{
// Fake resolver: every name resolves to a non-null dummy except the names listed in g_missing.
std::vector<std::string> g_missing;
void dummy() {}
void* fakeResolver(const char* name)
{
    for (auto& m : g_missing) if (m == name) return nullptr;
    return reinterpret_cast<void*>(&dummy);
}
// Fake GetAudioDeviceInfo for readDeviceKey tests.
const char* g_identIn = "BlackHole 64ch"; const char* g_identOut = "BlackHole 64ch"; const char* g_mode = "";
bool fakeGetAudioDeviceInfo(const char* attr, char* buf, int bufSz)
{
    const char* v = nullptr;
    if (!std::strcmp(attr, "IDENT_IN")) v = g_identIn;
    else if (!std::strcmp(attr, "IDENT_OUT")) v = g_identOut;
    else if (!std::strcmp(attr, "SRATE")) v = "48000";
    else if (!std::strcmp(attr, "BSIZE")) v = "64";
    else if (!std::strcmp(attr, "MODE")) v = g_mode;
    else return false;
    std::snprintf(buf, (size_t) bufSz, "%s", v); return true;
}
}

TEST_CASE("null getReaperApi -> NotReaper")
{
    ReaperApi a; std::vector<std::string> m;
    REQUIRE(loadReaperApi(nullptr, a, m) == ApiStatus::NotReaper);
    REQUIRE(a.Audio_RegHardwareHook == nullptr);
}

TEST_CASE("all symbols present -> Ok and pointers filled")
{
    g_missing.clear(); ReaperApi a; std::vector<std::string> m;
    REQUIRE(loadReaperApi(&fakeResolver, a, m) == ApiStatus::Ok);
    REQUIRE(m.empty());
    REQUIRE(a.Audio_RegHardwareHook != nullptr);
    REQUIRE(a.GetResourcePath != nullptr);
    REQUIRE(a.GetMainHwnd != nullptr);
}

TEST_CASE("missing Audio_RegHardwareHook -> MissingSymbols names it")
{
    g_missing = { "Audio_RegHardwareHook" }; ReaperApi a; std::vector<std::string> m;
    REQUIRE(loadReaperApi(&fakeResolver, a, m) == ApiStatus::MissingSymbols);
    REQUIRE(m == std::vector<std::string> { "Audio_RegHardwareHook" });
    g_missing.clear();
}

TEST_CASE("optional symbols missing -> still Ok")
{
    g_missing = { "GetMainHwnd", "kbd_getTextFromCmd", "SectionFromUniqueID" }; ReaperApi a; std::vector<std::string> m;
    REQUIRE(loadReaperApi(&fakeResolver, a, m) == ApiStatus::Ok);
    REQUIRE(a.GetMainHwnd == nullptr);
    REQUIRE(a.Audio_RegHardwareHook != nullptr);
    g_missing.clear();
}

TEST_CASE("readDeviceKey parses SRATE/BSIZE and identities")
{
    ReaperApi a; a.GetAudioDeviceInfo = &fakeGetAudioDeviceInfo;
    g_identIn = "BlackHole 64ch"; g_identOut = "BlackHole 64ch";
    const DeviceKey k = readDeviceKey(a);
    REQUIRE(k.sampleRate == 48000);
    REQUIRE(k.bufferSize == 64);
    REQUIRE(k.identIn == "BlackHole 64ch");
    REQUIRE_FALSE(k.isDefault);
}

TEST_CASE("readDeviceKey flags <default> and empty identity")
{
    ReaperApi a; a.GetAudioDeviceInfo = &fakeGetAudioDeviceInfo;
    g_identIn = "<default>"; g_identOut = "BlackHole 64ch";
    REQUIRE(readDeviceKey(a).isDefault);
    g_identIn = "BlackHole 64ch"; g_identOut = "";
    REQUIRE(readDeviceKey(a).isDefault);
    g_identOut = "BlackHole 64ch";
}

TEST_CASE("readDeviceKey without GetAudioDeviceInfo is Unavailable (isDefault)")
{
    ReaperApi a;
    REQUIRE(readDeviceKey(a).isDefault);
}

namespace
{
bool closedGetAudioDeviceInfo(const char*, char*, int) { return false; }
}

TEST_CASE("readDeviceKey reports a closed device (every attribute fails) as not open and not persistable")
{
    ReaperApi a; a.GetAudioDeviceInfo = &closedGetAudioDeviceInfo;
    const DeviceKey k = readDeviceKey(a);
    REQUIRE_FALSE(k.open);
    REQUIRE(k.isDefault);
}

TEST_CASE("readDeviceKey reports an open named device as open")
{
    ReaperApi a; a.GetAudioDeviceInfo = &fakeGetAudioDeviceInfo;
    g_identIn = "BlackHole 64ch"; g_identOut = "BlackHole 64ch";
    REQUIRE(readDeviceKey(a).open);
    g_identIn = "<default>";
    const DeviceKey k = readDeviceKey(a);
    REQUIRE(k.open);
    REQUIRE(k.isDefault);
    g_identIn = "BlackHole 64ch";
}

TEST_CASE("readDeviceKey treats REAPER's 'CoreAudio Default' identity as the default device")
{
    ReaperApi a; a.GetAudioDeviceInfo = &fakeGetAudioDeviceInfo;
    g_identIn = "CoreAudio Default"; g_identOut = "CoreAudio Default";
    const DeviceKey k = readDeviceKey(a);
    REQUIRE(k.open);
    REQUIRE(k.isDefault);
    g_identIn = "CoreAudio BlackHole 64ch"; g_identOut = "CoreAudio Default";
    REQUIRE(readDeviceKey(a).isDefault);
    g_identIn = "CoreAudio BlackHole 64ch"; g_identOut = "CoreAudio BlackHole 64ch";
    REQUIRE_FALSE(readDeviceKey(a).isDefault);
}

TEST_CASE("track FX named-config API is resolved but optional (own slot name)")
{
    g_missing.clear(); ReaperApi a; std::vector<std::string> m;
    REQUIRE(loadReaperApi(&fakeResolver, a, m) == ApiStatus::Ok);
    REQUIRE(a.TrackFX_GetNamedConfigParm != nullptr);
    REQUIRE(a.TrackFX_SetNamedConfigParm != nullptr);
    g_missing = { "TrackFX_GetNamedConfigParm", "TrackFX_SetNamedConfigParm" }; ReaperApi b; m.clear();
    REQUIRE(loadReaperApi(&fakeResolver, b, m) == ApiStatus::Ok);
    REQUIRE(b.TrackFX_SetNamedConfigParm == nullptr);
    g_missing.clear();
}

TEST_CASE("driverVerdict: family prefix match", "[reaper_api]")          // Review Focus 1
{
    using V = ni::host::DriverVerdict;
    const std::string ok = ni::host::platform::kSupportedDriver;
    std::string lower = ok;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    REQUIRE(ni::host::driverVerdict(ok) == V::Supported);
    REQUIRE(ni::host::driverVerdict(ok + " (RME Fireface USB)") == V::Supported);
    REQUIRE(ni::host::driverVerdict(lower) == V::Supported);
    REQUIRE(ni::host::driverVerdict(ok + "X") == V::Unsupported);   // a token, not a raw prefix
    REQUIRE(ni::host::driverVerdict("Dummy Audio") == V::Unsupported);
    REQUIRE(ni::host::driverVerdict("WASAPI") == V::Unsupported);
    REQUIRE(ni::host::driverVerdict("JACK") == V::Unsupported);
}

TEST_CASE("driverVerdict: empty mode passes as Unknown", "[reaper_api]")  // Review Focus 2
{
    REQUIRE(ni::host::driverVerdict("") == ni::host::DriverVerdict::Unknown);
    REQUIRE(ni::host::driverVerdict("   ") == ni::host::DriverVerdict::Unknown);
}

TEST_CASE("readDeviceKey fills mode and default flags per table", "[reaper_api]")
{
    ReaperApi a; a.GetAudioDeviceInfo = &fakeGetAudioDeviceInfo;
    g_identIn = "BlackHole 64ch"; g_identOut = "BlackHole 64ch"; g_mode = "ASIO";
    const DeviceKey key = readDeviceKey(a);
    REQUIRE(key.mode == "ASIO");
    g_mode = "";
    REQUIRE(ni::host::isDefaultDeviceName(""));
    REQUIRE(ni::host::isDefaultDeviceName("<default>"));
    REQUIRE(ni::host::isDefaultDeviceName("CoreAudio Default"));
    REQUIRE(ni::host::isDefaultDeviceName("ALSA default"));   // docs/port/driver-identity.md
    REQUIRE_FALSE(ni::host::isDefaultDeviceName("ALSA 0,0"));
    REQUIRE_FALSE(ni::host::isDefaultDeviceName("Babyface (23456789)"));
}
