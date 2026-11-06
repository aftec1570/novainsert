#include <functional>
#include <typeinfo>
// ni_gui_snapshot (VU GUI plan Task 9): renders the VU meter and the editor body with fixed ViewInputs to PNGs so the drawing
// can be checked without a host. Usage: ni_gui_snapshot <output dir>. Writes vu_*.png, body_*.png and device_list.png.
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstdio>
#include "gui/main_panel.hpp"

using namespace ni::gui;
using Reading = ni::engine::MeterEngine::Reading;

namespace
{
juce::File gOut;

void save (const juce::Image& img, const char* name)
{
    auto f = gOut.getChildFile (name);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    if (! os.openedOk() || ! png.writeImageToStream (img, os)) { std::fprintf (stderr, "cannot write %s\n", name); std::exit (1); }
    std::printf ("%s\n", f.getFullPathName().toRawUTF8());
}

// a steady sine at rmsDbfs on both channels (rect = 2√2/π · rms)
Reading sine (float rmsL, float rmsR, bool peak = false)
{
    Reading r {};
    const float rms[2] = { rmsL, rmsR };
    for (int c = 0; c < 2; ++c)
    {
        r.rmsDb[c] = rms[c];
        r.peakDb[c] = r.peakHoldDb[c] = rms[c] + 3.01f;
        r.vu[c] = rms[c];
        r.rect[c] = std::pow (10.0f, rms[c] / 20.0f) * std::sqrt (2.0f) * 2.0f / juce::MathConstants<float>::pi;
        r.clip[c] = peak;
        r.nearFullScale[c] = peak;
    }
    return r;
}

ViewInputs base (bool stereo)
{
    ViewInputs in;
    in.deviceName = stereo ? "Pultec EQP-1A" : "1176 LN";
    in.interfaceName = "Apollo x16";
    in.outName = stereo ? "Out 11/12" : "Out 11";
    in.inName = stereo ? "In 9/10" : "In 9";
    in.state = ni::engine::State::Locked;
    in.physLoop = 412; in.reportedPdc = 412; in.driverIn = 120; in.driverOut = 140;
    in.timestampUnix = 1793633136;
    in.sendStereo = in.retStereo = in.recSendStereo = in.recReturnStereo = stereo;
    in.sendDbuAt0dBFS = 24.0f; in.returnDbuAt0dBFS = 24.0f; in.vuRefDbfs = -18.0f;
    in.sendTrimDb = -2.0f; in.returnTrimDb = 2.0f;
    in.devices = { { "1176 LN", "" }, { "Pultec EQP-1A", "" }, { "SSL EQ L", "" }, { "SSL EQ stereo", "Vocal Bus" } };
    return in;
}

juce::Image body (MainPanel& p, const ViewInputs& in, const Reading& s, const Reading& r, float scale)
{
    p.update (in, 2, "measured 2026-11-02 12:00:00  ·  algorithm v1  ·  0.1.0");
    p.setMeters (s, r);
    p.settleNeedles();
    p.update (in, 2, "measured 2026-11-02 12:00:00  ·  algorithm v1  ·  0.1.0");
    return p.createComponentSnapshot (p.getLocalBounds(), true, scale);
}
} // namespace

// --link-check (user 2026-11-06: "With the link enabled turning the input down made the input knob glitch on low settings"):
// real mouse drags through JUCE's Slider on the real MainPanel. With LINK on, the dragged trim must stop where the other hits
// its range edge, never pass it (no pull-back), and the other must follow in the same event, so send + return stays constant.
int fails = 0;
void expect (bool ok, const char* what, double a, double b)
{
    if (! ok) ++fails;
    std::printf ("%s %s (%.2f, %.2f)\n", ok ? "PASS" : "FAIL", what, a, b);
}

juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::Point<float> down, int clicks, bool dragged)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    return juce::MouseEvent (src, pos, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                             juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), clicks, dragged);
}

// drags k vertically by dy pixels in 2 px steps; returns the lowest/highest value k showed and checks the sum on every step
void drag (TrimKnob& k, TrimKnob& other, float dy, double& minSeen, double& maxSeen, bool& sumHeld, double sum)
{
    const juce::Point<float> c (41.0f, 41.0f);
    k.mouseDown (mouseAt (k, c, c, 1, false));
    minSeen = maxSeen = k.getValue();
    for (float y = dy > 0 ? 2.0f : -2.0f; std::abs (y) <= std::abs (dy); y += dy > 0 ? 2.0f : -2.0f)
    {
        k.mouseDrag (mouseAt (k, c.translated (0.0f, y), c, 1, true));
        minSeen = std::min (minSeen, k.getValue()); maxSeen = std::max (maxSeen, k.getValue());
        if (std::abs (k.getValue() + other.getValue() - sum) > 1e-4) sumHeld = false;
    }
    k.mouseUp (mouseAt (k, c.translated (0.0f, dy), c, 1, true));
}

