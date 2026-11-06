#include "gui/setup_panel.hpp"
#include <cmath>
#include "plugin/library.hpp"

namespace ni::gui
{
namespace
{
constexpr int kPresetApollo = 1, kPresetTwin = 2, kPresetCustom = 3;
constexpr int kRef20 = 1, kRef18 = 2, kRefCustom = 3;
bool near (double a, double b) { return std::fabs (a - b) < 0.05; }

void setIfIdle (juce::Slider& s, double v)
{
    if (s.isMouseButtonDown() || s.hasKeyboardFocus (true)) return;
    s.setValue (v, juce::dontSendNotification);
}
} // namespace

void SetupPanel::setupNumber (juce::Slider& s, double lo, double hi, double step, const juce::String& unit) { setupNumberSlider (s, lo, hi, step, unit); }

SetupPanel::SetupPanel()
{
    preset_.addItem (juce::String::fromUTF8 ("Apollo x / +4 \xc2\xb7 24.0"), kPresetApollo);
    preset_.addItem (juce::String::fromUTF8 ("Apollo Twin MkII \xc2\xb7 20.2"), kPresetTwin);
    preset_.addItem ("Custom", kPresetCustom);
    preset_.onChange = [this] {
        if (updating_) return;
        const int id = preset_.getSelectedId();
        if (id == kPresetCustom) return;
        const double v = id == kPresetApollo ? kApolloDbu : kTwinMk2Dbu;
        sendDbu_.setValue (v, juce::dontSendNotification);
        returnDbu_.setValue (v, juce::dontSendNotification);
        emitConverter();
    };
    ref_.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "20 dBFS")), kRef20);
    ref_.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "18 dBFS")), kRef18);
    ref_.addItem ("Custom", kRefCustom);
    ref_.onChange = [this] {
        if (updating_) return;
        const int id = ref_.getSelectedId();
        if (id != kRefCustom) refCustom_.setValue (id == kRef20 ? -20.0 : -18.0, juce::dontSendNotification);
        refCustom_.setEnabled (id == kRefCustom);
        repaint();
        emitConverter();
    };
    for (auto* c : { &preset_, &ref_ })
    {
        c->getProperties().set ("small", true);
        addAndMakeVisible (c);
    }

    setupNumber (sendDbu_, 10.0, 30.0, 0.1, "dBu");
    setupNumber (returnDbu_, 10.0, 30.0, 0.1, "dBu");
    setupNumber (refCustom_, -30.0, -6.0, 0.5, "dBFS");
    setupNumber (outMax_, 0.0, 30.0, 0.5, "dBu");
    for (auto* s : { &sendDbu_, &returnDbu_, &refCustom_, &outMax_ })
    {
        s->onValueChange = [this] { if (! updating_) emitConverter(); };
        addAndMakeVisible (s);
    }
    outMaxOn_.onClick = [this] { outMax_.setEnabled (outMaxOn_.getToggleState()); if (! updating_) emitConverter(); };
    addAndMakeVisible (outMaxOn_);
    measureOnOpen_.onClick = [this] { if (! updating_ && onMeasureOnOpen) onMeasureOnOpen (measureOnOpen_.getToggleState()); };
    addAndMakeVisible (measureOnOpen_);
    skin_.onChange = [this] (int i) { if (! updating_ && onSkin) onSkin (i); };
    addAndMakeVisible (skin_);

    for (auto& o : off_)
    {
        setupNumber (o, ni::plugin::kOffsetMinDb, ni::plugin::kOffsetMaxDb, 0.1, "dB");
        o.setDoubleClickReturnValue (true, 0.0);
        o.onValueChange = [this] { if (! updating_) emitOffsets(); };
        addAndMakeVisible (o);
    }

    apply_.getProperties().set ("ni", "hw");
    apply_.onClick = [this] { if (onApplyDefaults) onApplyDefaults(); };
    addChildComponent (apply_);
    done_.getProperties().set ("ni", "hw");
    done_.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible (done_);
    size_.onChange = [this] (int i) { if (onSize) onSize (kSizes[i]); };
    addAndMakeVisible (size_);

    preset_.setTooltip ("Interface preset: fills in the converter alignment of a known interface, or Custom");
    sendDbu_.setTooltip ("Send: the analog level, in dBu, at 0 dBFS on the output this device uses");
    returnDbu_.setTooltip ("Return: the analog level, in dBu, at 0 dBFS on the input this device uses");
    ref_.setTooltip ("0 VU reference: the dBFS level the RETURN meter and the return CAL treat as 0 VU");
    refCustom_.setTooltip ("Custom reference: -30 to -6 dBFS");
    outMaxOn_.setTooltip ("Show a marker on the SEND scale at the outboard's maximum input level");
    outMax_.setTooltip ("Outboard max input in dBu: where the SEND scale marker sits");
    measureOnOpen_.setTooltip ("Ticked: every instance measures its loop when a project opens (and falls back to the saved latency if that fails). For all instances");
    off_[0].setTooltip ("Send L offset: fine trim in 0.1 dB steps on top of the send trim");
    off_[1].setTooltip ("Send R offset: fine trim in 0.1 dB steps on top of the send trim (stereo send)");
    off_[2].setTooltip ("Return L offset: fine trim in 0.1 dB steps on top of the return trim");
    off_[3].setTooltip ("Return R offset: fine trim in 0.1 dB steps on top of the return trim (stereo return)");
    skin_.setTooltip ("Skin: the colour scheme of every NovaInsert editor");
    size_.setTooltip ("Editor size: fixed steps; drag the lower right corner for any size");
    apply_.setTooltip ("Takes the preset's trims, LINK and offsets into this instance");
    done_.setTooltip ("Closes SETUP; changes are already applied");
}

