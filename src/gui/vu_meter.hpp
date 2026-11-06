// One analog VU meter (VU GUI spec 2026-11-02 §4/§5, mockup v5). Vector-drawn: metallic bezel, cream face with a warm glow,
// VU scale −20…+3 with the red 0…+3 arc, a dBFS sub-scale (label = VU + subScaleOffsetDb), "VU", NOVAINSERT, the scale
// label, an ● L ● R legend in stereo, a domed peak LED, one needle (mono, black) or two (L black #141210, R red #b3121a)
// rotating about one pivot far below the face and vanishing under a straight bottom lip; vignette and glass on top.
// Layers: the static face and the glass are cached to images (they only change with setConfig / the LED); the needles
// repaint every frame. Mockup units: face 330 × 190, pivot (165, 285), radius 225, ±33°.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include "gui/vu_needle.hpp"

namespace ni::gui
{
class VuMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kFaceW = 330.0f, kFaceH = 190.0f, kBezel = 9.0f;

    VuMeter();
    // markerVu: an outboard limit drawn as a small amber triangle on the scale (nullopt = none)
    void setConfig (bool stereo, float subScaleOffsetDb, juce::String label, std::optional<float> markerVu);
    void setDrive (float l, float r, bool peak);   // drive relative to 0 VU (needleDrive), peak LED
    void advance (double dt);                      // integrate the needles by the real frame time
    float needlePosition (int ch) const noexcept { return needles_[ch & 1].position(); }
    void settle (float l, float r);                // snapshot/tests: place the needles at rest on these drives

    void paint (juce::Graphics&) override;
    void resized() override;

    static float angleDeg (float position) noexcept;   // −33° … +33° for position 0 … 1, pinned −0.02 … 1.05

private:
    struct Face final : juce::Component { VuMeter* m = nullptr; void paint (juce::Graphics&) override; };
    struct Needles final : juce::Component { VuMeter* m = nullptr; void paint (juce::Graphics&) override; };
    struct Glass final : juce::Component { VuMeter* m = nullptr; void paint (juce::Graphics&) override; };

    Face face_;
    Needles needleLayer_;
    Glass glass_;
    VuNeedle needles_[2];
    float drive_[2] { 0.0f, 0.0f }, drawn_[2] { -1.0f, -1.0f };
    bool stereo_ = false, peak_ = false;
    float offset_ = -20.0f;
    juce::String label_;
    std::optional<float> marker_;
};
} // namespace ni::gui
