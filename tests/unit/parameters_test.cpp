#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>
#include "plugin/parameters.hpp"

using namespace ni::plugin;

TEST_CASE("parameter ids and order are fixed forever")
{
    const std::vector<std::string> ids(kParamIds.begin(), kParamIds.end());
    REQUIRE(ids == std::vector<std::string> { "device", "mode", "sendSel", "returnSel", "stereo", "sendTrim", "returnTrim",
                                              "preRollLeadIn", "reapplyAnticipative", "state", "physLoop", "reportedPdc", "inherentGap",
                                              "link", "calAction", "calStatus" });
    REQUIRE(kParamCount == 16);
    REQUIRE(static_cast<int>(Param::State) == 9);
    REQUIRE(static_cast<int>(Param::InherentGap) == 12);
}

TEST_CASE("read-only status parameters are not automatable")
{
    REQUIRE_FALSE(kParamAutomatable[static_cast<int>(Param::State)]);
    REQUIRE_FALSE(kParamAutomatable[static_cast<int>(Param::PhysLoop)]);
    REQUIRE_FALSE(kParamAutomatable[static_cast<int>(Param::ReportedPdc)]);
    REQUIRE_FALSE(kParamAutomatable[static_cast<int>(Param::InherentGap)]);
    REQUIRE(kParamAutomatable[static_cast<int>(Param::Device)]);
    REQUIRE(kParamAutomatable[static_cast<int>(Param::SendTrim)]);
}

TEST_CASE("parameter ranges match the F1 design")
{
    REQUIRE(kDeviceMax == 999);
    REQUIRE(kTrimMinDb == -24.0f);
    REQUIRE(kTrimMaxDb == 12.0f);
    REQUIRE(kPhysLoopMax == 65536);
    REQUIRE(kStateMax == 9);
}

TEST_CASE("parameters_level_cal_appended")
{
    REQUIRE(static_cast<int>(Param::Link) == 13);
    REQUIRE(static_cast<int>(Param::CalAction) == 14);
    REQUIRE(static_cast<int>(Param::CalStatus) == 15);
    REQUIRE(paramId(Param::Link) == "link");
    REQUIRE(paramId(Param::CalAction) == "calAction");
    REQUIRE(paramId(Param::CalStatus) == "calStatus");
    const std::vector<bool> autom(kParamAutomatable.begin(), kParamAutomatable.end());
    REQUIRE(autom == std::vector<bool> { true, true, true, true, true, true, true, true, true, false, false, false, false,
                                         true, false, false });
    REQUIRE(kCalActionMax == 3);
    REQUIRE(kCalStatusMax == 13);
}