int linkCheck()
{
    MainPanel p;
    p.setBounds (0, 0, MainPanel::kW, MainPanel::kH);
    p.sendTrim.setValue (0.0, juce::dontSendNotification);
    p.returnTrim.setValue (0.0, juce::dontSendNotification);
    p.link.setToggleState (true, juce::dontSendNotification);
    double mn = 0, mx = 0; bool sum = true;
    drag (p.sendTrim, p.returnTrim, 260.0f, mn, mx, sum, 0.0);   // down ~26 dB: LINK stops at −12 (return +12)
    expect (std::abs (mn + 12.0) < 1e-4, "send never below -12 while dragging down 26 dB", mn, p.sendTrim.getValue());
    expect (std::abs (p.sendTrim.getValue() + 12.0) < 1e-4 && std::abs (p.returnTrim.getValue() - 12.0) < 1e-4, "ends at send -12 / return +12",
            p.sendTrim.getValue(), p.returnTrim.getValue());
    expect (sum, "send + return held at 0 on every drag step", p.sendTrim.getValue(), p.returnTrim.getValue());
    sum = true;
    drag (p.sendTrim, p.returnTrim, -50.0f, mn, mx, sum, 0.0);   // back up 5 dB
    expect (std::abs (p.sendTrim.getValue() + 7.0) < 0.11 && sum, "back up: send -7 / return +7, sum held", p.sendTrim.getValue(), p.returnTrim.getValue());
    sum = true;
    drag (p.returnTrim, p.sendTrim, -300.0f, mn, mx, sum, 0.0);   // return up 30 dB: stops at +12 (send −12)
    expect (mx <= 12.0 + 1e-4 && std::abs (p.returnTrim.getValue() - 12.0) < 1e-4 && sum, "return up stops at +12, sum held",
            mx, p.sendTrim.getValue());
    sum = true;
    drag (p.returnTrim, p.sendTrim, 400.0f, mn, mx, sum, 0.0);   // return down 40 dB: stops at −12 (send +12)
    expect (std::abs (mn + 12.0) < 1e-4 && std::abs (p.sendTrim.getValue() - 12.0) < 1e-4 && sum, "return down stops at -12 (send +12), sum held",
            mn, p.sendTrim.getValue());
    // LINK on, send UP: from send 0 / return −20 the return has 4 dB of room → send stops at +4
    p.sendTrim.setValue (0.0, juce::dontSendNotification); p.returnTrim.setValue (-20.0, juce::dontSendNotification); sum = true;
    drag (p.sendTrim, p.returnTrim, -200.0f, mn, mx, sum, -20.0);
    expect (mx <= 4.0 + 1e-4 && std::abs (p.sendTrim.getValue() - 4.0) < 1e-4 && std::abs (p.returnTrim.getValue() + 24.0) < 1e-4 && sum,
            "linked send up stops at +4 (return -24), sum held", p.sendTrim.getValue(), p.returnTrim.getValue());
    // LINK on, off-grid CAL values (send 3.8, return −5.37): steps of 0.1 on the dragged knob, the partner exact, the edge exact
    p.sendTrim.setValue (3.8, juce::dontSendNotification); p.returnTrim.setValue (-5.37, juce::dontSendNotification); sum = true;
    drag (p.sendTrim, p.returnTrim, 300.0f, mn, mx, sum, 3.8 - 5.37);
    expect (std::abs (p.returnTrim.getValue() - 12.0) < 1e-4 && std::abs (p.sendTrim.getValue() - (3.8 - 5.37 - 12.0)) < 1e-4 && sum,
            "linked off-grid: send stops exactly where return hits +12", p.sendTrim.getValue(), p.returnTrim.getValue());
    // LINK on, double-click on the send: aims at 0 dB as far as the room allows (return 12 → send may rise to 0 only if return can fall)
    p.sendTrim.setValue (-12.0, juce::dontSendNotification); p.returnTrim.setValue (-20.0, juce::dontSendNotification);
    p.sendTrim.mouseDoubleClick (mouseAt (p.sendTrim, { 41.0f, 41.0f }, { 41.0f, 41.0f }, 2, false));
    expect (std::abs (p.sendTrim.getValue() + 8.0) < 1e-4, "linked double-click: send to -8 (return has 4 dB room)", p.sendTrim.getValue(), p.returnTrim.getValue());

    // LINK off: both knobs, both directions, full range, the partner never moves
    p.link.setToggleState (false, juce::dontSendNotification);
    struct Case { TrimKnob* k; TrimKnob* o; float dy; double want; const char* what; };
    const Case cases[] = { { &p.sendTrim, &p.returnTrim, 500.0f, -24.0, "unlinked send down to -24, return untouched" },
                           { &p.sendTrim, &p.returnTrim, -500.0f, 12.0, "unlinked send up to +12, return untouched" },
                           { &p.returnTrim, &p.sendTrim, 500.0f, -24.0, "unlinked return down to -24, send untouched" },
                           { &p.returnTrim, &p.sendTrim, -500.0f, 12.0, "unlinked return up to +12, send untouched" } };
    for (const auto& c : cases)
    {
        c.k->setValue (0.0, juce::dontSendNotification); c.o->setValue (-3.0, juce::dontSendNotification);
        drag (*c.k, *c.o, c.dy, mn, mx, sum, 0.0);
        expect (std::abs (c.k->getValue() - c.want) < 1e-4 && std::abs (c.o->getValue() + 3.0) < 1e-9 && mn >= -24.0 && mx <= 12.0, c.what,
                c.k->getValue(), c.o->getValue());
    }
    p.sendTrim.setValue (-15.0, juce::dontSendNotification); p.returnTrim.setValue (7.0, juce::dontSendNotification);
    p.sendTrim.mouseDoubleClick (mouseAt (p.sendTrim, { 41.0f, 41.0f }, { 41.0f, 41.0f }, 2, false));
    p.returnTrim.mouseDoubleClick (mouseAt (p.returnTrim, { 41.0f, 41.0f }, { 41.0f, 41.0f }, 2, false));
    expect (std::abs (p.sendTrim.getValue()) < 1e-9 && std::abs (p.returnTrim.getValue()) < 1e-9, "unlinked double-click: both to 0 dB",
            p.sendTrim.getValue(), p.returnTrim.getValue());
    std::printf ("%s\n", fails == 0 ? "LINK CHECK PASS" : "LINK CHECK FAIL");
    return fails == 0 ? 0 : 1;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: ni_gui_snapshot <output dir> | --link-check\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    if (juce::String (argv[1]) == "--link-check") return linkCheck();
    gOut = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (juce::CharPointer_UTF8 (argv[1])));
    gOut.createDirectory();

    {   // single meters: mono send (offset −20 = 4 − 24), stereo return (offset −18), outboard marker at +2 VU
        VuMeter m;
        m.setBounds (0, 0, 348, 208);
        m.setConfig (false, -20.0f, juce::String::fromUTF8 ("SEND · 0 VU = +4 dBu"), 2.0f);
        m.settle (1.0f, 0.0f);
        save (m.createComponentSnapshot (m.getLocalBounds(), true, 2.0f), "vu_mono_send.png");
        m.setConfig (true, -18.0f, juce::String::fromUTF8 ("RETURN · 0 VU = −18 dBFS"), std::nullopt);
        m.setDrive (0.5f, 1.2f, true);
        m.settle (0.5f, 1.2f);
        save (m.createComponentSnapshot (m.getLocalBounds(), true, 2.0f), "vu_stereo_return.png");
    }

    MainPanel p;
    p.setBounds (0, 0, MainPanel::kW, MainPanel::kH);
    p.sendTrim.setValue (-2.0, juce::dontSendNotification);
    p.returnTrim.setValue (2.0, juce::dontSendNotification);
    p.sendSel.setSelected (0);
    p.returnSel.setSelected (0);

    auto mono = base (false);
    save (body (p, mono, sine (-20.0f, -120.0f), sine (-21.0f, -120.0f), 1.0f), "body_mono.png");

    auto st = base (true);
    st.calStatus = 1;   // SEND CAL running: tone at 0 VU
    st.link = true;
    p.link.setToggleState (true, juce::dontSendNotification);
    save (body (p, st, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f, true), 1.0f), "body_stereo_cal.png");
    save (body (p, st, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f, true), 0.75f), "body_stereo_75.png");
    save (body (p, st, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f, true), 1.5f), "body_stereo_150.png");
    save (body (p, st, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f, true), 3.0f), "body_stereo_300.png");   // 150 % on a 2× Retina display

    auto err = base (true);
    err.calStatus = 9;
    err.state = ni::engine::State::Stale;
    err.reason = ni::engine::Reason::RoutingChange;
    err.defaultsChanged = true;
    err.outboardMaxInDbu = 22.0f;
    err.sendDbuAt0dBFS = 20.2f; err.returnDbuAt0dBFS = 20.2f;
    err.sendOffsetDb[1] = 0.3f; err.returnOffsetDb[0] = -0.2f;
    save (body (p, err, sine (-30.0f, -28.0f), sine (-40.0f, -40.0f), 1.0f), "body_error.png");
    p.showSetup (true);
    save (body (p, err, sine (-30.0f, -28.0f), sine (-40.0f, -40.0f), 1.0f), "body_setup.png");
    p.showSetup (false);
    auto fail = base (true);   // RE-CALIBRATION live: the open check failed, the saved latency runs (user 2026-11-06)
    fail.state = ni::engine::State::Locked;
    fail.openFallback = true; fail.openFallbackReason = ni::engine::Reason::NoReturn;
    save (body (p, fail, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f), 1.0f), "body_recalibrate.png");
    p.showSetup (false);

    {   // DEVICES page: the running preset's record in the form, host channel names in the pickers
        auto dv = base (true);
        for (int i = 0; i < 16; ++i) { dv.outChannelNames.push_back ("Apollo Out " + std::to_string (i + 1)); dv.inChannelNames.push_back ("Apollo In " + std::to_string (i + 1)); }
        auto rec = [] (const char* id, const char* name, bool stereo, int o0, int o1, int i0, int i1) {
            ni::plugin::DeviceRecord r; r.id = id; r.name = name; r.sendStereo = r.returnStereo = stereo; r.out[0] = o0; r.out[1] = o1; r.in[0] = i0; r.in[1] = i1; return r; };
        dv.records = { rec ("1176-ln", "1176 LN", false, 10, -1, 8, -1), rec ("pultec-eqp-1a", "Pultec EQP-1A", true, 10, 11, 8, 9),
                       rec ("ssl-eq-l", "SSL EQ L", false, 12, -1, 10, -1), rec ("ssl-eq-stereo", "SSL EQ stereo", true, 12, 13, 10, 11) };
        dv.records[1].outboardMaxInDbu = 22.0f;
        p.showDevices (true);
        save (body (p, dv, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f), 1.0f), "body_devices.png");
        p.devices.beginNew();
        save (body (p, dv, sine (-20.0f, -20.0f), sine (-18.5f, -16.5f), 1.0f), "body_devices_new.png");
        p.showDevices (false);
    }

    {   // the device list as the popup draws it: the stereo preset whose channels another track owns is greyed
        auto& lnf = p.getLookAndFeel();
        const auto vm = ViewModel::from (err);
        const int w = 420, rowH = 24, h = rowH * ((int) vm.deviceItems.size() + 1) + 8;
        juce::Image img (juce::Image::ARGB, w * 2, h * 2, true);
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (2.0f));
        lnf.drawPopupMenuBackground (g, w, h);
        int y = 4;
        auto item = [&] (const juce::String& t, bool enabled, bool ticked) {
            lnf.drawPopupMenuItem (g, { 0, y, w, rowH }, false, enabled, false, ticked, false, t, {}, nullptr, nullptr);
            y += rowH;
        };
        item ("No device", true, false);
        for (size_t i = 0; i < vm.deviceItems.size(); ++i) item (juce::String (vm.deviceItems[i].first), vm.deviceItems[i].second, i == 1);
        save (img, "device_list.png");
    }
    // hover tips (user 2026-11-06): every control the user can point at carries one; parts inside a control (a slider's text
    // box, a combo's label) are covered by their owner
    int missing = 0, checked = 0;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c) {
        for (auto* ch : c.getChildren())
        {
            auto* tip = dynamic_cast<juce::SettableTooltipClient*> (ch);
            const bool inner = dynamic_cast<juce::SettableTooltipClient*> (ch->getParentComponent()) != nullptr
                               && ch->getParentComponent() != &p;
            if (tip != nullptr && ! inner) ++checked;
            if (tip != nullptr && ! inner && tip->getTooltip().isEmpty())
            {
                std::fprintf (stderr, "no tooltip: %s in %s\n", typeid (*ch).name(), typeid (c).name());
                ++missing;
            }
            if (tip == nullptr || ! inner) walk (*ch);
        }
    };
    walk (p);
    std::printf ("tooltips: %d checked, %d missing\n", checked, missing);
    return missing == 0 ? 0 : 3;
}
