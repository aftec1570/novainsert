#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include "engine/version.hpp"

TEST_CASE("engine version is 1.4.3 and algorithm version is 1")
{
    REQUIRE(ni::engine::kVersion == std::string_view { "1.4.3" });
    REQUIRE_FALSE(ni::engine::kIsPreRelease);   // a production release: no BETA footer
    REQUIRE(ni::engine::kCalibAlgorithmVersion == 1);
}
