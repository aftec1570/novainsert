#include "gui/devices_panel.hpp"
#include <cmath>

namespace ni::gui
{
namespace
{
constexpr int kPresetApollo = 1, kPresetTwin = 2, kPresetCustom = 3;
constexpr int kRef20 = 1, kRef18 = 2, kRefCustom = 3;
constexpr float kApolloDbu = 24.0f, kTwinMk2Dbu = 20.2f;
constexpr int kPad = 22, kListW = 214, kFormX = 258, kLabelW = 104, kCtlX = kFormX + kLabelW, kRow = 28, kTop = 42, kHalf = 164;
constexpr int kModeW = 118, kChanX = kCtlX + kModeW + 10, kChanW = 100;   // Send / Return rows: [MONO|STEREO] + one or two channel pickers
constexpr int kCapS = 38, kCapR = 46;   // dBu row: caption widths ("send" / "return") left of the two fields
bool near (double a, double b) { return std::fabs (a - b) < 0.05; }
} // namespace

void DevicesPanel::applySkin()
{
    list_.setColour (juce::ListBox::backgroundColourId, juce::Colour (sk().lcdTop));
    list_.setColour (juce::ListBox::outlineColourId, juce::Colour (sk().lcdOutline));
    name_.setColour (juce::TextEditor::backgroundColourId, juce::Colour (sk().lcdTop));
    name_.setColour (juce::TextEditor::textColourId, ink::lcdText);
    name_.setColour (juce::TextEditor::outlineColourId, juce::Colour (sk().lcdOutline));
    name_.setColour (juce::TextEditor::focusedOutlineColourId, ink::lcdDim);
    name_.applyColourToAllText (ink::lcdText);
    list_.repaint();
}

DevicesPanel::DevicesPanel()
{
    list_.setRowHeight (22);
    list_.setOutlineThickness (1);
    addAndMakeVisible (list_);

    for (auto* b : { &newBtn_, &dupBtn_, &delBtn_, &save_, &cancel_, &done_ }) { b->getProperties().set ("ni", "hw"); addAndMakeVisible (b); }
    newBtn_.onClick = [this] { beginNew(); };
    dupBtn_.onClick = [this] {
        auto r = edit_; r.id.clear(); r.name += " copy"; r.version = 0;
        isNew_ = true; loadForm (r); dirty_ = true; list_.deselectAllRows(); repaint();
    };
    delBtn_.onClick = [this] {
        if (isNew_ || edit_.id.empty() || ! onDelete) return;
        const auto err = onDelete (edit_.id);
        setError (err);
        if (err.empty()) { beginNew(); dirty_ = false; }
    };
    save_.onClick = [this] {
        if (! onSave) return;
        readForm();
        const auto err = onSave (edit_);
        setError (err);
        if (err.empty()) { pendingName_ = edit_.name; dirty_ = false; isNew_ = false; }
    };
    cancel_.onClick = [this] {
        const int row = list_.getSelectedRow();
        if (row >= 0 && row < (int) records_.size()) { isNew_ = false; loadForm (records_[(size_t) row]); }
        else beginNew();
        dirty_ = false; setError ({}); repaint();
    };
    done_.onClick = [this] { if (onClose) onClose(); };

    name_.setFont (uiFont (14.0f));
    applySkin();
    name_.setInputRestrictions (40);
    name_.setSelectAllWhenFocused (true);
    name_.onTextChange = [this] { changed(); };
    addAndMakeVisible (name_);

    sendMode_.onChange = [this] (int) { changed(); loadForm (edit_); dirty_ = true; };   // the R picker follows the width
    retMode_.onChange = [this] (int) { changed(); loadForm (edit_); dirty_ = true; };
    mode_.onChange = [this] (int) { changed(); };
    addAndMakeVisible (sendMode_);
    addAndMakeVisible (retMode_);
    addAndMakeVisible (mode_);

    for (auto* c : { &outL_, &outR_, &inL_, &inR_, &preset_, &ref_ })
    {
        c->getProperties().set ("small", true);
        c->setTextWhenNothingSelected (juce::String::fromUTF8 ("choose\xe2\x80\xa6"));
        addAndMakeVisible (c);
    }
    for (auto* c : { &outL_, &outR_, &inL_, &inR_ }) c->onChange = [this] { changed(); };
    preset_.addItem (juce::String::fromUTF8 ("Apollo x / +4 \xc2\xb7 24.0"), kPresetApollo);
    preset_.addItem (juce::String::fromUTF8 ("Apollo Twin MkII \xc2\xb7 20.2"), kPresetTwin);
    preset_.addItem ("Custom", kPresetCustom);
    preset_.onChange = [this] {
        if (loading_) return;
        const int id = preset_.getSelectedId();
        if (id != kPresetCustom)
        {
            const double v = id == kPresetApollo ? kApolloDbu : kTwinMk2Dbu;
            sendDbu_.setValue (v, juce::dontSendNotification); returnDbu_.setValue (v, juce::dontSendNotification);
        }
        changed();
    };
    ref_.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "20 dBFS")), kRef20);
    ref_.addItem (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92" "18 dBFS")), kRef18);
    ref_.addItem ("Custom", kRefCustom);
    ref_.onChange = [this] {
        if (loading_) return;
        const int id = ref_.getSelectedId();
        if (id != kRefCustom) refCustom_.setValue (id == kRef20 ? -20.0 : -18.0, juce::dontSendNotification);
        refCustom_.setEnabled (id == kRefCustom);
        changed();
    };
    setupNumberSlider (sendDbu_, 10.0, 30.0, 0.1, "dBu");
    setupNumberSlider (returnDbu_, 10.0, 30.0, 0.1, "dBu");
    setupNumberSlider (refCustom_, -30.0, -6.0, 0.5, "dBFS");
    setupNumberSlider (outMax_, 0.0, 30.0, 0.5, "dBu");
    for (auto* s : { &sendDbu_, &returnDbu_, &refCustom_, &outMax_ }) { s->onValueChange = [this] { changed(); }; addAndMakeVisible (s); }
    sendDbu_.onValueChange = returnDbu_.onValueChange = [this] {
        if (loading_) return;
        const bool apollo = near (sendDbu_.getValue(), kApolloDbu) && near (returnDbu_.getValue(), kApolloDbu);
        const bool twin = near (sendDbu_.getValue(), kTwinMk2Dbu) && near (returnDbu_.getValue(), kTwinMk2Dbu);
        preset_.setSelectedId (apollo ? kPresetApollo : twin ? kPresetTwin : kPresetCustom, juce::dontSendNotification);
        changed();
    };
    outMaxOn_.onClick = [this] { outMax_.setEnabled (outMaxOn_.getToggleState()); changed(); };
    addAndMakeVisible (outMaxOn_);
    beginNew();
    dirty_ = false;

    list_.setTooltip ("Presets: click one to edit it; the running one is marked with a triangle");
    newBtn_.setTooltip ("NEW: start a new device preset");
    dupBtn_.setTooltip ("DUPLICATE: copy the selected preset under a new name");
    delBtn_.setTooltip ("DELETE: remove the selected preset (not possible while an instance uses it)");
    save_.setTooltip ("SAVE: store the preset; this instance selects it and measures the loop");
    cancel_.setTooltip ("CANCEL: discard the changes to this preset");
    done_.setTooltip ("DONE: close PRESET SETUP");
    name_.setTooltip ("Name: also becomes the FX slot name in REAPER");
    sendMode_.setTooltip ("Send width: MONO uses one output, STEREO a pair");
    retMode_.setTooltip ("Return width: MONO uses one input, STEREO a pair");
    mode_.setTooltip ("Mode: INSERT measures the hardware loop; EFFECT uses the driver latency");
    outL_.setTooltip ("Send output (left or mono)");
    outR_.setTooltip ("Send output, right");
    inL_.setTooltip ("Return input (left or mono)");
    inR_.setTooltip ("Return input, right");
    preset_.setTooltip ("Interface preset: the converter alignment of a known interface, or Custom");
    sendDbu_.setTooltip ("Send: dBu at 0 dBFS on the output");
    returnDbu_.setTooltip ("Return: dBu at 0 dBFS on the input");
    ref_.setTooltip ("0 VU reference for the RETURN meter");
    refCustom_.setTooltip ("Custom reference: -30 to -6 dBFS");
    outMaxOn_.setTooltip ("Show a marker on the SEND scale at the outboard's maximum input level");
    outMax_.setTooltip ("Outboard max input in dBu");
}

