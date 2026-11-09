#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>
#include "host/pair_registry.hpp"

using namespace ni::host;

namespace
{
Pair stereoPair(int out0, int in0) { return Pair { { out0, out0 + 1 }, { in0, in0 + 1 }, true, true }; }
Pair monoPair(int out0, int in0) { return Pair { { out0, -1 }, { in0, -1 }, false, false }; }
}

TEST_CASE("pair registry: first claimant owns the pair")
{
    PairRegistry r;
    REQUIRE(r.claim(1, stereoPair(0, 0), "Tr1") == Claim::Owner);
    REQUIRE(r.ownerTrackOf(stereoPair(0, 0)) == std::string("Tr1"));
    REQUIRE_FALSE(r.ownerTrackOf(stereoPair(4, 4)).has_value());
}

TEST_CASE("pair registry: second claimant on the same output is a conflict naming the owner track")
{
    PairRegistry r;
    REQUIRE(r.claim(1, stereoPair(0, 0), "Tr1") == Claim::Owner);
    REQUIRE(r.claim(2, stereoPair(0, 8), "Tr2") == Claim::Conflict);
    REQUIRE(r.ownerTrackOf(stereoPair(0, 8)) == std::string("Tr1"));
}

TEST_CASE("pair registry: overlap on one channel of a stereo pair is a conflict, disjoint pairs are not")
{
    PairRegistry r;
    REQUIRE(r.claim(1, stereoPair(0, 0), "Tr1") == Claim::Owner);
    REQUIRE(r.claim(2, Pair { { 1, 2 }, { 4, 5 }, true }, "Tr2") == Claim::Conflict);   // shares out 1
    REQUIRE(r.claim(3, Pair { { 4, 5 }, { 1, 6 }, true }, "Tr3") == Claim::Conflict);   // shares in 1
    REQUIRE(r.claim(4, stereoPair(2, 2), "Tr4") == Claim::Owner);
    REQUIRE(r.claim(5, monoPair(6, 6), "Tr5") == Claim::Owner);
    REQUIRE(r.claim(6, monoPair(6, 9), "Tr6") == Claim::Conflict);                     // mono out 6 already owned
}

TEST_CASE("pair registry: releasing the owner promotes the oldest waiter exactly once")
{
    PairRegistry r;
    std::vector<int> promoted;
    r.onPromoted = [&](int id) { promoted.push_back(id); };
    REQUIRE(r.claim(1, stereoPair(0, 0), "Tr1") == Claim::Owner);
    REQUIRE(r.claim(2, stereoPair(0, 0), "Tr2") == Claim::Conflict);
    REQUIRE(r.claim(3, stereoPair(0, 0), "Tr3") == Claim::Conflict);
    r.release(1);
    REQUIRE(promoted == std::vector<int> { 2 });
    REQUIRE(r.ownerTrackOf(stereoPair(0, 0)) == std::string("Tr2"));
    r.release(2);
    REQUIRE(promoted == std::vector<int> { 2, 3 });
    r.release(3);
    REQUIRE_FALSE(r.ownerTrackOf(stereoPair(0, 0)).has_value());
    REQUIRE(promoted.size() == 2);
}

TEST_CASE("pair registry: same library device on different pairs warns but does not conflict")
{
    PairRegistry r;
    REQUIRE(r.claim(1, stereoPair(0, 0), "Tr1", "apollo-x8") == Claim::Owner);
    REQUIRE(r.claim(2, stereoPair(2, 2), "Tr2", "apollo-x8") == Claim::Owner);
    REQUIRE(r.sameLibraryDeviceElsewhere(1, "apollo-x8"));
    REQUIRE(r.sameLibraryDeviceElsewhere(2, "apollo-x8"));
    REQUIRE_FALSE(r.sameLibraryDeviceElsewhere(1, "pultec"));
    r.release(2);
    REQUIRE_FALSE(r.sameLibraryDeviceElsewhere(1, "apollo-x8"));
}

namespace
{
int g_fakeTrack = 0;
int fakeCountTracks(ReaProject*) { return 1; }
MediaTrack* fakeGetTrack(ReaProject*, int idx) { return idx == 0 ? reinterpret_cast<MediaTrack*>(&g_fakeTrack) : nullptr; }
int fakeNumSends(MediaTrack*, int category) { return category == 1 ? 1 : 0; }
double fakeSendInfo(MediaTrack*, int category, int sendidx, const char* parm)
{ return (category == 1 && sendidx == 0 && !std::strcmp(parm, "I_DSTCHAN")) ? 8.0 : 0.0; }
bool fakeTrackName(MediaTrack*, char* buf, int sz) { std::snprintf(buf, (size_t) sz, "Bus"); return true; }
}

TEST_CASE("pair registry: hardware-send enumeration names the track and the 1-based output pair")
{
    ReaperApi api;
    api.CountTracks = &fakeCountTracks; api.GetTrack = &fakeGetTrack; api.GetTrackNumSends = &fakeNumSends;
    api.GetTrackSendInfo_Value = &fakeSendInfo; api.GetTrackName = &fakeTrackName;
    PairRegistry r;
    const auto w = r.hardwareSendWarnings(api, nullptr, stereoPair(8, 0));
    REQUIRE(w == std::vector<std::string> { "Bus hardware send on out 9/10" });
    REQUIRE(r.hardwareSendWarnings(api, nullptr, stereoPair(0, 0)).empty());   // send on 9/10 does not touch outs 1/2
    REQUIRE(r.hardwareSendWarnings(api, nullptr, monoPair(9, 0)).size() == 1);  // overlaps the second channel of the send
}

TEST_CASE("pair registry: ownerTrackOf excludes self")
{
    PairRegistry r;
    REQUIRE(r.claim(1, stereoPair(2, 2), "Vox") == Claim::Owner);
    REQUIRE_FALSE(r.ownerTrackOf(stereoPair(2, 2), 1).has_value());
    REQUIRE(r.ownerTrackOf(stereoPair(2, 2), 2) == std::string("Vox"));
    REQUIRE(r.ownerTrackOf(monoPair(3, 3), 2) == std::string("Vox"));
    REQUIRE_FALSE(r.ownerTrackOf(monoPair(6, 6), 2).has_value());
}

TEST_CASE("pair registry: per-direction widths - a mono-send/stereo-return pair owns one output and two inputs")
{
    PairRegistry r;
    const Pair mixed { { 0, 1 }, { 0, 1 }, false, true };   // out 1 only (out[1] ignored), in 1/2
    REQUIRE(r.claim(1, mixed, "Tr1") == Claim::Owner);
    REQUIRE(r.claim(2, Pair { { 1, -1 }, { 4, -1 }, false, false }, "Tr2") == Claim::Owner);      // out 2 is free: the mono send ignores out[1]
    REQUIRE(r.claim(3, Pair { { 6, -1 }, { 1, -1 }, false, false }, "Tr3") == Claim::Conflict);   // in 2 is owned: the stereo return uses both
    REQUIRE(r.ownerTrackOf(Pair { { 1, -1 }, { 9, -1 }, false, false }, 2) == std::nullopt);
}
