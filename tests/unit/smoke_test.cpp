#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include "engine/version.hpp"

TEST_CASE("engine version is 1.4.0-beta.2 and algorithm version is 1")
{
    REQUIRE(ni::engine::kVersion == std::string_view { "1.4.0-beta.2" });
    REQUIRE(ni::engine::kIsPreRelease);   // the editor footer marks the build as BETA
    REQUIRE(ni::engine::kCalibAlgorithmVersion == 1);
}