void DevicesPanel::beginNew()
{
    plugin::DeviceRecord r;
    r.name = "New device"; r.sendStereo = r.returnStereo = true; r.out[0] = r.out[1] = r.in[0] = r.in[1] = -1;
    isNew_ = true; loadForm (r); dirty_ = true; list_.deselectAllRows(); setError ({}); repaint();
}

void DevicesPanel::fillChannelCombos()
{
    auto fill = [] (juce::ComboBox& c, const std::vector<std::string>& names) {
        const int sel = c.getSelectedId();
        c.clear (juce::dontSendNotification);
        for (size_t i = 0; i < names.size(); ++i) c.addItem (juce::String (ViewModel::channelLabel ((int) i, names[i])), (int) i + 1);
        c.setSelectedId (sel, juce::dontSendNotification);
    };
    fill (outL_, outNames_); fill (outR_, outNames_); fill (inL_, inNames_); fill (inR_, inNames_);
}

void DevicesPanel::loadForm (const plugin::DeviceRecord& r)
{
    const juce::ScopedValueSetter<bool> guard (loading_, true);
    edit_ = r;
    name_.setText (juce::String (r.name), juce::dontSendNotification);
    sendMode_.setSelected (r.sendStereo ? 1 : 0);
    retMode_.setSelected (r.returnStereo ? 1 : 0);
    mode_.setSelected (r.mode == engine::Mode::Effect ? 1 : 0);
    outL_.setSelectedId (r.out[0] + 1, juce::dontSendNotification);
    outR_.setSelectedId (r.sendStereo ? r.out[1] + 1 : 0, juce::dontSendNotification);
    inL_.setSelectedId (r.in[0] + 1, juce::dontSendNotification);
    inR_.setSelectedId (r.returnStereo ? r.in[1] + 1 : 0, juce::dontSendNotification);
    outR_.setVisible (r.sendStereo); inR_.setVisible (r.returnStereo);   // a mono direction shows one picker
    // the pickers name their side in the placeholder (a caption above them would sit under the row above)
    outL_.setTextWhenNothingSelected (juce::String::fromUTF8 (r.sendStereo ? "output L\xe2\x80\xa6" : "output\xe2\x80\xa6"));
    outR_.setTextWhenNothingSelected (juce::String::fromUTF8 ("output R\xe2\x80\xa6"));
    inL_.setTextWhenNothingSelected (juce::String::fromUTF8 (r.returnStereo ? "input L\xe2\x80\xa6" : "input\xe2\x80\xa6"));
    inR_.setTextWhenNothingSelected (juce::String::fromUTF8 ("input R\xe2\x80\xa6"));
    sendDbu_.setValue (r.sendDbuAt0dBFS, juce::dontSendNotification);
    returnDbu_.setValue (r.returnDbuAt0dBFS, juce::dontSendNotification);
    preset_.setSelectedId (near (r.sendDbuAt0dBFS, kApolloDbu) && near (r.returnDbuAt0dBFS, kApolloDbu) ? kPresetApollo
                           : near (r.sendDbuAt0dBFS, kTwinMk2Dbu) && near (r.returnDbuAt0dBFS, kTwinMk2Dbu) ? kPresetTwin : kPresetCustom,
                           juce::dontSendNotification);
    const int refId = near (r.vuRefDbfs, -20.0) ? kRef20 : near (r.vuRefDbfs, -18.0) ? kRef18 : kRefCustom;
    ref_.setSelectedId (refId, juce::dontSendNotification);
    refCustom_.setValue (r.vuRefDbfs, juce::dontSendNotification);
    refCustom_.setEnabled (refId == kRefCustom);
    outMaxOn_.setToggleState (r.outboardMaxInDbu.has_value(), juce::dontSendNotification);
    if (r.outboardMaxInDbu) outMax_.setValue (*r.outboardMaxInDbu, juce::dontSendNotification);
    outMax_.setEnabled (r.outboardMaxInDbu.has_value());
    delBtn_.setEnabled (! isNew_ && ! r.id.empty());
}

