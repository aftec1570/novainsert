#include "gui/editor.hpp"
#include "engine/version.hpp"
#include "plugin/parameters.hpp"
#include "plugin/processor.hpp"
#include <algorithm>

namespace ni::gui
{
namespace
{
int gSizePercent = 100;   // editor size step: a UI preference shared by all instances of this process (not saved in the project)
constexpr double kSlowPeriod = 0.125;   // texts / readouts at 8 Hz
}

NovaInsertEditor::NovaInsertEditor (NovaInsertProcessor& p) : juce::AudioProcessorEditor (p), proc_ (p)
{
    using P = ni::plugin::Param;
    addAndMakeVisible (panel_);
    sendTrimAtt_ = std::make_unique<juce::SliderParameterAttachment> (param (P::SendTrim), panel_.sendTrim);
    returnTrimAtt_ = std::make_unique<juce::SliderParameterAttachment> (param (P::ReturnTrim), panel_.returnTrim);
    linkAtt_ = std::make_unique<juce::ButtonParameterAttachment> (param (P::Link), panel_.link);
    // LINK: a drag (or wheel / double-click) of one trim moves the other in the same call (MainPanel::linkedUserValue), so the
    // partner parameter gets its own gesture for the drag's duration — REAPER's touch/latch automation sees both moves.
    for (auto [self, partner] : { std::pair { &panel_.sendTrim, &param (P::ReturnTrim) }, std::pair { &panel_.returnTrim, &param (P::SendTrim) } })
    {
        auto held = std::make_shared<bool> (false);
        self->onDragStart = [this, partner, held] { if (panel_.link.getToggleState()) { partner->beginChangeGesture(); *held = true; } };
        self->onDragEnd = [partner, held] { if (*held) { partner->endChangeGesture(); *held = false; } };
    }
    modeAtt_ = attachSegment (panel_.mode, P::Mode);
    sendSelAtt_ = attachSegment (panel_.sendSel, P::SendSel);
    returnSelAtt_ = attachSegment (panel_.returnSel, P::ReturnSel);

    panel_.onSelectDevice = [this] (int d) { proc_.selectDeviceFromGui (d); };
    panel_.onCal = [this] (int a) { proc_.pressCal (a); };
    panel_.onReapply = [this] { proc_.reapplyAnticipativeFromGui(); };
    panel_.onRecalibrate = [this] { proc_.recalibrateFromGui(); };
    panel_.setup.onConverter = [this] (const SetupPanel::Converter& c) { proc_.setConverterCal (c.sendDbu, c.returnDbu, c.vuRef, c.outboardMax); };
    panel_.setup.onOffsets = [this] (std::array<float, 2> s, std::array<float, 2> r) { proc_.setOffsets (s, r); };
    panel_.setup.onApplyDefaults = [this] { proc_.applyDeviceDefaults(); };
    panel_.setup.onSize = [this] (int pc) { setSizePercent (pc); };
    panel_.setup.onMeasureOnOpen = [this] (bool on) { proc_.setMeasureOnOpen (on); };
    panel_.setup.onReverseWheel = [this] (bool on) { proc_.setReverseWheel (on); };
    panel_.setup.onSkin = [this] (int s) { proc_.setSkin (s); refreshSlow(); };
    panel_.devices.onSave = [this] (const ni::plugin::DeviceRecord& r) { return proc_.saveDevice (r); };
    panel_.devices.onDelete = [this] (const std::string& id) { return proc_.deleteDevice (id); };
    proc_.pollLibrary();
    // Meter view (VU/TUBE/LED/TWIN) and unit (dBu/dBFS): global settings.json values, read after the poll so another
    // instance's choice is current; kept across REAPER restarts (forum report 2026-11-12: LED came back as VU).
    panel_.setMeterView (proc_.meterView());
    panel_.onMeterView = [this] (int v) { proc_.setMeterView (v); };
    panel_.setMeterUnit (proc_.meterUnit());
    panel_.onMeterUnit = [this] (int u) { proc_.setMeterUnit (u); };

    // Free resize from the bottom-right corner (aspect locked, 50 % … 300 % of the 780 × 560 base); SETUP's size buttons jump to
    // the fixed steps. Everything is vector-drawn and scaled by one transform in resized(), so any size (and any display scale,
    // Retina included: JUCE renders through the backing scale and sizes buffered images by the physical pixel scale) stays sharp.
    const int wanted = gSizePercent;   // read before setResizeLimits(): its constrained 0x0 -> minimum pass runs resized() and records 50 %
    setResizable (true, true);
    setResizeLimits (MainPanel::kW / 2, MainPanel::kH / 2, MainPanel::kW * 3, MainPanel::kH * 3);
    getConstrainer()->setFixedAspectRatio ((double) MainPanel::kW / (double) MainPanel::kH);
    setSizePercent (wanted);
    refreshSlow();
    lastMs_ = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

NovaInsertEditor::~NovaInsertEditor() { stopTimer(); }

juce::RangedAudioParameter& NovaInsertEditor::param (ni::plugin::Param p) const
{
    auto* r = dynamic_cast<juce::RangedAudioParameter*> (proc_.getParameters()[static_cast<int> (p)]);
    jassert (r != nullptr);
    return *r;
}

std::unique_ptr<juce::ParameterAttachment> NovaInsertEditor::attachSegment (Segment& seg, ni::plugin::Param p)
{
    auto att = std::make_unique<juce::ParameterAttachment> (param (p), [&seg] (float v) { seg.setSelected (juce::roundToInt (v)); });
    auto* raw = att.get();
    seg.onChange = [raw] (int i) { raw->setValueAsCompleteGesture (static_cast<float> (i)); };
    att->sendInitialUpdate();
    return att;
}

void NovaInsertEditor::setSizePercent (int percent)
{
    setSize (MainPanel::kW * percent / 100, MainPanel::kH * percent / 100);   // resized() records the resulting percent
}

void NovaInsertEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (sk().editorBase));   // housing base: only visible as a letterbox when the host ignores the aspect lock
}

