// SETUP panel (VU GUI spec 2026-11-02 §3/§6): converter calibration (presets Apollo x / +4 = 24 dBu, Apollo Twin MkII =
// 20.2 dBu, Custom), return converter dBu, the 0 VU reference (−20 / −18 / custom), the optional outboard maximum input,
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
    static constexpr float kApolloDbu = 24.0f, kTwinMk2Dbu = 20.2f;
    static constexpr int kSizes[4] = { 75, 100, 125, 150 };

    SetupPanel();
    void setValues (const ViewInputs&, int sizePercent);   // skipped for a control the user is dragging or editing

    std::function<void (const Converter&)> onConverter;
    std::function<void (std::array<float, 2> send, std::array<float, 2> ret)> onOffsets;
    std::function<void()> onApplyDefaults, onClose;
    std::function<void (int percent)> onSize;
    std::function<void (bool on)> onMeasureOnOpen;   // global: every instance measures its loop when a project opens
    std::function<void (int skin)> onSkin;            // global GUI skin: 0 Dark / 1 Ivory / 2 Silver

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void emitConverter();
    void emitOffsets();
    static void setupNumber (juce::Slider&, double lo, double hi, double step, const juce::String& unit);

    juce::ComboBox preset_, ref_;
    juce::Slider sendDbu_, returnDbu_, refCustom_, outMax_;
    juce::ToggleButton outMaxOn_ { "Limit" };
    juce::ToggleButton measureOnOpen_ { juce::String::fromUTF8 ("Measure the loop (all instances; off = use the latency saved with the project)") };
    juce::Slider off_[4];   // send L, send R, return L, return R
    juce::TextButton apply_ { "APPLY DEVICE DEFAULTS" }, done_ { "DONE" };
    Segment size_ { { "75 %", "100 %", "125 %", "150 %" } };
    Segment skin_ { { "DARK", "IVORY", "SILVER" } };
    bool updating_ = false, sendStereo_ = false, retStereo_ = false, defaultsChanged_ = false;
};
} // namespace ni::gui