void DevicesPanel::readForm()
{
    edit_.name = name_.getText().trim().toStdString();
    edit_.sendStereo = sendMode_.selected() == 1;
    edit_.returnStereo = retMode_.selected() == 1;
    edit_.mode = mode_.selected() == 1 ? engine::Mode::Effect : engine::Mode::Insert;
    edit_.out[0] = outL_.getSelectedId() - 1; edit_.in[0] = inL_.getSelectedId() - 1;
    edit_.out[1] = edit_.sendStereo ? outR_.getSelectedId() - 1 : -1; edit_.in[1] = edit_.returnStereo ? inR_.getSelectedId() - 1 : -1;
    edit_.sendDbuAt0dBFS = (float) sendDbu_.getValue(); edit_.returnDbuAt0dBFS = (float) returnDbu_.getValue();
    const int refId = ref_.getSelectedId();
    edit_.vuRefDbfs = refId == kRef20 ? -20.0f : refId == kRef18 ? -18.0f : (float) refCustom_.getValue();
    edit_.outboardMaxInDbu = outMaxOn_.getToggleState() ? std::optional<float> ((float) outMax_.getValue()) : std::nullopt;
}

void DevicesPanel::setError (const std::string& e) { if (e != error_) { error_ = e; repaint(); } }

void DevicesPanel::setInputs (const ViewInputs& in, int selectedDevice)
{
    running_ = selectedDevice;
    if (in.outChannelNames != outNames_ || in.inChannelNames != inNames_)
    {
        outNames_ = in.outChannelNames; inNames_ = in.inChannelNames;
        fillChannelCombos();
    }
    const bool listChanged = in.records != records_ || in.devices != entries_;
    if (listChanged)
    {
        records_ = in.records; entries_ = in.devices;
        list_.updateContent();
        list_.repaint();
    }
    if (! pendingName_.empty())
    {
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].name == pendingName_)
            {
                pendingName_.clear(); isNew_ = false;
                list_.selectRow ((int) i, juce::dontSendNotification);
                loadForm (records_[i]);
                break;
            }
    }
    else if (listChanged && ! dirty_ && ! isNew_)
    {
        // the record under the form changed elsewhere (another instance, CAL defaults): follow it
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].id == edit_.id) { loadForm (records_[i]); list_.selectRow ((int) i, juce::dontSendNotification); break; }
    }
    if (! dirty_ && isNew_ && list_.getSelectedRow() < 0 && running_ >= 1 && running_ <= (int) records_.size())
    {   // first show: the running preset
        isNew_ = false;
        list_.selectRow (running_ - 1, juce::dontSendNotification);
        loadForm (records_[(size_t) (running_ - 1)]);
    }
}

