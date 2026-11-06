#include "gui/main_panel.hpp"
#include "engine/level_cal.hpp"
#include <cmath>

namespace ni::gui
{
using juce::Rectangle;

namespace
{
// layout at the base size (mockup v5 proportions; the housing padding is 24)
const Rectangle<int> kHousing { 4, 4, MainPanel::kW - 8, MainPanel::kH - 8 };
constexpr int kX0 = 28, kInnerW = 724;
const Rectangle<int> kTop { kX0, 28, kInnerW, 40 };
constexpr int kMeterY = 82, kMeterW = 343, kMeterH = 205, kDividerW = 18, kGap = 10, kLcdW = 284;   // 284: room for the VU | TUBE | LED | TWIN and dBu | dBFS rockers and the full lamp text
constexpr int kStripY = 303, kStripH = 128, kStripW = 298, kLinkW = 92;
const Rectangle<int> kStripL { kX0, kStripY, kStripW, kStripH }, kStripR { kX0 + kInnerW - kStripW, kStripY, kStripW, kStripH };
const Rectangle<int> kBottom { kX0, 447, kInnerW, 26 };
const Rectangle<int> kText { kX0 + 4, 484, kInnerW - 8, 46 };
constexpr int kPresetW = 130, kSetupW = 136;   // SETUP and RE-CALIBRATION share one column (user 2026-11-06)
constexpr int kSetupH = 28;   // PRESET SETUP closes the selector row; SETUP opens the status row below it (left)

juce::String trimText (double v) { return juce::String (ViewModel::signedText ((float) v, 1)) + " dB"; }

void setupKnob (juce::Slider& s)
{
    s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    s.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    s.setRotaryParameters (juce::degreesToRadians (-135.0f), juce::degreesToRadians (135.0f), true);
    s.setRange (-24.0, 12.0, 0.0);   // continuous: TrimKnob::snapValue does the 0.1 dB steps, then LINK's limit
    s.setDoubleClickReturnValue (true, 0.0);   // TrimKnob::mouseDoubleClick re-aims it through LINK's limit
    s.setMouseDragSensitivity (360);
    s.setVelocityBasedMode (false);
}
} // namespace

MainPanel::MainPanel()
{
    setLookAndFeel (&lnf_);
    setSize (kW, kH);
    setOpaque (true);

    device.getProperties().set ("detail", "");
    device.setTextWhenNothingSelected ("No device");
    device.onChange = [this] { if (onSelectDevice && device.getSelectedId() > 0) onSelectDevice (device.getSelectedId() - 1); };
    addAndMakeVisible (device);
    addAndMakeVisible (lamp_);

    addAndMakeVisible (sendVu_);
    addAndMakeVisible (returnVu_);
    addChildComponent (bars_);
    meterView.onChange = [this] (int i) { setMeterView (i); if (onMeterView) onMeterView (i); };
    addAndMakeVisible (meterView);
    meterUnit.onChange = [this] (int i) { setMeterUnit (i); if (onMeterUnit) onMeterUnit (i); };
    addAndMakeVisible (meterUnit);

    for (auto* k : { &sendTrim, &returnTrim })
    {
        setupKnob (*k);
        k->onValueChange = [this] { repaint (kStripL.getUnion (kStripR)); };
        addAndMakeVisible (k);
    }
    sendTrim.onUser = [this] (double v) { return linkedUserValue (sendTrim, returnTrim, v); };
    returnTrim.onUser = [this] (double v) { return linkedUserValue (returnTrim, sendTrim, v); };
    sendTrim.onLimit = [this] (double v) { return (double) ni::engine::cal::linkLimit ((float) v, (float) sendTrim.getValue(), (float) returnTrim.getValue(), link.getToggleState(), -24.0f, 12.0f); };
    returnTrim.onLimit = [this] (double v) { return (double) ni::engine::cal::linkLimit ((float) v, (float) returnTrim.getValue(), (float) sendTrim.getValue(), link.getToggleState(), -24.0f, 12.0f); };
    sendCal.getProperties().set ("ni", "cal");
    returnCal.getProperties().set ("ni", "cal");
    sendCal.onClick = [this] { if (onCal) onCal (1); };
    returnCal.onClick = [this] { if (onCal) onCal (in_.calStatus == 2 || in_.calStatus == 3 ? 3 : 2); };
    link.getProperties().set ("ni", "link");
    link.setClickingTogglesState (true);
    link.setTooltip ("LINK: couples the trims. More send means the same amount less return, so the mix level stays the same");
    device.setTooltip ("Device: pick the preset whose outputs and inputs this instance owns, or No device");
    lamp_.setTooltip ("Status: the measurement state, the measured LOOP and the PDC reported to REAPER");
    sendVu_.setTooltip ("SEND meter: 0 VU = +4 dBu at the converter output; the dBFS sub-scale follows SETUP");
    returnVu_.setTooltip ("RETURN meter: 0 VU = the dBFS reference set in SETUP");
    bars_.setTooltip ("Level bars: SEND above, RETURN below, with peak hold");
    meterView.setTooltip ("Meter view: VU faces, or the TUBE, LED and TWIN level bars");
    meterUnit.setTooltip ("Meter unit: show the readouts in dBu or in dBFS");
    sendTrim.setTooltip ("SEND TRIM: gain before the converter, -24 to +12 dB. Double-click resets to 0 dB");
    returnTrim.setTooltip ("RETURN TRIM: gain after the converter input, -24 to +12 dB. Double-click resets to 0 dB");
    sendCal.setTooltip ("CAL +4 dBu: play the track, and the send trim is set so it leaves at 0 VU = +4 dBu. Press again to cancel");
    returnCal.setTooltip ("CAL (return): measures the return and sets the return trim to digital unity");
    sendPeak_.setTooltip ("PEAK: the send reached -0.1 dBFS or more");
    returnClip_.setTooltip ("CLIP: a returned sample exceeded 0 dBFS");
    mode.setTooltip ("MODE: INSERT measures the hardware loop; EFFECT uses the driver latency and keeps tails");
    sendSel.setTooltip ("SEND: STEREO uses both outputs; L or R uses one side of a stereo unit as a mono send");
    returnSel.setTooltip ("RETURN: STEREO uses both inputs; L or R uses one side of a stereo unit as a mono return");
    setupButton.setTooltip ("SETUP: converter alignment, 0 VU reference, channel offsets, skin, editor size, measuring on project open");
    devicesButton.setTooltip ("PRESET SETUP: create, edit, duplicate and delete device presets");
    reapply.setTooltip ("Turns Anticipative FX processing off again for the listed tracks");
    setupButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x9a\x99 SETUP")));
    setupButton.getProperties().set ("ni", "hw");
    setupButton.onClick = [this] { showSetup (! setup.isVisible()); };
    recalibrate.getProperties().set ("ni", "hw");
    recalibrate.setTooltip ("RE-CALIBRATION: measures the hardware loop again. Available only after a failed calibration, or when the check on project open failed and the saved latency is in use");
    recalibrate.setEnabled (false);
    recalibrate.onClick = [this] { if (onRecalibrate) onRecalibrate(); };
    devicesButton.getProperties().set ("ni", "hw");
    devicesButton.onClick = [this] { showDevices (! devices.isVisible()); };
    reapply.getProperties().set ("ni", "hw");
    reapply.onClick = [this] { if (onReapply) onReapply(); };
    for (juce::Component* c : { (juce::Component*) &sendCal, (juce::Component*) &returnCal, (juce::Component*) &link,
                                (juce::Component*) &setupButton, (juce::Component*) &recalibrate, (juce::Component*) &devicesButton, (juce::Component*) &mode, (juce::Component*) &sendSel,
                                (juce::Component*) &returnSel, (juce::Component*) &sendPeak_, (juce::Component*) &returnClip_ })
        addAndMakeVisible (c);
    addChildComponent (reapply);

    setup.onClose = [this] { showSetup (false); };
    addChildComponent (setup);
    devices.onClose = [this] { showDevices (false); };
    addChildComponent (devices);
}

MainPanel::~MainPanel() { setLookAndFeel (nullptr); }

void MainPanel::showSetup (bool on)
{
    if (on && devices.isVisible()) showDevices (false);
    setup.setVisible (on);
    if (on) setup.toFront (false);
    setupButton.setToggleState (on, juce::dontSendNotification);
}

void MainPanel::showDevices (bool on)
{
    if (on && setup.isVisible()) showSetup (false);
    devices.setVisible (on);
    if (on) devices.toFront (false);
    devicesButton.setToggleState (on, juce::dontSendNotification);
}

void MainPanel::setMeterView (int view)
{
    const auto v = meterViewFromStored (view);
    const bool bars = isBarView (v);
    if (bars) bars_.setStyle (v);
    sendVu_.setVisible (! bars);
    returnVu_.setVisible (! bars);
    bars_.setVisible (bars);
    meterView.setSelected ((int) v);
    repaint (Rectangle<int> (kX0, kMeterY, kInnerW, kMeterH));   // the divider between the VU faces goes with them
}

void MainPanel::setMeterUnit (int unit)
{
    meterUnit.setSelected (unit == 1 ? 1 : 0);
    const ViewInputs copy = in_;
    update (copy, device.getSelectedId() - 1, footer_.toStdString());   // scales, labels and readouts follow at once
}

void MainPanel::resized()
{
    // top bar: LCD, the VU | BARS and dBu | dBFS rockers, the status lamp (meter view design 2026-11-05)
    const int rockerW = meterView.idealWidth() + 10, unitW = meterUnit.idealWidth() + 10;
    device.setBounds (kTop.withWidth (kLcdW));
    meterView.setBounds (kX0 + kLcdW + 10, kTop.getY() + 4, rockerW, kTop.getHeight() - 8);
    meterUnit.setBounds (kX0 + kLcdW + 10 + rockerW + 8, kTop.getY() + 4, unitW, kTop.getHeight() - 8);
    lamp_.setBounds (kTop.withTrimmedLeft (kLcdW + 10 + rockerW + 8 + unitW + 10));
    sendVu_.setBounds (kX0, kMeterY, kMeterW, kMeterH);
    returnVu_.setBounds (kX0 + kInnerW - kMeterW, kMeterY, kMeterW, kMeterH);
    bars_.setBounds (kX0, kMeterY, kInnerW, kMeterH);

    const int y0 = kStripY;
    sendTrim.setBounds (kStripL.getX() + 75 - 41, y0 + 8, 82, 82);
    sendCal.setBounds (kStripL.getX() + 210 - 60, y0 + 16, 120, 38);
    sendPeak_.setBounds (kStripL.getX() + 210 + 20, y0 + 94, 12, 12);
    returnCal.setBounds (kStripR.getX() + 88 - 60, y0 + 16, 120, 38);
    returnClip_.setBounds (kStripR.getX() + 88 + 18, y0 + 94, 12, 12);
    returnTrim.setBounds (kStripR.getX() + 223 - 41, y0 + 8, 82, 82);
    const int linkCx = kStripL.getRight() + 18 + kLinkW / 2;
    link.setBounds (linkCx - 38, y0 + 14, 76, 76);

    auto b = kBottom;
    b.removeFromLeft (44);
    mode.setBounds (b.removeFromLeft (mode.idealWidth()));
    b.removeFromLeft (58);
    sendSel.setBounds (b.removeFromLeft (sendSel.idealWidth()));   // shown only for a STEREO send (a mono send has nothing to pick)
    b.removeFromLeft (62);
    returnSel.setBounds (b.removeFromLeft (returnSel.idealWidth()));
    // user 2026-11-06: PRESET SETUP (the former DEVICES) closes the selector row on the right; SETUP sits one row lower, on the
    // left, and the status lines run to its right
    devicesButton.setBounds (kBottom.getRight() - kPresetW, kBottom.getY() - 3, kPresetW, 32);
    setupButton.setBounds (kX0, kText.getY(), kSetupW, kSetupH);
    recalibrate.setBounds (kX0, kText.getY() + kSetupH + 4, kSetupW, kSetupH);   // user 2026-11-06: under SETUP
    jassert (returnSel.getRight() + 8 <= devicesButton.getX());   // user 2026-11-06: RETURN's R cell sat under DEVICES (cell pad 20 + 10 overflowed)
    reapply.setBounds (kText.getRight() - 230, kText.getY(), 230, 28);

    setup.setBounds (kX0 - 6, kMeterY - 6, kInnerW + 12, kStripY + kStripH - kMeterY + 12);
    devices.setBounds (setup.getBounds());
}

juce::String MainPanel::readout (const Reading& r, bool send) const
{
    const float dbuAt0 = send ? in_.sendDbuAt0dBFS : in_.returnDbuAt0dBFS;
    if (! (send ? in_.sendStereo : in_.retStereo)) return juce::String (ViewModel::levelText (r.rmsDb[0], in_.meterDbu, dbuAt0));
    auto num = [&] (int c) {
        if (r.rmsDb[c] <= ViewModel::kFloorDb) return std::string ("—");
        return ViewModel::signedText (in_.meterDbu ? r.rmsDb[c] + dbuAt0 : r.rmsDb[c], 1);
    };
    return juce::String ("L " + num (0) + "  R " + num (1) + (in_.meterDbu ? " dBu" : " dBFS"));
}

void MainPanel::update (const ViewInputs& source, int selectedDevice, const std::string& footer)
{
    const bool dbu = meterUnit.selected() == 0;   // the meter unit is the panel's own preference, not a processor input
    in_ = source;
    in_.meterDbu = dbu;
    const ViewInputs& in = in_;
    vm_ = ViewModel::from (in);
    footer_ = juce::String (footer);

    if (vm_.deviceItems != items_ && ! device.isPopupActive())
    {
        items_ = vm_.deviceItems;
        device.clear (juce::dontSendNotification);
        device.addItem ("No device", 1);
        for (size_t i = 0; i < items_.size(); ++i)
        {
            device.addItem (juce::String (items_[i].first), (int) i + 2);
            device.setItemEnabled ((int) i + 2, items_[i].second);
        }
    }
    if (device.getSelectedId() != selectedDevice + 1) device.setSelectedId (selectedDevice + 1, juce::dontSendNotification);
    const juce::String detail = in.deviceName.empty() ? juce::String() : juce::String (in.outName + " · " + in.inName);
    if (device.getProperties()["detail"].toString() != detail)
    {
        device.getProperties().set ("detail", detail);
        device.resized();
        device.repaint();
    }

    const auto pdc = in.reportedPdc >= 0 ? std::to_string (in.reportedPdc) + " smp" : std::string ("—");
    const auto loop = in.physLoop >= 0 ? std::to_string (in.physLoop) + " smp" : std::string ("—");
    lamp_.set (juce::Colour (vm_.stateColour), juce::String (vm_.stateText).toUpperCase(),
               juce::String ("LOOP " + loop + " · PDC " + pdc));

    const auto marker = in.outboardMaxInDbu ? std::optional<float> (*in.outboardMaxInDbu - 4.0f) : std::nullopt;
    sendVu_.setConfig (in.sendStereo, vm_.sendSubScaleOffset, juce::String (vm_.sendScaleLabel), marker);
    returnVu_.setConfig (in.retStereo, vm_.returnSubScaleOffset, juce::String (vm_.returnScaleLabel), std::nullopt);
    bars_.setConfig (in.sendStereo, in.retStereo, in.sendDbuAt0dBFS, in.returnDbuAt0dBFS, in.vuRefDbfs, in.outboardMaxInDbu, in.meterDbu);
    if (sendSel.isVisible() != in.recSendStereo || returnSel.isVisible() != in.recReturnStereo)
    {
        sendSel.setVisible (in.recSendStereo);
        returnSel.setVisible (in.recReturnStereo);
        repaint();   // the SEND / RETURN captions follow
    }

    sendCal.setToggleState (in.calStatus == 1, juce::dontSendNotification);
    returnCal.setToggleState (in.calStatus == 2 || in.calStatus == 3, juce::dontSendNotification);
    reapply.setVisible (vm_.showReapplyAnticipative);
    recalibrate.setEnabled (vm_.recalibrateEnabled);
    setup.setValues (in, sizePercent_);
    devices.setInputs (in, selectedDevice);

    sendReadout_ = readout (send_, true);
    returnReadout_ = readout (ret_, false);
    // paint() draws the housing, strips and status text from these inputs only: repaint on a change, not on every 8 Hz tick
    const juce::String key = sendReadout_ + "|" + returnReadout_ + "|" + footer_ + "|" + juce::String (vm_.calText) + "|"
                             + juce::String (vm_.failureReason) + "|" + juce::String (vm_.noteText) + "|" + juce::String (vm_.progressText) + "|"
                             + juce::String (vm_.showLeadInHint ? 1 : 0) + juce::String (vm_.calError ? 1 : 0) + juce::String (link.getToggleState() ? 1 : 0) + "|"
                             + juce::String (vm_.warnings.size()) + (vm_.warnings.empty() ? juce::String() : juce::String (vm_.warnings.front()));
    if (key != paintKey_) { paintKey_ = key; repaint(); }
}

void MainPanel::setMeters (const Reading& send, const Reading& ret)
{
    send_ = send;
    ret_ = ret;
    const bool ss = in_.sendStereo, rs = in_.retStereo;
    auto drive = [] (float rect, float zero) { return needleDrive (rect, zero); };
    sendVu_.setDrive (drive (send.rect[0], vm_.sendSubScaleOffset), drive (send.rect[1], vm_.sendSubScaleOffset),
                      send.nearFullScale[0] || (ss && send.nearFullScale[1]));
    returnVu_.setDrive (drive (ret.rect[0], vm_.returnSubScaleOffset), drive (ret.rect[1], vm_.returnSubScaleOffset),
                        ret.nearFullScale[0] || (rs && ret.nearFullScale[1]));
    bars_.setReadings (send, ret);
    sendPeak_.setOn (send.nearFullScale[0] || (ss && send.nearFullScale[1]));
    returnClip_.setOn (ret.clip[0] || (rs && ret.clip[1]));
}

void MainPanel::settleNeedles()
{
    auto d = [] (float rect, float zero) { return needleDrive (rect, zero); };
    sendVu_.settle (d (send_.rect[0], vm_.sendSubScaleOffset), d (send_.rect[1], vm_.sendSubScaleOffset));
    returnVu_.settle (d (ret_.rect[0], vm_.returnSubScaleOffset), d (ret_.rect[1], vm_.returnSubScaleOffset));
    bars_.settle();
}

void MainPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (sk().panelBg));
    drawHousing (g, kHousing.toFloat());
    if (! bars_.isVisible() || bars_.style() == MeterView::Twin) drawDivider (g, Rectangle<float> ((float) (kX0 + kMeterW + kGap), (float) kMeterY, (float) kDividerW, (float) kMeterH));
    drawStrip (g, kStripL.toFloat());
    drawStrip (g, kStripR.toFloat());

    auto text = [&] (const juce::String& s, Rectangle<int> r, juce::Font f, juce::Colour c, juce::Justification j = juce::Justification::centred) {
        g.setFont (f);
        g.setColour (c);
        g.drawText (s, r, j, true);
    };
    const auto lbl = uiFont (10.0f, true, 0.2f), val = uiFont (13.0f, true);
    const int y0 = kStripY;
    // SEND strip
    text ("SEND TRIM", { kStripL.getX() + 15, y0 + 92, 120, 12 }, lbl, ink::label);
    text (trimText (sendTrim.getValue()), { kStripL.getX() + 15, y0 + 106, 120, 16 }, val, ink::cream);
    text (sendReadout_, { kStripL.getX() + 140, y0 + 64, 150, 16 }, val, ink::cream);
    text ("PEAK", { kStripL.getX() + 210 - 30, y0 + 92, 46, 16 }, lbl, ink::label, juce::Justification::centredRight);
    // RETURN strip
    text (returnReadout_, { kStripR.getX() + 6, y0 + 64, 164, 16 }, val, ink::cream);
    text ("CLIP", { kStripR.getX() + 88 - 30, y0 + 92, 44, 16 }, lbl, ink::label, juce::Justification::centredRight);
    text ("RETURN TRIM", { kStripR.getX() + 163, y0 + 92, 120, 12 }, lbl, ink::label);
    text (trimText (returnTrim.getValue()), { kStripR.getX() + 163, y0 + 106, 120, 16 }, val, ink::cream);
    // LINK
    text ("LINK", { kStripL.getRight() + 18, y0 + 96, kLinkW, 12 }, lbl, link.getToggleState() ? ink::amber : ink::label);

    // bottom row captions
    const auto cap = uiFont (10.0f, true, 0.1f);
    text ("MODE", { kBottom.getX(), kBottom.getY(), 42, kBottom.getHeight() }, cap, ink::label, juce::Justification::centredLeft);
    if (sendSel.isVisible()) text ("SEND", { sendSel.getX() - 58, kBottom.getY(), 54, kBottom.getHeight() }, cap, ink::label, juce::Justification::centredRight);
    if (returnSel.isVisible()) text ("RETURN", { returnSel.getX() - 62, kBottom.getY(), 58, kBottom.getHeight() }, cap, ink::label, juce::Justification::centredRight);

    // status lines: progress (why the track is silent) first, then CAL, failure, warnings, neutral notes, hint
    std::vector<std::pair<juce::String, juce::Colour>> lines;
    if (! vm_.progressText.empty()) lines.emplace_back (juce::String (vm_.progressText), ink::cream);
    if (! vm_.calText.empty()) lines.emplace_back (juce::String (vm_.calText), vm_.calError ? juce::Colour (sk().error) : ink::cream);
    if (! vm_.failureReason.empty()) lines.emplace_back (juce::String (vm_.failureReason), juce::Colour (sk().error));
    for (const auto& w : vm_.warnings) lines.emplace_back (juce::String (w), ink::warn);
    if (! vm_.noteText.empty()) lines.emplace_back (juce::String (vm_.noteText + " — see SETUP"), ink::label);
    if (vm_.showLeadInHint) lines.emplace_back ("Tip: enable the pre-roll lead-in so renders start with the loop already filled", ink::dim);
    const int textX = setupButton.getRight() + 12;   // SETUP holds the row's left end
    auto area = kText.withLeft (textX);
    const int textW = (reapply.isVisible() ? reapply.getX() - 12 : kText.getRight()) - textX;   // the re-apply button sits at the right end
    for (size_t i = 0; i < lines.size() && i < 3; ++i)
        text (lines[i].first, area.removeFromTop (15).withWidth (textW), uiFont (12.0f, i == 0 && (! vm_.progressText.empty() || ! vm_.calText.empty())), lines[i].second,
              juce::Justification::centredLeft);
    text (footer_, kText.withTrimmedTop (32), uiFont (10.5f), ink::dim.withAlpha (0.8f), juce::Justification::bottomRight);
}

double MainPanel::linkedUserValue (TrimKnob& self, TrimKnob& other, double attempted)
{
    const bool on = link.getToggleState();
    const float now = (float) self.getValue(), o = (float) other.getValue();
    const float v = ni::engine::cal::linkLimit ((float) attempted, now, o, on, -24.0f, 12.0f);
    if (on && ni::engine::cal::moved (v, now))
        other.setValue ((double) (o - (v - now)), juce::sendNotificationSync);   // the other first: both land in this one call
    return (double) v;
}

void MainPanel::applySkin (Skin s)
{
    setSkin (s);
    lnf_.applySkin();
    devices.applySkin();
    sendLookAndFeelChange();
    std::function<void (juce::Component&)> all = [&all] (juce::Component& c) {
        c.repaint();   // a buffered child (the meter faces, the VU glass) only re-renders its cached image on its own repaint
        for (auto* k : c.getChildren()) all (*k);
    };
    all (*this);
}
} // namespace ni::gui
