// ni_manual_shots: renders the editor states shown in the user manual (docs/manual) at 2x from fixed ViewInputs, the same way
// tests/tools/gui_snapshot.cpp does, plus a numbered-callout overview. Usage: ni_manual_shots <output dir>.
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <cstdio>
#include "engine/version.hpp"
#include "gui/main_panel.hpp"

using namespace ni::gui;
using Reading = ni::engine::MeterEngine::Reading;

namespace
{
juce::File gOut;
constexpr float kScale = 2.0f;

void save (const juce::Image& img, const juce::String& name)
{
    auto f = gOut.getChildFile (name);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    if (! os.openedOk() || ! png.writeImageToStream (img, os)) { std::fprintf (stderr, "cannot write %s\n", name.toRawUTF8()); std::exit (1); }
    std::printf ("%s\n", f.getFullPathName().toRawUTF8());
}

Reading sine (float rmsL, float rmsR, bool clip = false, bool near = false)
{
    Reading r {};
    const float rms[2] = { rmsL, rmsR };
    for (int c = 0; c < 2; ++c)
    {
        r.rmsDb[c] = rms[c];
        r.peakDb[c] = r.peakHoldDb[c] = rms[c] + 3.01f;
        r.vu[c] = rms[c];
        r.rect[c] = std::pow (10.0f, rms[c] / 20.0f) * std::sqrt (2.0f) * 2.0f / juce::MathConstants<float>::pi;
        r.clip[c] = clip;
        r.nearFullScale[c] = near || clip;
    }
    return r;
}
const Reading kSilent = sine (-120.0f, -120.0f);

ViewInputs base (bool stereo)
{
    ViewInputs in;
    in.deviceName = stereo ? "Pultec EQP-1A" : "1176 LN";
    in.interfaceName = "CoreAudio Studio Interface";
    in.outName = stereo ? "Out 11/12" : "Out 11";
    in.inName = stereo ? "In 9/10" : "In 9";
    in.state = ni::engine::State::Locked;
    in.physLoop = 412; in.reportedPdc = 640; in.driverIn = 120; in.driverOut = 140;
    in.timestampUnix = 1793633136;
    in.sendStereo = in.retStereo = in.recSendStereo = in.recReturnStereo = stereo;
    in.sendDbuAt0dBFS = 24.0f; in.returnDbuAt0dBFS = 24.0f; in.vuRefDbfs = -20.0f;
    in.hostVersion = "7.83/macOS-arm64";
    in.devices = { { "1176 LN", "" }, { "Pultec EQP-1A", "" }, { "SSL EQ L", "" }, { "SSL EQ stereo", "Vocal Bus" } };
    return in;
}

std::string footer (const ViewInputs& in)
{
    const auto vm = ViewModel::from (in);
    return (vm.timestampText.empty() ? std::string ("not measured") : "measured " + vm.timestampText) + "  ·  algorithm v1  ·  " + std::string (ni::engine::kVersion);
}

juce::Image body (MainPanel& p, const ViewInputs& in, int device, const Reading& s, const Reading& r)
{
    p.mode.setSelected (in.mode == ni::engine::Mode::Effect ? 1 : 0);
    p.update (in, device, footer (in));
    p.setMeters (s, r);
    p.settleNeedles();
    p.update (in, device, footer (in));
    return p.createComponentSnapshot (p.getLocalBounds(), true, kScale);
}

// numbered badge at base-size coordinates
void badge (juce::Graphics& g, int n, float x, float y)
{
    const float r = 13.0f * kScale;
    g.setColour (juce::Colour (0xFFE0A325));
    g.fillEllipse (x * kScale - r, y * kScale - r, 2 * r, 2 * r);
    g.setColour (juce::Colours::black);
    g.drawEllipse (x * kScale - r, y * kScale - r, 2 * r, 2 * r, 2.0f);
    g.setFont (juce::Font (juce::FontOptions (15.0f * kScale, juce::Font::bold)));
    g.drawText (juce::String (n), juce::Rectangle<float> (x * kScale - r, y * kScale - r, 2 * r, 2 * r), juce::Justification::centred);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: ni_manual_shots <output dir>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    gOut = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (juce::CharPointer_UTF8 (argv[1])));
    gOut.createDirectory();