void DevicesPanel::selectedRowsChanged (int row)
{
    if (row < 0 || row >= (int) records_.size()) return;
    isNew_ = false; dirty_ = false;
    loadForm (records_[(size_t) row]);
    setError ({});
    repaint();
}

void DevicesPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) records_.size()) return;
    const auto& r = records_[(size_t) row];
    const std::string inUse = row < (int) entries_.size() ? entries_[(size_t) row].inUseBy : std::string();
    if (selected) { g.setColour (juce::Colour (sk().listSelected)); g.fillRect (0, 0, w, h); }
    g.setFont (monoFont (13.0f));
    g.setColour (inUse.empty() ? ink::lcdText : ink::lcdDim.withAlpha (0.6f));
    const bool runs = row + 1 == running_;
    g.drawText ((runs ? juce::String::fromUTF8 ("\xe2\x96\xb6 ") : juce::String ("  ")) + juce::String (r.name), 6, 0, w - 12, h,
                juce::Justification::centredLeft, true);
    if (! inUse.empty())
    {
        g.setFont (monoFont (10.0f));
        g.drawText ("in use", 0, 0, w - 8, h, juce::Justification::centredRight, true);
    }
}

void DevicesPanel::resized()
{
    const int h = getHeight();
    list_.setBounds (kPad, kTop, kListW, h - kTop - 58);
    newBtn_.setBounds (kPad, h - 48, 64, 34);
    dupBtn_.setBounds (kPad + 70, h - 48, 92, 34);
    delBtn_.setBounds (kPad + 168, h - 48, 68, 34);
    auto row = [&] (int x, int i, int w) { return juce::Rectangle<int> (x, kTop + i * kRow, w, 24); };
    name_.setBounds (row (kCtlX, 0, 2 * kHalf + 8));
    sendMode_.setBounds (row (kCtlX, 1, kModeW).withHeight (24));
    outL_.setBounds (row (kChanX, 1, kChanW)); outR_.setBounds (row (kChanX + kChanW + 8, 1, kChanW));
    retMode_.setBounds (row (kCtlX, 2, kModeW).withHeight (24));
    inL_.setBounds (row (kChanX, 2, kChanW)); inR_.setBounds (row (kChanX + kChanW + 8, 2, kChanW));
    mode_.setBounds (row (kCtlX, 3, mode_.idealWidth()).withHeight (24));
    preset_.setBounds (row (kCtlX, 4, 2 * kHalf + 8));
    // dBu row: the "send" / "return" captions sit inside the row, left of each field (the 4 pt row gap has no room above)
    sendDbu_.setBounds (row (kCtlX + kCapS, 5, kHalf - kCapS)); returnDbu_.setBounds (row (kCtlX + kHalf + 8 + kCapR, 5, kHalf - kCapR));
    ref_.setBounds (row (kCtlX, 6, kHalf)); refCustom_.setBounds (row (kCtlX + kHalf + 8, 6, kHalf));
    outMaxOn_.setBounds (row (kCtlX, 7, 66)); outMax_.setBounds (row (kCtlX + 68, 7, kHalf));
    const int bw = 86, by = h - 48;
    done_.setBounds (getWidth() - kPad - bw, by, bw, 34);
    cancel_.setBounds (getWidth() - kPad - 2 * bw - 8, by, bw, 34);
    save_.setBounds (getWidth() - kPad - 3 * bw - 16, by, bw, 34);
}

void DevicesPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    juce::DropShadow (juce::Colours::black.withAlpha (0.8f), 18, { 0, 8 }).drawForRectangle (g, r.toNearestInt());
    g.setGradientFill (juce::ColourGradient (juce::Colour (sk().overlayTop), 0.0f, 0.0f, juce::Colour (sk().overlayBot), 0.0f, r.getHeight(), false));
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (juce::Colour (sk().overlayEdge));
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (ink::cream);
    g.setFont (uiFont (13.0f, true, 0.2f));
    g.drawText ("PRESET SETUP", kPad, 16, 200, 20, juce::Justification::left);
    g.setFont (uiFont (10.0f, true, 0.15f));
    g.setColour (ink::label);
    g.drawText ("PRESETS", kPad, 28, 200, 14, juce::Justification::left);
    g.drawText (isNew_ ? "NEW PRESET" : "PRESET", kFormX, 28, 200, 14, juce::Justification::left);
    if (! error_.empty())
    {
        g.setFont (uiFont (12.0f, true));
        g.setColour (juce::Colour (sk().error));
        g.drawText (juce::String (error_), kFormX, 12, getWidth() - kFormX - kPad, 18, juce::Justification::centredRight, true);
    }
    else if (dirty_)
    {
        g.setFont (uiFont (11.0f));
        g.setColour (ink::amber);
        g.drawText (juce::String::fromUTF8 ("unsaved \xe2\x80\x94 SAVE selects and measures the preset"), kFormX, 12, getWidth() - kFormX - kPad, 18, juce::Justification::centredRight, true);
    }
    g.setFont (uiFont (12.5f));
    auto label = [&] (int i, const juce::String& t, bool enabled = true) {
        g.setColour (enabled ? ink::cream : ink::dim);
        g.drawText (t, kFormX, kTop + i * kRow, kLabelW - 6, 24, juce::Justification::centredLeft);
    };
    label (0, "Name");
    label (1, "Send");
    label (2, "Return");
    label (3, "Mode");
    label (4, "Interface");
    label (5, "dBu at 0 dBFS");
    label (6, "0 VU reference");
    label (7, "Outboard max");
    g.setFont (uiFont (10.0f));
    g.setColour (ink::dim);
    g.drawText ("send", kCtlX, kTop + 5 * kRow, kCapS - 6, 24, juce::Justification::centredLeft);
    g.drawText ("return", kCtlX + kHalf + 8, kTop + 5 * kRow, kCapR - 6, 24, juce::Justification::centredLeft);
}
} // namespace ni::gui
