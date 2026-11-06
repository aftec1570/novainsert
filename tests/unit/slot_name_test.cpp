#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "host/slot_name.hpp"

using namespace ni::host;

namespace
{
// Fake REAPER track FX chain: per slot its original name (fx_name) and its instance name (renamed_name).
struct FakeSlot { std::string fxName, renamed; };
std::map<int, FakeSlot> g_chain;
int g_sets = 0;
MediaTrack* const kTrack = reinterpret_cast<MediaTrack*> (0x1000);

bool fakeGet (MediaTrack* tr, int fx, const char* parm, char* buf, int sz)
{
    auto it = g_chain.find (fx);
    if (tr != kTrack || it == g_chain.end()) return false;
    const std::string& v = ! std::strcmp (parm, "fx_name") ? it->second.fxName : ! std::strcmp (parm, "renamed_name") ? it->second.renamed : std::string ("\x01");
    if (v == "\x01") return false;
    std::snprintf (buf, static_cast<size_t> (sz), "%s", v.c_str()); return true;
}
bool fakeSet (MediaTrack* tr, int fx, const char* parm, const char* value)
{
    auto it = g_chain.find (fx);
    if (tr != kTrack || it == g_chain.end() || std::strcmp (parm, "renamed_name") != 0) return false;
    it->second.renamed = value; ++g_sets; return true;
}
ReaperApi fakeApi() { ReaperApi a; a.TrackFX_GetNamedConfigParm = &fakeGet; a.TrackFX_SetNamedConfigParm = &fakeSet; return a; }
void chain (std::map<int, FakeSlot> c) { g_chain = std::move (c); g_sets = 0; }
}

TEST_CASE("slot name: the selected device's name replaces NovaInsert in the mixer")
{
    chain ({ { 0, { "VST: ReaEQ (Cockos)", "" } }, { 1, { "VST3: NovaInsert (Hillside Studios)", "" } } });
    REQUIRE (syncSlotName (fakeApi(), kTrack, 1, "Pultec EQP-1A") == SlotSync::Written);
    REQUIRE (g_chain[1].renamed == "Pultec EQP-1A");
    REQUIRE (g_chain[0].renamed.empty());
}

TEST_CASE("slot name: nothing is written while the slot already shows the device name")
{
    chain ({ { 0, { "VST3: NovaInsert (Hillside Studios)", "Pultec EQP-1A" } } });
    REQUIRE (syncSlotName (fakeApi(), kTrack, 0, "Pultec EQP-1A") == SlotSync::Unchanged);
    REQUIRE (g_sets == 0);
}

TEST_CASE("slot name: a manual rename is reverted to the device name")
{
    chain ({ { 0, { "VST3: NovaInsert (Hillside Studios)", "my comp" } } });
    REQUIRE (syncSlotName (fakeApi(), kTrack, 0, "LA-2A") == SlotSync::Written);
    REQUIRE (g_chain[0].renamed == "LA-2A");
}

TEST_CASE("slot name: no device restores the plain NovaInsert name")
{
    chain ({ { 0, { "VST3: NovaInsert (Hillside Studios)", "LA-2A" } } });
    REQUIRE (syncSlotName (fakeApi(), kTrack, 0, "") == SlotSync::Written);
    REQUIRE (g_chain[0].renamed.empty());
}

TEST_CASE("slot name: a slot that is not a NovaInsert is never renamed")
{
    chain ({ { 0, { "VST: ReaEQ (Cockos)", "" } } });
    REQUIRE (syncSlotName (fakeApi(), kTrack, 0, "LA-2A") == SlotSync::NotOurs);
    REQUIRE (g_sets == 0);
}

TEST_CASE("slot name: without the REAPER FX config API nothing happens")
{
    chain ({ { 0, { "VST3: NovaInsert (Hillside Studios)", "" } } });
    REQUIRE (syncSlotName (ReaperApi {}, kTrack, 0, "LA-2A") == SlotSync::Unsupported);
    REQUIRE (syncSlotName (fakeApi(), nullptr, 0, "LA-2A") == SlotSync::Unsupported);
    REQUIRE (g_sets == 0);
}

TEST_CASE("slot name: REAPER's chain-index context is 1-based, 0 = unknown (bench 2026-11-02, VST3)")
{
    REQUIRE (chainIndexFromContext (1) == 0);
    REQUIRE (chainIndexFromContext (3) == 2);
    REQUIRE (chainIndexFromContext (0) == -1);
}
