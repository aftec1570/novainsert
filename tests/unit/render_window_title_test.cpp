#include <catch2/catch_test_macros.hpp>
#include "host/render_window_title.hpp"

TEST_CASE("renderTitleMatches: English and LangPack", "[render_window]")
{
    using ni::host::platform::renderTitleMatches;
    REQUIRE(renderTitleMatches("Render to File", ""));
    REQUIRE(renderTitleMatches("Render to File - project.rpp", ""));
    REQUIRE_FALSE(renderTitleMatches("Render queue", ""));
    REQUIRE(renderTitleMatches("Renderel\xc3\xa9s f\xc3\xa1jlba", "Renderel\xc3\xa9s f\xc3\xa1jlba"));   // Review Focus 5: UTF-8 title
    REQUIRE_FALSE(renderTitleMatches("", "Renderel\xc3\xa9s f\xc3\xa1jlba"));
    REQUIRE(renderTitleMatches("Render to File", "Renderel\xc3\xa9s f\xc3\xa1jlba"));   // English still counts under a LangPack
}
