#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <vector>
#include "engine/types.hpp"

using namespace ni::engine;

static CalibKey baseKey()
{
    CalibKey k{};
    k.identIn = "BlackHole 64ch"; k.identOut = "BlackHole 64ch";
    k.sendIdx[0] = 8; k.sendIdx[1] = 9; k.sendName[0] = "Out 9"; k.sendName[1] = "Out 10";
    k.returnIdx[0] = 8; k.returnIdx[1] = 9; k.returnName[0] = "In 9"; k.returnName[1] = "In 10";
    k.sampleRate = 48000; k.bufferSize = 64; k.sendStereo = false; k.returnStereo = false;
    k.send = SideSel::Stereo; k.ret = SideSel::Stereo; k.mode = Mode::Insert; k.algorithmVersion = 1;
    return k;
}

TEST_CASE("constants match F1 §23")
{
    REQUIRE(kLMax == 65536);
    REQUIRE(kBMax == 2048);
    REQUIRE(kMaxInstances == 20);
    REQUIRE(kMarginBlocks == 2);
}

TEST_CASE("CalibKey equality is field-complete")
{
    REQUIRE(baseKey() == baseKey());
    std::vector<std::function<void(CalibKey&)>> mutations = {
        [](CalibKey& k) { k.identIn = "x"; },        [](CalibKey& k) { k.identOut = "x"; },
        [](CalibKey& k) { k.sendIdx[0] = 1; },       [](CalibKey& k) { k.sendIdx[1] = 1; },
        [](CalibKey& k) { k.sendName[0] = "x"; },    [](CalibKey& k) { k.returnIdx[0] = 1; },
        [](CalibKey& k) { k.returnName[1] = "x"; },  [](CalibKey& k) { k.sampleRate = 44100; },
        [](CalibKey& k) { k.bufferSize = 128; },     [](CalibKey& k) { k.sendStereo = true; },  [](CalibKey& k) { k.returnStereo = true; },
        [](CalibKey& k) { k.send = SideSel::R; },  [](CalibKey& k) { k.ret = SideSel::L; },
        [](CalibKey& k) { k.mode = Mode::Effect; },  [](CalibKey& k) { k.algorithmVersion = 2; },
        [](CalibKey& k) { k.sendName[1] = "x"; },    [](CalibKey& k) { k.returnIdx[1] = 1; },
        [](CalibKey& k) { k.returnName[0] = "x"; },
    };
    for (auto& m : mutations) { CalibKey k = baseKey(); m(k); REQUIRE_FALSE(k == baseKey()); }
}

TEST_CASE("State and Reason enumerators exist with the F1 names")
{
    REQUIRE(static_cast<int>(State::UnsupportedSampleRate) == 9);
    REQUIRE(static_cast<int>(State::Idle) == 0);
    Reason r = Reason::PdcNotObserved; (void) r;
    CalibResult res{}; res.failReason = Reason::None; REQUIRE(res.reportedPdc == 0);
}
