#pragma once
// GUI skins (user 2026-11-06 "Put Ivory and Silver in the plugin too"; design canvas A Ivory console / B Silver hi-fi). One global
// choice, persisted in settings.json "skin" (0 Dark / 1 Ivory / 2 Silver). Dark holds the original colours exactly, so the default
// look is unchanged. Plain ARGB values: no JUCE here, so the palettes are unit-tested (skin_test).
#include <cstdint>

namespace ni::gui
{
enum class Skin { Dark = 0, Ivory = 1, Silver = 2 };

constexpr Skin skinFromStored (int v) noexcept { return v == 1 ? Skin::Ivory : v == 2 ? Skin::Silver : Skin::Dark; }

struct Palette
{
    using C = std::uint32_t;
    // text
    C cream, label, dim, amber, lcdText, lcdDim, red, warn, error, lampText;
    // housing, screws, control strips, the slot between the meters
    C housingTop, housingMid, housingBot, housingEdgeHi, housingEdgeLo, editorBase, panelBg;
    C screwHi, screwMid, screwLo, screwSlot;
    C stripTop, stripBot, stripEdge;
    C dividerEdge, dividerMid;
    // popups, number boxes, plain buttons
    C popupBg, popupHi, textBoxBg, textBoxOutline, buttonBase;
    // knobs and LINK
    C knobSkirtA, knobSkirtB, knobCapHi, knobCapMid, knobCapLo, knobPointer;
    C linkHi, linkLo, linkOff;
    // hardware buttons (SETUP, PRESET SETUP, …) and the CAL buttons
    C hwTop, hwBot, hwBase, hwHighlight, hwText;
    C calTop, calBot, calBase, calHighlight, calText;
    // device LCD (combo, preset list, name box)
    C lcdTop, lcdBot, lcdOutline, listSelected;
    // segmented selectors
    C segOnTop, segOnBot, segOnText, segOffTop, segOffBot, segOffText;
    // SETUP / PRESET SETUP overlay page
    C overlayTop, overlayBot, overlayEdge;
    // meter bezels (gradient stops 0, .18, .5, .85, 1), VU face (centre → rim), VU inks, the dark lip under the face
    C bezel[5], bezelHi, bezelLo;
    C vuFace[4], vuInk, vuRed, needleL, needleR, vuMarker, vuInk2, vuInk3;
    C vuLip[3];
    // BARS meter face (the LED smoked-glass view and the LCD read-outs stay dark in every skin)
    C barFace[3], barInk, barDim, barFs, barRef, barZero, barOutboard, barChan, barTitle, barCaption, barBrand, barLip[2];
    C popupHiText;   // text of the highlighted popup-menu row (on popupHi)
};

inline const Palette& palette (Skin s) noexcept
{
    static const Palette dark {
        0xFFEFE6CC, 0xFFBDB6A4, 0xFF8B8574, 0xFFFFCF6A, 0xFF8DFFB7, 0xFF5FBF86, 0xFFD0262B, 0xFFE0A325, 0xFFFF6A5E, 0xFFCFC8B6,
        0xFF38393D, 0xFF1F2023, 0xFF151618, 0xFF5A5C61, 0xFF08090A, 0xFF151618, 0xFF17181A,
        0xFFD9DADC, 0xFF7D7F84, 0xFF2B2C2F, 0xFF222222,
        0xFF2A2B2F, 0xFF18191B, 0xFF4B4D52,
        0xFF0D0E10, 0xFF33353A,
        0xFF1B1C1F, 0xFF3A3C41, 0xFF101113, 0xFF3A3C41, 0xFF2F3034,
        0xFF25272A, 0xFF6B6E74, 0xFF6C6F75, 0xFF2C2E32, 0xFF141517, 0xFFF2E3B0,
        0xFF4D5056, 0xFF1C1D20, 0xFF6A6A6A,
        0xFFF7ECD0, 0xFFD6C49A, 0xFF7D6E4C, 0xFFFFFFFF, 0xFF2A2416,
        0xFFE0574B, 0xFFA4241A, 0xFF5D130C, 0xFFFF9A8E, 0xFFFFF2E6,
        0xFF0C1A12, 0xFF06100A, 0xFF4A4C50, 0xFF1E3A28,
        0xFFF7ECD0, 0xFFCDBB90, 0xFF2A2416, 0xFF2F3034, 0xFF1B1C1F, 0xFF9A9484,
        0xF22A2B2F, 0xF2141517, 0xFF4B4D52,
        { 0xFF8D9096, 0xFF46484D, 0xFF1C1D20, 0xFF3B3D41, 0xFF6F7277 }, 0xFFB9BCC2, 0xFF0A0A0A,
        { 0xFFFFFBE9, 0xFFF6EBC8, 0xFFE2D1A2, 0xFFB9A77A }, 0xFF1C1A16, 0xFFD0262B, 0xFF141210, 0xFFB3121A, 0xFFC8781E, 0xFF3A3328, 0xFF5A5040,
        { 0xFF5A5C61, 0xFF2A2B2E, 0xFF101113 },
        { 0xFFF5EDD5, 0xFFEBE0BF, 0xFFD6C79E }, 0xFF2A2620, 0xFF6B6352, 0xFF4A4336, 0xFFC8322A, 0xFFC8322A, 0xFFA8541A, 0xFF4A4336,
        0xFF4A4438, 0xFF4A4336, 0xFF6F6656, { 0xFF1C1B19, 0xFF0D0D0C },
        0xFF8DFFB7 };

    // A · Ivory console: warm ivory panel, dark knobs, LCD and buttons, the cream VU face kept
    static const Palette ivory {
        0xFF26221C, 0xFF4A4338, 0xFF574F42, 0xFF6A3C00, 0xFF8DFFB7, 0xFF5FBF86, 0xFFC0302A, 0xFF6A3C00, 0xFF8A1A14, 0xFF3D372D,
        0xFFF1EBDE, 0xFFE6DECE, 0xFFD8CFBC, 0xFFFFFAF0, 0xFFB9B09C, 0xFFD8CFBC, 0xFFE6DECE,
        0xFFF4EFE4, 0xFFCFC8B8, 0xFFB5AD9C, 0xFF6E6656,
        0xFFE9E2D3, 0xFFDED5C2, 0xFFC9C0AC,
        0xFFA89F8B, 0xFFD6CEBB,
        0xFFF6F1E6, 0xFFDDD4C1, 0xFFFBF8F0, 0xFFA89F8B, 0xFF2F2B25,
        0xFF25272A, 0xFF6B6E74, 0xFF6C6F75, 0xFF2C2E32, 0xFF141517, 0xFFF2E3B0,
        0xFFF6F1E6, 0xFFD6CDB9, 0xFF5A5245,
        0xFF2F2B25, 0xFF1E1B17, 0xFF0E0D0B, 0xFF6A6458, 0xFFF3ECD9,
        0xFFD6463B, 0xFFA92A22, 0xFF7D1E18, 0xFFF28A80, 0xFFFFF2E6,
        0xFF0C1A12, 0xFF06100A, 0xFF8F8775, 0xFF1E3A28,
        0xFF3A352C, 0xFF221F1A, 0xFFF3ECD9, 0xFFECE5D6, 0xFFD9D0BD, 0xFF5A5245,
        0xF2EFE9DC, 0xF2DDD4C1, 0xFFA89F8B,
        { 0xFFD9D1BF, 0xFFC2B9A6, 0xFFA59C89, 0xFFB3AA96, 0xFF8F8775 }, 0xFFFFFAF0, 0xFF6B6455,
        { 0xFFFFFBE9, 0xFFF6EBC8, 0xFFE2D1A2, 0xFFB9A77A }, 0xFF1C1A16, 0xFFD0262B, 0xFF141210, 0xFFB3121A, 0xFFC8781E, 0xFF3A3328, 0xFF5A5040,
        { 0xFF6A6458, 0xFF3A352C, 0xFF22201B },
        { 0xFFF5EDD5, 0xFFEBE0BF, 0xFFD6C79E }, 0xFF2A2620, 0xFF6B6352, 0xFF4A4336, 0xFFC8322A, 0xFFC8322A, 0xFFA8541A, 0xFF4A4336,
        0xFF4A4438, 0xFF4A4336, 0xFF6F6656, { 0xFF3A352C, 0xFF22201B },
        0xFF26221C };

    // B · Silver hi-fi: brushed silver panel, machined knobs, amber LCD and backlit amber meter faces, black buttons, orange CAL
    static const Palette silver {
        0xFF15171A, 0xFF3D4249, 0xFF454A52, 0xFF6A3C00, 0xFFFFB547, 0xFFD99A3D, 0xFFB0261C, 0xFF6A3C00, 0xFF8A1A14, 0xFF2A2E33,
        0xFFE3E5E8, 0xFFCDD0D5, 0xFFB8BCC2, 0xFFFFFFFF, 0xFF8F949B, 0xFFB8BCC2, 0xFFCDD0D5,
        0xFFFFFFFF, 0xFFC9CDD2, 0xFF9AA0A8, 0xFF555B63,
        0xFFD4D7DB, 0xFFC3C7CC, 0xFF9EA3AA,
        0xFF868B92, 0xFFE9EBEE,
        0xFFEEF0F2, 0xFFC9CDD3, 0xFFF7F8F9, 0xFF868B92, 0xFF2C3036,
        0xFF8A9098, 0xFFF5F6F8, 0xFFF3F4F6, 0xFFB2B7BE, 0xFF8F959D, 0xFF15171A,
        0xFFF5F6F8, 0xFF9AA0A8, 0xFF15171A,
        0xFF2C3036, 0xFF1B1E22, 0xFF0A0B0D, 0xFF5A5F66, 0xFFE9ECEF,
        0xFFEF7A2C, 0xFFC9571A, 0xFF8E3C10, 0xFFFFB078, 0xFFFFF6EE,
        0xFF101317, 0xFF1A1F25, 0xFF868B92, 0xFF3A2C14,
        0xFFFFBF5C, 0xFFF0A43A, 0xFF1A1206, 0xFF2C3036, 0xFF1B1E22, 0xFFC9CED5,
        0xF2E6E8EB, 0xF2C9CDD2, 0xFF868B92,
        { 0xFFF4F5F7, 0xFFC8CCD1, 0xFF9EA3AA, 0xFFB6BAC0, 0xFFC8CCD1 }, 0xFFFFFFFF, 0xFF6B7077,
        { 0xFFFFF1CF, 0xFFFFE3A6, 0xFFF6C66B, 0xFFE3A548 }, 0xFF2A1C06, 0xFFB0261C, 0xFF1A1206, 0xFFB0261C, 0xFF8A3A0A, 0xFF3A2808, 0xFF4A3410,
        { 0xFF5A5E64, 0xFF2A2D32, 0xFF15171A },
        { 0xFFFFE3A6, 0xFFF6C66B, 0xFFE3A548 }, 0xFF2A1C06, 0xFF5A3F12, 0xFF4A3410, 0xFFB0261C, 0xFFB0261C, 0xFF8A3A0A, 0xFF4A3410,
        0xFF3A2808, 0xFF4A3410, 0xFF5A3F12, { 0xFF2A2D32, 0xFF15171A },
        0xFF15171A };

    return s == Skin::Ivory ? ivory : s == Skin::Silver ? silver : dark;
}
} // namespace ni::gui
