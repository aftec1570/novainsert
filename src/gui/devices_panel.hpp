// DEVICES page (spec docs/superpowers/specs/2026-11-05-novainsert-device-editor-design.md): the preset list and the form that
// edits one device record — name, send width + output channel(s), return width + input channel(s) (mono/stereo routing design
// 2026-11-05: a MONO direction has one channel picker), default mode, converter levels. The form
// edits a copy; SAVE hands the record to the processor (which validates, writes the library and selects it), DELETE removes
// it. Callbacks only; no library access here, so tools/gui_snapshot can render it.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "gui/controls.hpp"
#include "gui/view_model.hpp"

namespace ni::gui
{
class DevicesPanel final : public juce::Component, private juce::ListBoxModel
{
public:
    DevicesPanel();
    void applySkin();   // the LCD list / name box colours of the current skin
    void setInputs (const ViewInputs&, int selectedDevice);   // ~8 Hz: list + channel names; the form follows its record unless edited

    std::function<std::string (const plugin::DeviceRecord&)> onSave;   // returns the error text ("" = saved and selected)
    std::function<std::string (const std::string& id)> onDelete;       // returns the error text ("" = deleted)
    std::function<void()> onClose;

    bool editing() const noexcept { return dirty_; }
    const plugin::DeviceRecord& edited() const noexcept { return edit_; }   // tests / snapshots
    void beginNew();                                                        // NEW: an empty record in the form

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override { return (int) records_.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int row) override;

    void loadForm (const plugin::DeviceRecord&);
    void readForm();            // controls → edit_
    void fillChannelCombos();
    void setError (const std::string&);
    void changed() { if (! loading_) { readForm(); dirty_ = true; repaint(); } }

    juce::ListBox list_ { "devices", this };
    juce::TextButton newBtn_ { "NEW" }, dupBtn_ { "DUPLICATE" }, delBtn_ { "DELETE" }, save_ { "SAVE" }, cancel_ { "CANCEL" }, done_ { "DONE" };
    juce::TextEditor name_;
    Segment sendMode_ { { "MONO", "STEREO" } }, retMode_ { { "MONO", "STEREO" } }, mode_ { { "INSERT", "EFFECT" } };
    juce::ComboBox outL_, outR_, inL_, inR_, preset_, ref_;
    juce::Slider sendDbu_, returnDbu_, refCustom_, outMax_;
    juce::ToggleButton outMaxOn_ { "Limit" };

    std::vector<plugin::DeviceRecord> records_;
    std::vector<DeviceListEntry> entries_;
    std::vector<std::string> outNames_, inNames_;
    plugin::DeviceRecord edit_;
    std::string pendingName_;   // just saved: select this record when the list shows it
    std::string error_;
    int running_ = 0;           // 1-based library index the instance runs (0 = none)
    bool dirty_ = false, isNew_ = false, loading_ = false;
};
} // namespace ni::gui
