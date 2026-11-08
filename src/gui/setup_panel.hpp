// SETUP panel (VU GUI spec 2026-11-02 §3/§6): the audio device REAPER reports (read-only, 1.0.1), send / return converter
// dBu at 0 dBFS typed in (default +24.0), the 0 VU reference (−20 / −18 / custom), the optional outboard maximum input,
// per-channel L/R send/return offsets in 0.1 dB steps, the editor size (75/100/125/150 %), and "Apply device defaults" when
// the library record's defaults differ from this instance. Callbacks only; holds no state the processor does not own.
#pragma once
#include <array>
#include <optional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/controls.hpp"
#include "gui/view_model.hpp"

namespace ni::gui
{
class SetupPanel final : public juce::Component
{
public:
    struct Converter { float sendDbu, returnDbu, vuRef; std::optional<float> outboardMax; };
    static constexpr int kSizes[4] = { 75, 100, 125, 150 };

    SetupPanel();
    void setValues (const ViewInputs&, int sizePercent);   // skipped for a control the user is dragging or editing

    std::function<void (const Converter&)> onConverter;
    std::function<void (std::array<float, 2> send, std::array<float, 2> ret)> onOffsets;
    std::function<void()> onApplyDefaults, onClose;
    std::function<void (int percent)> onSize;
    std::function<void (bool on)> onMeasureOnOpen;   // global: every instance measures its loop when a project opens
    std::function<void (int skin)> onSkin;            // global GUI skin: 0 Dark / 1 Ivory / 2 Silver
    std::function<void (bool)> onReverseWheel;        // global: trim knobs turn the other way on the mouse wheel

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void emitConverter();
    void emitOffsets();
    static void setupNumber (juce::Slider&, double lo, double hi, double step, const juce::String& unit);

    juce::ComboBox ref_;
    juce::String interface_;   // the audio device REAPER reports (read-only line)
    juce::Slider sendDbu_, returnDbu_, refCustom_, outMax_;
    juce::ToggleButton outMaxOn_ { "Limit" };
    juce::ToggleButton measureOnOpen_ { juce::String::fromUTF8 ("Measure the loop (all instances; off = use the latency saved with the project)") };
    juce::ToggleButton reverseWheel_ { juce::String::fromUTF8 ("Reverse (turn this on if wheel up turns the trim knobs down)") };
    juce::Slider off_[4];   // send L, send R, return L, return R
    juce::TextButton apply_ { "APPLY DEVICE DEFAULTS" }, done_ { "DONE" };
    Segment size_ { { "75 %", "100 %", "125 %", "150 %" } };
    Segment skin_ { { "DARK", "IVORY", "SILVER" } };
    bool updating_ = false, sendStereo_ = false, retStereo_ = false, defaultsChanged_ = false;
};
} // namespace ni::gui
