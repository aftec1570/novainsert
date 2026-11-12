#include <catch2/catch_test_macros.hpp>
#include <string_view>
#include "engine/version.hpp"

TEST_CASE("engine version is 1.2.1 and algorithm version is 1")
{
    REQUIRE(ni::engine::kVersion == std::string_view { "1.2.1" });
    REQUIRE(ni::engine::kCalibAlgorithmVersion == 1);
}
