// Hardware-style controls for the VU editor (VU GUI spec 2026-11-02 §4, mockup v5). Original vector drawing only: no images,
// no third-party artwork. NovaLookAndFeel draws the 3D rotary trims, the red CAL buttons (property "ni" = "cal"), the round
// amber LINK button ("link"), cream hardware buttons ("hw") and the green LCD device combo (property "detail" = right text).
#pragma once
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/skin.hpp"

namespace ni::gui
{
// text colours of the current skin (setSkin reassigns them; message thread only). Dark values are the defaults.
namespace ink
{
inline juce::Colour cream { 0xFFEFE6CC }, label { 0xFFBDB6A4 }, dim { 0xFF8B8574 }, amber { 0xFFFFCF6A },
    lcdText { 0xFF8DFFB7 }, lcdDim { 0xFF5FBF86 }, red { 0xFFD0262B }, warn { 0xFFE0A325 };
}

// the current skin's palette (skin.hpp). One per process, like the settings.json key it mirrors; message thread only.
const Palette& sk() noexcept;
Skin currentSkin() noexcept;
void setSkin (Skin);   // swaps the palette and the ink colours; callers repaint (NovaEditor::applySkin)

juce::Font uiFont (float size, bool bold = false, float tracking = 0.0f);
juce::Font monoFont (float size);
juce::Font scaleFont (float size, bool bold = false);

void drawHousing (juce::Graphics&, juce::Rectangle<float>);   // dark brushed housing with four screws
void drawStrip (juce::Graphics&, juce::Rectangle<float>);     // raised control strip
void drawDivider (juce::Graphics&, juce::Rectangle<float>);   // recessed slot between the meters
// inc/dec number box with a unit suffix (SETUP and DEVICES pages)
void setupNumberSlider (juce::Slider&, double lo, double hi, double step, const juce::String& unit);

class NovaLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    NovaLookAndFeel();
    void applySkin();   // re-reads the JUCE colour ids from the current skin
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override { return uiFont (14.0f); }
    void drawComboBoxTextWhenNothingSelected (juce::Graphics&, juce::ComboBox&, juce::Label&) override;
};

// Segmented selector (MODE / SEND / RETURN, size steps). Index changes come from clicks only; setSelected never notifies.
class Segment final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit Segment (juce::StringArray items, int cellPad = 20);   // cellPad: px around each label (the 4-way meter view rocker uses less)
    void setSelected (int i) { if (i != sel_) { sel_ = i; repaint(); } }
    int selected() const noexcept { return sel_; }
    std::function<void (int)> onChange;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    int idealWidth() const;

private:
    juce::StringArray items_;
    std::vector<int> cellW_;   // text width + 20 per item, measured once in the ctor (items never change)
    int totalW_ = 1;
    int sel_ = 0;
};

class Led final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setOn (bool on) { if (on != on_) { on_ = on; repaint(); } }
    bool isOn() const noexcept { return on_; }
    void paint (juce::Graphics&) override;

private:
    bool on_ = false;
};

class StatusLamp final : public juce::Component, public juce::SettableTooltipClient
{
public:
    void set (juce::Colour c, juce::String line1, juce::String line2);
    void paint (juce::Graphics&) override;

private:
    juce::Colour colour_ { 0xFF8A8F98 };
    juce::String l1_, l2_;
};
} // namespace ni::gui
