// GUI skins (user 2026-11-06 "Put Ivory and Silver in the plugin too"; design canvas A Ivory console / B Silver hi-fi): Dark keeps
// the original colours exactly; the light skins keep every text colour readable on the surface it is drawn on (WCAG 4.5:1).
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include "gui/skin.hpp"

using namespace ni::gui;

namespace
{
double lin (std::uint32_t c, int shift)
{
    const double v = ((c >> shift) & 0xFF) / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow ((v + 0.055) / 1.055, 2.4);
}
double lum (std::uint32_t c) { return 0.2126 * lin (c, 16) + 0.7152 * lin (c, 8) + 0.0722 * lin (c, 0); }
double contrast (std::uint32_t a, std::uint32_t b)
{
    const double x = lum (a), y = lum (b);
    return (std::max (x, y) + 0.05) / (std::min (x, y) + 0.05);
}
}

TEST_CASE("skin_dark_keeps_the_original_colours")
{
    const Palette& p = palette (Skin::Dark);
    CHECK (p.cream == 0xFFEFE6CCu);  CHECK (p.label == 0xFFBDB6A4u);  CHECK (p.dim == 0xFF8B8574u);  CHECK (p.amber == 0xFFFFCF6Au);
    CHECK (p.lcdText == 0xFF8DFFB7u); CHECK (p.lcdDim == 0xFF5FBF86u); CHECK (p.warn == 0xFFE0A325u); CHECK (p.error == 0xFFFF6A5Eu);
    CHECK (p.housingTop == 0xFF38393Du); CHECK (p.housingMid == 0xFF1F2023u); CHECK (p.housingBot == 0xFF151618u);
    CHECK (p.stripTop == 0xFF2A2B2Fu); CHECK (p.segOnTop == 0xFFF7ECD0u); CHECK (p.segOffText == 0xFF9A9484u);
    CHECK (p.hwTop == 0xFFF7ECD0u); CHECK (p.calTop == 0xFFE0574Bu); CHECK (p.lcdTop == 0xFF0C1A12u);
    CHECK (p.vuFace[0] == 0xFFFFFBE9u); CHECK (p.vuInk == 0xFF1C1A16u); CHECK (p.bezel[3] == 0xFF3B3D41u);
    CHECK (p.overlayTop == 0xF22A2B2Fu); CHECK (p.lampText == 0xFFCFC8B6u); CHECK (p.popupHiText == 0xFF8DFFB7u);
}

TEST_CASE("skin_light_text_is_readable_on_its_surface")
{
    for (Skin s : { Skin::Ivory, Skin::Silver })
    {
        INFO ("skin " << static_cast<int> (s));
        const Palette& p = palette (s);
        for (std::uint32_t bg : { p.housingTop, p.housingMid, p.housingBot, p.stripTop, p.stripBot })
        {
            CHECK (contrast (p.cream, bg) >= 4.5);
            CHECK (contrast (p.label, bg) >= 4.5);
            CHECK (contrast (p.error, bg) >= 4.5);
            CHECK (contrast (p.warn, bg) >= 4.5);
            CHECK (contrast (p.lampText, bg) >= 4.5);
            CHECK (contrast (p.dim, bg) >= 4.5);
        }
        for (std::uint32_t bg : { p.overlayTop | 0xFF000000u, p.overlayBot | 0xFF000000u })
        {
            CHECK (contrast (p.cream, bg) >= 4.5);
            CHECK (contrast (p.label, bg) >= 4.5);
            CHECK (contrast (p.error, bg) >= 4.5);
        }
        CHECK (contrast (p.segOnText, p.segOnTop) >= 4.5);  CHECK (contrast (p.segOnText, p.segOnBot) >= 4.5);
        CHECK (contrast (p.segOffText, p.segOffTop) >= 4.5); CHECK (contrast (p.segOffText, p.segOffBot) >= 4.5);
        CHECK (contrast (p.hwText, p.hwTop) >= 4.5);  CHECK (contrast (p.hwText, p.hwBot) >= 4.5);
        CHECK (contrast (p.lcdText, p.lcdTop) >= 4.5); CHECK (contrast (p.lcdText, p.lcdBot) >= 4.5);
        CHECK (contrast (p.cream, p.textBoxBg) >= 4.5); CHECK (contrast (p.cream, p.popupBg) >= 4.5);
        CHECK (contrast (p.popupHiText, p.popupHi) >= 4.5);
        CHECK (contrast (p.vuInk, p.vuFace[1]) >= 4.5); CHECK (contrast (p.vuInk, p.vuFace[2]) >= 4.5);
    }
}

TEST_CASE("skin_from_stored_clamps_unknown_values_to_dark")
{
    CHECK (skinFromStored (0) == Skin::Dark);
    CHECK (skinFromStored (1) == Skin::Ivory);
    CHECK (skinFromStored (2) == Skin::Silver);
    CHECK (skinFromStored (3) == Skin::Dark);
    CHECK (skinFromStored (-1) == Skin::Dark);
}
