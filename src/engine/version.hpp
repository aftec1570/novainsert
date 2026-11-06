// NovaInsert engine core — version constants. Header-only, dependency-free (ni::engine).
#pragma once
#include <string_view>

namespace ni::engine
{
inline constexpr std::string_view kVersion = "0.1.0";
// Calibration algorithm version: part of CalibKey (F1 §4). Bump whenever probe/correlator/lock semantics change.
inline constexpr int kCalibAlgorithmVersion = 1;
} // namespace ni::engine
