// MainPanel (VU GUI spec 2026-11-02 §4, mockup v5): the whole editor body at the base size 780 × 560 – top bar (green LCD
// device list + status lamp), SEND and RETURN VU meters, control strips (trims, CAL buttons, readouts, PEAK/CLIP LEDs, LINK),
// bottom row (MODE, SEND/RETURN selectors, ⚙ SETUP), status lines, and the SETUP overlay. JUCE-only and independent of the
// processor so tools/gui_snapshot can render it; the editor wires the controls to parameters and the callbacks to the
// processor and scales it with a transform.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/meters.hpp"
#include "gui/controls.hpp"
#include "gui/devices_panel.hpp"
#include "gui/level_bars.hpp"
#include "gui/setup_panel.hpp"
#include "gui/view_model.hpp"
#include "gui/vu_meter.hpp"

namespace ni::gui
{
// A trim knob: 0.1 dB steps; every user value (drag, wheel, double-click – snapValue is only called for user input) goes
// through onUser, which with LINK on stops the knob where the other trim hits its range edge and moves the other trim in the
// same call. The knob is never pulled back after the fact (user 2026-11-06: "with the link enabled turning the input down
// made the input knob glitch on low settings"), and the processor sees both trims move together, so it does not re-link.
class TrimKnob final : public juce::Slider
{
public:
    std::function<double (double)> onUser;    // limit + move the LINK partner (drag, wheel)
    std::function<double (double)> onLimit;   // limit only (double-click: the processor's LINK moves the partner, inside the room)
    bool reverseWheel = false;                // SETUP "Mouse wheel: reverse" (GitHub #1)
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {   // JUCE turns the knob up for a wheel moved up, using macOS's "natural scrolling" flag on the event. A scroll utility
        // (Logi Options+, Mos, Scroll Reverser, LinearMouse...) that reverses only the mouse leaves that flag set, so the knob
        // runs backwards and no plugin can tell; the global SETUP option flips it back.
        auto d = w;
        if (reverseWheel) { d.deltaX = -d.deltaX; d.deltaY = -d.deltaY; }
        juce::Slider::mouseWheelMove (e, d);
    }
    double snapValue (double attempted, DragMode) override
    {
        const double v = std::round (attempted * 10.0) / 10.0;
        return onUser ? onUser (v) : v;
    }
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {   // JUCE's own double-click (with its drag start/end, so the parameter gets a gesture), aimed at 0 dB as far as LINK allows
        setDoubleClickReturnValue (true, onLimit ? onLimit (0.0) : 0.0);
        juce::Slider::mouseDoubleClick (e);
    }
};

class MainPanel final : public juce::Component
{
public:
    static constexpr int kW = 780, kH = 560;
    using Reading = engine::MeterEngine::Reading;

    MainPanel();
    ~MainPanel() override;

    void update (const ViewInputs&, int selectedDevice, const std::string& footer);   // ~8 Hz: texts, lists, readouts
    void setMeters (const Reading& send, const Reading& ret);                          // every frame: needle drives, LEDs
    void advance (double dt) { sendVu_.advance (dt); returnVu_.advance (dt); bars_.advance (dt); }
    void settleNeedles();                                                              // snapshot: needles at rest on the drives, bars at the peaks
    void setMeterView (int view);                                                      // MeterView index: 0 VU faces, 1 TUBE, 2 LED, 3 TWIN bars; global, settings.json "meterView"
    int currentMeterView() const noexcept { return meterView.selected(); }
    void setMeterUnit (int unit);                                                      // 0 = dBu, 1 = dBFS on BOTH meters (user 2026-11-05); global, settings.json "meterUnit"
    void showSetup (bool on);
    void showDevices (bool on);   // SETUP and DEVICES never show together
    bool setupVisible() const { return setup.isVisible(); }
    void setSizePercent (int p) { sizePercent_ = p; }
    void applySkin (Skin);   // swap the palette, re-read colour ids, invalidate the cached meter faces, repaint
    double linkedUserValue (TrimKnob& self, TrimKnob& other, double attempted);   // LINK's limit + the other trim's opposite move

    TrimKnob sendTrim, returnTrim;
    juce::TextButton link, sendCal { "CAL +4 dBu" }, returnCal { "CAL" }, setupButton, recalibrate { "RE-CALIBRATION" }, devicesButton { "PRESET SETUP" }, reapply { "RE-APPLY ANTICIPATIVE FX" };
    Segment mode { { "INSERT", "EFFECT" }, 16 }, sendSel { { "STEREO", "L", "R" }, 16 }, returnSel { { "STEREO", "L", "R" }, 16 }, meterView { { "VU", "TUBE", "LED", "TWIN" }, 11 }, meterUnit { { "dBu", "dBFS" }, 12 };
    juce::ComboBox device;
    SetupPanel setup;
    DevicesPanel devices;

    std::function<void (int deviceIndex)> onSelectDevice;   // 0 = none, else library index + 1
    std::function<void (int action)> onCal;                  // 1 = SEND CAL toggle, 2 = RETURN CAL, 3 = cancel
    std::function<void()> onReapply;
    std::function<void()> onRecalibrate;                     // RE-CALIBRATION under SETUP (user 2026-11-06)
    std::function<void (int view)> onMeterView;              // the user switched the meter view (stored in settings.json, global)
    std::function<void (int unit)> onMeterUnit;              // the user switched the meter unit (same kind of preference)

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::String readout (const Reading&, bool send) const;

    NovaLookAndFeel lnf_;
    VuMeter sendVu_, returnVu_;
    LevelBars bars_;
    Led sendPeak_, returnClip_;
    StatusLamp lamp_;
    ViewInputs in_;
    juce::String paintKey_;   // the last painted text inputs (update() repaints only when they change)
    ViewModel vm_;
    Reading send_ {}, ret_ {};
    juce::String sendReadout_, returnReadout_, footer_;
    std::vector<std::pair<std::string, bool>> items_;
    int sizePercent_ = 100;
};
} // namespace ni::gui
