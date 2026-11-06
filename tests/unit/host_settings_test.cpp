#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>
#include "host/host_settings.hpp"

using namespace ni::host;

namespace
{
// Fake REAPER track model: 4 tracks, folder chain T0 > T1 > T2 (own), T3 a sibling root.
struct FakeTrack { int perf; FakeTrack* parent; GUID guid; const char* name; };
FakeTrack g_t[4];
void resetTracks()
{
    for (int i = 0; i < 4; ++i) { g_t[i] = FakeTrack { 0, nullptr, GUID {}, "" }; g_t[i].guid.Data1 = static_cast<unsigned long>(100 + i); }
    g_t[0].name = "Folder A"; g_t[1].name = "Folder B"; g_t[2].name = "Own"; g_t[3].name = "Other";
    g_t[1].parent = &g_t[0]; g_t[2].parent = &g_t[1];
}
FakeTrack* ft(MediaTrack* t) { return reinterpret_cast<FakeTrack*>(t); }
MediaTrack* mt(FakeTrack* t) { return reinterpret_cast<MediaTrack*>(t); }
MediaTrack* fakeParent(MediaTrack* t) { return mt(ft(t)->parent); }
double fakeGet(MediaTrack* t, const char* p) { return std::strcmp(p, "I_PERFFLAGS") == 0 ? ft(t)->perf : 0.0; }
bool fakeSet(MediaTrack* t, const char* p, double v) { if (std::strcmp(p, "I_PERFFLAGS")) return false; ft(t)->perf = static_cast<int>(v); return true; }
GUID* fakeGuid(MediaTrack* t) { return &ft(t)->guid; }
bool fakeName(MediaTrack* t, char* buf, int sz) { std::snprintf(buf, (size_t) sz, "%s", ft(t)->name); return true; }
ReaperApi fakeApi()
{
    ReaperApi a; a.GetParentTrack = &fakeParent; a.GetMediaTrackInfo_Value = &fakeGet; a.SetMediaTrackInfo_Value = &fakeSet;
    a.GetTrackGUID = &fakeGuid; a.GetTrackName = &fakeName; return a;
}
}

TEST_CASE("host settings: require sets bit 2 on the track and both ancestors")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    REQUIRE((g_t[2].perf & 2) == 2);
    REQUIRE((g_t[1].perf & 2) == 2);
    REQUIRE((g_t[0].perf & 2) == 2);
    REQUIRE(g_t[3].perf == 0);
}

TEST_CASE("host settings: release restores only tracks whose previous value had bit 2 clear and that nobody else requires")
{
    resetTracks(); g_t[0].perf = 2; ReaperApi api = fakeApi(); HostSettings h(api);   // the user already had it on for Folder A
    h.require(1, mt(&g_t[2]));
    h.require(2, mt(&g_t[1]));                       // instance 2 lives on Folder B: requires T1 and T0
    h.release(1);
    REQUIRE(g_t[2].perf == 0);                       // only instance 1 required Own
    REQUIRE((g_t[1].perf & 2) == 2);                 // still required by instance 2
    REQUIRE((g_t[0].perf & 2) == 2);
    h.release(2);
    REQUIRE(g_t[1].perf == 0);
    REQUIRE(g_t[0].perf == 2);                       // previous value had the bit set: never cleared by us
}

TEST_CASE("host settings: a value the user changed since our write is never overwritten on release")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    g_t[2].perf = 2 | 1;                             // user changed other bits after our write
    h.release(1);
    REQUIRE(g_t[2].perf == (2 | 1));
}

TEST_CASE("host settings: recheck after a reorder requires the new ancestor")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    g_t[2].parent = &g_t[3];                          // Own moved under Other
    h.recheck();
    REQUIRE((g_t[3].perf & 2) == 2);
    REQUIRE((g_t[2].perf & 2) == 2);
    h.release(1);
    REQUIRE(g_t[3].perf == 0);
    REQUIRE(g_t[2].perf == 0);
}