void SetupPanel::setValues (const ViewInputs& in, int sizePercent)
{
    const juce::ScopedValueSetter<bool> guard (updating_, true);
    setIfIdle (sendDbu_, in.sendDbuAt0dBFS);
    setIfIdle (returnDbu_, in.returnDbuAt0dBFS);
    const int presetId = near (in.sendDbuAt0dBFS, kApolloDbu) && near (in.returnDbuAt0dBFS, kApolloDbu) ? kPresetApollo
                         : near (in.sendDbuAt0dBFS, kTwinMk2Dbu) && near (in.returnDbuAt0dBFS, kTwinMk2Dbu) ? kPresetTwin
                                                                                                               : kPresetCustom;
    if (! preset_.isPopupActive()) preset_.setSelectedId (presetId, juce::dontSendNotification);
    const int refId = near (in.vuRefDbfs, -20.0) ? kRef20 : near (in.vuRefDbfs, -18.0) ? kRef18 : kRefCustom;
    if (! ref_.isPopupActive() && ref_.getSelectedId() != kRefCustom) ref_.setSelectedId (refId, juce::dontSendNotification);
    if (ref_.getSelectedId() == 0) ref_.setSelectedId (refId, juce::dontSendNotification);
    setIfIdle (refCustom_, in.vuRefDbfs);
    refCustom_.setEnabled (ref_.getSelectedId() == kRefCustom);
    outMaxOn_.setToggleState (in.outboardMaxInDbu.has_value(), juce::dontSendNotification);
    if (in.outboardMaxInDbu) setIfIdle (outMax_, *in.outboardMaxInDbu);
    outMax_.setEnabled (in.outboardMaxInDbu.has_value());
    measureOnOpen_.setToggleState (in.measureOnOpen, juce::dontSendNotification);
    skin_.setSelected (static_cast<int> (skinFromStored (in.skin)));
    const float v[4] = { in.sendOffsetDb[0], in.sendOffsetDb[1], in.returnOffsetDb[0], in.returnOffsetDb[1] };
    for (int i = 0; i < 4; ++i) setIfIdle (off_[i], v[i]);
    sendStereo_ = in.sendStereo; retStereo_ = in.retStereo;   // the R offsets exist only for a stereo direction
    off_[1].setEnabled (sendStereo_);
    off_[3].setEnabled (retStereo_);
    if (defaultsChanged_ != in.defaultsChanged) { defaultsChanged_ = in.defaultsChanged; apply_.setVisible (defaultsChanged_); repaint(); }
    int nearest = 0;   // the editor can be any size (corner drag): highlight the closest step
    for (int i = 1; i < 4; ++i)
        if (std::abs (kSizes[i] - sizePercent) < std::abs (kSizes[nearest] - sizePercent)) nearest = i;
    size_.setSelected (nearest);
}

