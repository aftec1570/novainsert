// JSON forms of the library records, shared by the library files and the VST3 state blob (persistence). Message thread only.
#pragma once
#include <juce_core/juce_core.h>
#include "plugin/library.hpp"

namespace ni::plugin::json
{
juce::var fromDevice(const DeviceRecord& r);
DeviceRecord toDevice(const juce::var& v);
juce::var fromCalibration(const CalibRecord& c);
CalibRecord toCalibration(const juce::var& v);
} // namespace ni::plugin::json