TEST_CASE("host settings: a cleared bit reports one violation, reapply sets it again, no automatic re-set")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    std::vector<std::pair<int, std::string>> v;
    h.onViolation = [&](int id, std::string name) { v.emplace_back(id, std::move(name)); };
    h.require(1, mt(&g_t[2]));
    g_t[1].perf = 0;                                  // user turned anticipative FX back on for Folder B
    h.recheck(); h.recheck(); h.recheck();
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].first == 1);
    REQUIRE(v[0].second == "Folder B");
    REQUIRE(g_t[1].perf == 0);                        // never set automatically
    REQUIRE(h.reapply(1));
    REQUIRE((g_t[1].perf & 2) == 2);
    g_t[1].perf = 0;                                  // cleared again after the re-apply: reported once more, still no auto-set
    h.recheck(); h.recheck();
    REQUIRE(v.size() == 2);
    REQUIRE(g_t[1].perf == 0);
}

namespace
{
// S2 review I3: tracks REAPER has deleted must never be dereferenced (ValidatePtr2 on the message thread).
FakeTrack* g_deleted = nullptr;
int g_getsOnDeleted = 0;
bool fakeValidate(ReaProject*, void* p, const char* type) { return std::strcmp(type, "MediaTrack*") == 0 && p != g_deleted; }
double countingGet(MediaTrack* t, const char* p) { if (ft(t) == g_deleted) ++g_getsOnDeleted; return fakeGet(t, p); }
bool countingSet(MediaTrack* t, const char* p, double v) { if (ft(t) == g_deleted) ++g_getsOnDeleted; return fakeSet(t, p, v); }
ReaperApi validatingApi() { ReaperApi a = fakeApi(); a.ValidatePtr2 = &fakeValidate; a.GetMediaTrackInfo_Value = &countingGet; a.SetMediaTrackInfo_Value = &countingSet; return a; }
}

TEST_CASE("host settings: a deleted ancestor is dropped on recheck without touching its pointer")
{
    resetTracks(); g_deleted = nullptr; g_getsOnDeleted = 0; ReaperApi api = validatingApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    g_t[2].parent = &g_t[0];                          // user deleted Folder B: Own now sits directly under Folder A
    g_deleted = &g_t[1];
    h.recheck();
    REQUIRE(g_getsOnDeleted == 0);
    REQUIRE((g_t[0].perf & 2) == 2);
    h.release(1);
    REQUIRE(g_getsOnDeleted == 0);
    REQUIRE(g_t[0].perf == 0);
    REQUIRE(g_t[2].perf == 0);
}

TEST_CASE("host settings: release and reapply skip a deleted own track")
{
    resetTracks(); g_deleted = nullptr; g_getsOnDeleted = 0; ReaperApi api = validatingApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    g_deleted = &g_t[2];
    REQUIRE_FALSE(h.reapply(1));
    h.release(1);
    REQUIRE(g_getsOnDeleted == 0);
    REQUIRE(g_t[1].perf == 0);                        // the still-valid ancestors were restored
    REQUIRE(g_t[0].perf == 0);
}

TEST_CASE("host settings: a record found by GUID takes the current track pointer")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    h.require(1, mt(&g_t[2]));
    // Delete + undo within one recheck period: same GUID, new object (T3 takes over T1's identity).
    g_t[3].guid = g_t[1].guid; g_t[3].perf = g_t[1].perf; g_t[3].parent = &g_t[0]; g_t[2].parent = &g_t[3]; g_t[1].parent = nullptr;
    h.recheck();
    g_t[3].perf = 0;                                  // user clears the bit on the new object
    std::vector<int> v; h.onViolation = [&](int id, std::string) { v.push_back(id); };
    h.recheck();
    REQUIRE(v == std::vector<int> { 1 });             // read through the new pointer, not the stale one
}

// S2 review I4: once the user (or another instance's reapply) sets the bit again, every requirer is told.
TEST_CASE("host settings: a violation cleared by the user is reported to every requirer once")
{
    resetTracks(); ReaperApi api = fakeApi(); HostSettings h(api);
    std::vector<int> cleared;
    h.onViolationCleared = [&](int id) { cleared.push_back(id); };
    h.require(1, mt(&g_t[2]));
    h.require(2, mt(&g_t[1]));                        // both require Folder B
    g_t[1].perf = 0;
    h.recheck();
    REQUIRE(cleared.empty());
    g_t[1].perf = 2;                                  // user sets it again in REAPER
    h.recheck(); h.recheck();
    REQUIRE(cleared == std::vector<int> { 1, 2 });
    g_t[1].perf = 0;
    h.recheck();
    REQUIRE(h.reapply(1));                            // reapply from instance 1 clears it for instance 2 too
    h.recheck();
    REQUIRE(cleared == std::vector<int> { 1, 2, 1, 2 });
}