void SetupPanel::emitConverter()
{
    if (! onConverter) return;
    const int refId = ref_.getSelectedId();
    const float vuRef = refId == kRef20 ? -20.0f : refId == kRef18 ? -18.0f : (float) refCustom_.getValue();
    onConverter ({ (float) sendDbu_.getValue(), (float) returnDbu_.getValue(), vuRef,
                   outMaxOn_.getToggleState() ? std::optional<float> ((float) outMax_.getValue()) : std::nullopt });
}

void SetupPanel::emitOffsets()
{
    if (onOffsets)
        onOffsets ({ (float) off_[0].getValue(), (float) off_[1].getValue() }, { (float) off_[2].getValue(), (float) off_[3].getValue() });
}

namespace
{
// two fixed columns: labels at kPad / kRightX, controls at kLeftCtl / kRightCtl
constexpr int kPad = 22, kRow = 34, kTop = 56, kLabelW = 150, kLeftCtl = kPad + kLabelW, kColW = 204, kRightX = 404, kRightCtl = 494,
              kNumW = 150;
}

void SetupPanel::resized()
{
    auto row = [&] (int x, int i, int w) { return juce::Rectangle<int> (x, kTop + i * kRow, w, 26); };
    preset_.setBounds (row (kLeftCtl, 0, kColW));
    sendDbu_.setBounds (row (kLeftCtl, 1, kNumW));
    returnDbu_.setBounds (row (kLeftCtl, 2, kNumW));
    ref_.setBounds (row (kLeftCtl, 3, kColW));
    refCustom_.setBounds (row (kLeftCtl, 4, kNumW));
    outMaxOn_.setBounds (row (kLeftCtl, 5, 66));
    outMax_.setBounds (row (kLeftCtl + 68, 5, kNumW));
    measureOnOpen_.setBounds (row (kLeftCtl, 6, 560));
    for (int i = 0; i < 4; ++i) off_[i].setBounds (row (kRightCtl, i, kNumW));
    skin_.setBounds (row (kRightCtl, 4, skin_.idealWidth()).withHeight (24));
    size_.setBounds (row (kRightCtl, 5, size_.idealWidth()).withHeight (24));
    apply_.setBounds (kPad, getHeight() - 48, 220, 34);
    done_.setBounds (getWidth() - kPad - 90, getHeight() - 48, 90, 34);
}

void SetupPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 18, { 0, 8 }).drawForRectangle (g, r.toNearestInt());
    g.setGradientFill (juce::ColourGradient (juce::Colour (sk().overlayTop), 0.0f, 0.0f, juce::Colour (sk().overlayBot), 0.0f, r.getHeight(), false));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (juce::Colour (sk().overlayEdge));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (ink::cream);
    g.setFont (uiFont (13.0f, true, 0.2f));
    g.drawText ("SETUP", kPad, 16, 200, 20, juce::Justification::left);
    const int rx = kRightX;
    g.setFont (uiFont (10.0f, true, 0.15f));
    g.setColour (ink::label);
    g.drawText ("CONVERTER", kPad, 38, 200, 14, juce::Justification::left);
    g.drawText ("CHANNEL OFFSETS", rx, 38, 200, 14, juce::Justification::left);

    g.setFont (uiFont (12.5f));
    auto label = [&] (int x, int i, const juce::String& t, bool enabled = true) {
        g.setColour (enabled ? ink::cream : ink::dim);
        g.drawText (t, x, kTop + i * kRow, kLabelW - 8, 26, juce::Justification::centredLeft);
    };
    label (kPad, 0, "Interface preset");
    label (kPad, 1, "Send: dBu at 0 dBFS");
    label (kPad, 2, "Return: dBu at 0 dBFS");
    label (kPad, 3, "0 VU reference");
    label (kPad, 4, "Custom reference", ref_.getSelectedId() == kRefCustom);
    label (kPad, 5, "Outboard max input");
    label (kPad, 6, "On project open");
    label (rx, 0, "Send L");
    label (rx, 1, "Send R", sendStereo_);
    label (rx, 2, "Return L");
    label (rx, 3, "Return R", retStereo_);
    label (rx, 4, "Skin");
    label (rx, 5, "Editor size");
    if (defaultsChanged_)
    {
        g.setColour (ink::label);
        g.setFont (uiFont (12.0f));
        g.drawText ("The device library defaults differ from this instance.", kPad + 232, getHeight() - 48, 360, 34,
                    juce::Justification::centredLeft);
    }
}
} // namespace ni::gui