void NovaInsertEditor::resized()
{
    // Fit the whole panel: hosts that resize the float window verbatim (REAPER applies the user's drag without asking the
    // plugin's constrainer) must never clip the panel, so the smaller axis wins and the rest is letterboxed, centred.
    const float scale = std::min ((float) getWidth() / (float) MainPanel::kW, (float) getHeight() / (float) MainPanel::kH);
    const float x = ((float) getWidth() - scale * (float) MainPanel::kW) * 0.5f, y = ((float) getHeight() - scale * (float) MainPanel::kH) * 0.5f;
    panel_.setTransform (juce::AffineTransform::scale (scale).translated (x, y));
    panel_.setBounds (0, 0, MainPanel::kW, MainPanel::kH);
    gSizePercent = juce::roundToInt (scale * 100.0f);
    panel_.setSizePercent (gSizePercent);
}

void NovaInsertEditor::refreshSlow()
{
    const auto in = proc_.viewInputs();
    if (const Skin s = skinFromStored (in.skin); s != currentSkin())
    {
        panel_.applySkin (s);   // this editor's SETUP choice, or one made in another instance (library poll)
        repaint();
    }
    const auto vm = ViewModel::from (in);
    const std::string footer = (vm.timestampText.empty() ? std::string ("not measured") : "measured " + vm.timestampText)
                               + "  ·  algorithm v" + std::to_string (ni::engine::kCalibAlgorithmVersion)
                               + "  ·  " + std::string (ni::engine::kVersion);
    panel_.update (in, proc_.selectedDevice(), footer);
}

void NovaInsertEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = std::max (0.0, (now - lastMs_) / 1000.0);
    lastMs_ = now;
    slowAcc_ += dt;
    if (slowAcc_ >= kSlowPeriod)
    {
        slowAcc_ = 0.0;
        if (++slowTicks_ % 8 == 0) proc_.pollLibrary();   // 1 Hz: presets saved elsewhere, host channel names
        refreshSlow();
    }
    panel_.setMeters (proc_.sendMeter().read (proc_.vuReference()), proc_.returnMeter().read (proc_.vuReference()));
    panel_.advance (dt);
}
} // namespace ni::gui