    MainPanel p;
    p.setBounds (0, 0, MainPanel::kW, MainPanel::kH);
    p.sendSel.setSelected (0);
    p.returnSel.setSelected (0);
    p.sendTrim.setValue (0.0, juce::dontSendNotification);
    p.returnTrim.setValue (0.0, juce::dontSendNotification);

    // 1. overview: stereo Pultec, Locked, program material, with callouts
    auto st = base (true);
    {
        auto img = body (p, st, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f));
        juce::Graphics g (img);
        badge (g, 1, 48, 30);     // device selector
        badge (g, 2, 578, 30);    // status lamp
        badge (g, 3, 48, 100);    // SEND VU
        badge (g, 4, 430, 100);   // RETURN VU
        badge (g, 5, 60, 320);    // send trim
        badge (g, 6, 180, 318);   // CAL +4 dBu
        badge (g, 7, 389, 322);   // LINK
        badge (g, 8, 485, 318);   // RETURN CAL
        badge (g, 9, 720, 320);   // return trim
        badge (g, 10, 48, 442);   // MODE
        badge (g, 11, 240, 442);  // SEND / RETURN selectors
        badge (g, 12, 636, 442);  // PRESET SETUP
        badge (g, 13, 180, 484);  // SETUP (status row, left)
        badge (g, 14, 262, 500);  // status lines
        badge (g, 18, 180, 518);  // RE-CALIBRATION (under SETUP)
        badge (g, 15, 505, 527);  // footer
        badge (g, 16, 376, 30);   // VU | TUBE | LED | TWIN meter view
        badge (g, 17, 476, 30);   // dBu | dBFS meter unit
        save (img, "overview.png");
    }
    save (body (p, st, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "stereo_locked.png");

    // 2. mono 1176, Locked, send from L, return to both
    auto mono = base (false);
    save (body (p, mono, 1, sine (-22.0f, -120.0f), sine (-22.5f, -120.0f)), "mono_locked.png");

    // 2b. the bar meter views TUBE (bars.png), LED, TWIN (2026-11-05/06): stereo with an outboard limit, and mono
    p.setMeterView (1);
    {
        auto lim = st;
        lim.outboardMaxInDbu = 22.0f;
        save (body (p, lim, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "bars.png");
        save (body (p, mono, 1, sine (-22.0f, -120.0f), sine (-22.5f, -120.0f)), "bars_mono.png");
        p.setMeterView (2);   // LED
        save (body (p, lim, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "bars_led.png");
        save (body (p, mono, 1, sine (-22.0f, -120.0f), sine (-22.5f, -120.0f)), "bars_led_mono.png");
        p.setMeterView (3);   // TWIN
        save (body (p, lim, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "bars_twin.png");
        save (body (p, mono, 1, sine (-22.0f, -120.0f), sine (-22.5f, -120.0f)), "bars_twin_mono.png");
    }
    p.setMeterView (0);

    // 2c. one meter unit for both sides (user 2026-11-05): dBFS
    p.setMeterUnit (1);
    save (body (p, st, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "meters_dbfs.png");
    p.setMeterUnit (0);

    // 3. no device
    ViewInputs none;
    none.hostVersion = "7.83/macOS-arm64";
    none.devices = st.devices;
    save (body (p, none, 0, kSilent, kSilent), "no_device.png");

    // 4. measuring (probe running: host output muted, meters silent)
    auto meas = base (true);
    meas.state = ni::engine::State::Measuring; meas.physLoop = -1; meas.reportedPdc = -1; meas.timestampUnix = 0;
    save (body (p, meas, 2, kSilent, kSilent), "measuring.png");

    // 5. queued (project with many instances)
    auto q = meas; q.state = ni::engine::State::Queued;
    save (body (p, q, 2, kSilent, kSilent), "queued.png");

    // 6. stale: routing changed, re-measure pending
    auto stale = base (true);
    stale.state = ni::engine::State::Stale; stale.reason = ni::engine::Reason::RoutingChange;
    save (body (p, stale, 2, sine (-30.0f, -28.0f), sine (-40.0f, -40.0f)), "stale.png");

    // 7. failed: no return
    auto failed = base (true);
    failed.state = ni::engine::State::Failed; failed.reason = ni::engine::Reason::NoReturn; failed.physLoop = -1; failed.reportedPdc = -1; failed.timestampUnix = 0;
    save (body (p, failed, 2, sine (-21.0f, -19.5f), kSilent), "failed.png");

    // 7b. measure on project open failed: the latency saved with the project is running (user 2026-11-06)
    auto fb = base (true);
    fb.reason = ni::engine::Reason::StateRestore; fb.openFallback = true; fb.openFallbackReason = ni::engine::Reason::NoReturn;
    save (body (p, fb, 2, sine (-21.0f, -19.5f), kSilent), "open_fallback.png");
    {   // RE-CALIBRATION live (user 2026-11-06): the bottom rows of the same state, close up
        const auto im = body (p, fb, 2, sine (-21.0f, -19.5f), kSilent);
        save (im.getClippedImage ({ 0, im.getHeight() * 430 / 560, im.getWidth(), im.getHeight() * 130 / 560 }), "recalibrate.png");
    }

    // 8. conflict: another instance owns the channels
    auto conf = base (true);
    conf.deviceName = "SSL EQ stereo"; conf.state = ni::engine::State::Unavailable; conf.reason = ni::engine::Reason::PairConflict;
    conf.conflictWith = "Vocal Bus"; conf.physLoop = -1; conf.reportedPdc = -1; conf.timestampUnix = 0; conf.outName = "Out 1/2"; conf.inName = "In 1/2";
    save (body (p, conf, 4, kSilent, kSilent), "conflict.png");

    // 9. anticipative FX re-enabled by the user
    auto ant = base (true);
    ant.state = ni::engine::State::UnsupportedHost; ant.reason = ni::engine::Reason::AnticipativeOn; ant.anticipativeViolationTracks = { "Drum Bus" };
    save (body (p, ant, 2, kSilent, kSilent), "anticipative.png");

    // 10. Effect mode: driver-reported loop
    auto eff = base (true);
    eff.deviceName = "Bricasti M7"; eff.mode = ni::engine::Mode::Effect; eff.physLoop = 260; eff.reportedPdc = 512;
    eff.devices = { { "1176 LN", "" }, { "Bricasti M7", "" }, { "Pultec EQP-1A", "" } };
    save (body (p, eff, 2, sine (-24.0f, -24.0f), sine (-26.0f, -25.0f)), "effect.png");
    p.mode.setSelected (0);

    // 11. SEND CAL running: tone at 0 VU
    auto cal = base (true);
    cal.calStatus = 1; cal.sendTrimDb = 0.0f;
    save (body (p, cal, 2, sine (-20.0f, -20.0f), sine (-20.0f, -20.0f)), "send_cal.png");

    // 12. RETURN CAL finished: unity, trims written
    auto done = base (true);
    done.calStatus = 4; done.sendTrimDb = -2.0f; done.returnTrimDb = 2.0f; done.link = true;
    p.sendTrim.setValue (-2.0, juce::dontSendNotification); p.returnTrim.setValue (2.0, juce::dontSendNotification);
    p.link.setToggleState (true, juce::dontSendNotification);
    save (body (p, done, 2, sine (-20.0f, -20.0f), sine (-20.0f, -20.0f)), "return_cal_done.png");

    // 13. RETURN CAL refused: the converter input clipped
    auto clip = done; clip.calStatus = 9; clip.link = false;
    p.link.setToggleState (false, juce::dontSendNotification);
    save (body (p, clip, 2, sine (-20.0f, -20.0f), sine (-0.5f, -0.3f, true)), "return_cal_clip.png");
    p.sendTrim.setValue (0.0, juce::dontSendNotification); p.returnTrim.setValue (0.0, juce::dontSendNotification);

    // 14. peak and clip LEDs lit while playing
    save (body (p, st, 2, sine (-4.0f, -3.5f, false, true), sine (-1.0f, 0.2f, true)), "peak_clip.png");

    // 15. inherent gap + lead-in tip, device defaults changed
    auto gap = base (true);
    gap.gapSamples = 640; gap.defaultsChanged = true;
    save (body (p, gap, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "gap_tip.png");

    // 16. SETUP overlay (20.2 dBu converter, outboard limit, offsets)
    auto setup = base (true);
    setup.sendDbuAt0dBFS = 20.2f; setup.returnDbuAt0dBFS = 20.2f; setup.outboardMaxInDbu = 22.0f; setup.vuRefDbfs = -18.0f;
    setup.sendOffsetDb[1] = 0.3f; setup.returnOffsetDb[0] = -0.2f; setup.defaultsChanged = true;
    p.showSetup (true);
    save (body (p, setup, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "setup.png");
    p.showSetup (false);

    // 17. outboard max marker on the SEND scale (+22 dBu max → marker at +18 VU... shown relative)
    save (body (p, setup, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "setup_applied.png");

    {   // DEVICES page (chapter 4): the running preset's record in the form, the host's channel names in the pickers
        auto dv = base (true);
        for (int i = 0; i < 16; ++i) { dv.outChannelNames.push_back ("Output " + std::to_string (i + 1)); dv.inChannelNames.push_back ("Input " + std::to_string (i + 1)); }
        auto rec = [] (const char* id, const char* name, bool stereo, int o0, int o1, int i0, int i1) {
            ni::plugin::DeviceRecord r; r.id = id; r.name = name; r.sendStereo = r.returnStereo = stereo; r.out[0] = o0; r.out[1] = o1; r.in[0] = i0; r.in[1] = i1; return r; };
        dv.records = { rec ("1176-ln", "1176 LN", false, 10, -1, 8, -1), rec ("pultec-eqp-1a", "Pultec EQP-1A", true, 10, 11, 8, 9),
                       rec ("ssl-eq-l", "SSL EQ L", false, 12, -1, 10, -1), rec ("ssl-eq-stereo", "SSL EQ stereo", true, 12, 13, 10, 11) };
        dv.records[1].outboardMaxInDbu = 22.0f;
        p.showDevices (true);
        save (body (p, dv, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "devices_page.png");
        p.devices.beginNew();
        save (body (p, dv, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "devices_new.png");
        p.showDevices (false);
    }

    // 18. the device list as the popup draws it
    {
        auto& lnf = p.getLookAndFeel();
        const auto vm = ViewModel::from (st);
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

    // 19. the light skins (SETUP → Skin, user 2026-11-06): the main panel, the TUBE bars and SETUP in Ivory and in Silver
    for (auto [skin, name] : { std::pair { Skin::Ivory, "ivory" }, std::pair { Skin::Silver, "silver" } })
    {
        const juce::String n (name);
        auto in = base (true);
        in.skin = static_cast<int> (skin);
        p.applySkin (skin);
        save (body (p, in, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "skin_" + n + ".png");
        p.setMeterView (1);
        save (body (p, in, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "skin_" + n + "_bars.png");
        p.setMeterView (0);
        p.showSetup (true);
        save (body (p, in, 2, sine (-21.0f, -19.5f), sine (-20.5f, -20.0f)), "skin_" + n + "_setup.png");
        p.showSetup (false);
    }
    p.applySkin (Skin::Dark);
    return 0;
}
