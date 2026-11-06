// Level-bar meter views (BARS user request 2026-11-05; three selectable looks user 2026-11-06: "All 3 looks cool, make all 3
// of them selectable"), an alternative to the two VU faces in the same 724 × 205 meter box:
//   TUBE — one metallic bezel, a backlit cream face, SEND above RETURN, each channel a rounded glass light-pipe;
//   LED  — the same layout on a smoked-glass face, each channel 60 domed one-dB LED segments;
//   TWIN — two VU-sized windows (SEND left, RETURN right) with the cream face, tubes and a clip lamp each.
// Every look shows: the dBu scale above the bars (the group's converter alignment: send D/A, return A/D), the L/R peak with
// REAPER-like ballistics (instant rise, 24 dB/s fall), the RMS, the engine's 10 s peak hold, the 0 VU reference line, the
// outboard maximum as a small triangle on the SEND dBu scale, a fixed −60 … 0 dBFS scale below and the PEAK HOLD readouts.
// Layers: the face (bezels, scales, legends, empty troughs/segments) is cached to an image and only redrawn by setConfig /
// setStyle; the bars, readouts and glass sheen repaint every frame they change. Original vector drawing only.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>
#include "engine/meters.hpp"
#include "gui/bar_scale.hpp"

namespace ni::gui
{
class LevelBars final : public juce::Component, public juce::SettableTooltipClient
{
public:
    using Reading = engine::MeterEngine::Reading;
    static constexpr int kW = 724, kH = 205, kBezel = 9, kTwinW = 343, kTwinGap = 38;   // TWIN windows: x 0 and kTwinW + kTwinGap

    LevelBars();
    void setConfig (bool sendStereo, bool retStereo, float sendDbuAt0dBFS, float returnDbuAt0dBFS, float vuRefDbfs, std::optional<float> outboardMaxDbu, bool dbu = true);
    void setStyle (MeterView v);                                  // Tube, Led or Twin; Vu is not a bar look and keeps the current one
    MeterView style() const noexcept { return style_; }
    void setReadings (const Reading& send, const Reading& ret);   // every frame: peaks drive the fall, RMS and hold are shown as given
    void advance (double dt);
    void settle();                                                // snapshots/tests: bars at the current peaks, no fall in flight
    float shownPeakDb (bool send, int ch) const noexcept { return (send ? send_ : ret_).fall[ch & 1].shownDb; }

    void resized() override;

    struct Geo;   // one group's layout in its face's coordinates (level_bars.cpp)

private:
    struct Group { BarFall fall[2]; Reading r {}; float dbuAt0 = 24.0f; bool stereo = false; };   // per-direction width
    struct Face final : juce::Component { LevelBars* m = nullptr; void paint (juce::Graphics&) override; };
    struct Bars final : juce::Component { LevelBars* m = nullptr; void paint (juce::Graphics&) override; };
    void paintFace (juce::Graphics&) const;
    void paintBars (juce::Graphics&) const;
    void paintGroupFace (juce::Graphics&, const Geo&, bool send) const;
    void paintGroupBars (juce::Graphics&, const Geo&, bool send) const;
    juce::String holdText (const Group&, int ch) const;

    Face face_;
    Bars bars_;
    Group send_, ret_;
    MeterView style_ = MeterView::Tube;
    float vuRef_ = -20.0f;
    bool dbu_ = true;   // PEAK HOLD readouts and the 0 VU line in dBu (0 VU = +4 dBu) or dBFS (the SETUP reference), both groups alike
    std::optional<float> outboardMaxDbu_;
    float drawn_[4] { -1.0f, -1.0f, -1.0f, -1.0f };
};
} // namespace ni::gui
