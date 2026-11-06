// NovaInsertEditor (VU GUI spec 2026-11-02 §4): hosts MainPanel at the base size 780 × 560, scaled by a transform to any size
// between 50 % and 300 % (bottom-right corner drag, aspect locked; SETUP's buttons jump to 75/100/125/150 %). Parameter attachments for the trims, LINK and the MODE/SEND/RETURN selectors; everything else goes
// through the processor's message-thread calls (device, CAL, converter, offsets, defaults). One 60 Hz timer: the needles
// integrate with the real frame time; texts, lists and readouts refresh at 8 Hz. Holds no coordination state.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "gui/main_panel.hpp"
#include "plugin/parameters.hpp"

class NovaInsertProcessor;

namespace ni::gui
{
class NovaInsertEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NovaInsertEditor (NovaInsertProcessor&);
    ~NovaInsertEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshSlow();
    void setSizePercent (int percent);
    juce::RangedAudioParameter& param (ni::plugin::Param p) const;
    std::unique_ptr<juce::ParameterAttachment> attachSegment (Segment&, ni::plugin::Param);

    NovaInsertProcessor& proc_;
    MainPanel panel_;
    std::unique_ptr<juce::SliderParameterAttachment> sendTrimAtt_, returnTrimAtt_;
    std::unique_ptr<juce::ButtonParameterAttachment> linkAtt_;
    std::unique_ptr<juce::ParameterAttachment> modeAtt_, sendSelAtt_, returnSelAtt_;
    double lastMs_ = 0.0, slowAcc_ = 0.0;
    unsigned slowTicks_ = 0;
    juce::TooltipWindow tooltips_ { this, 700 };   // hover tips on every control (user 2026-11-06)
};
} // namespace ni::gui
