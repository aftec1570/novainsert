// NovaInsert engine core – version constants. Header-only, dependency-free (ni::engine).
#pragma once
#include <string_view>

namespace ni::engine
{
inline constexpr std::string_view kVersion = "1.4beta";
// A pre-release is any version that is not purely digits and dots (e.g. "1.4beta"): the editor footer then says BETA BUILD
// in amber so a beta can never be mistaken for a production release (user 2026-11-15).
inline constexpr bool kIsPreRelease = kVersion.find_first_not_of("0123456789.") != std::string_view::npos;
// Calibration algorithm version: part of CalibKey (F1 §4). Bump whenever probe/correlator/lock semantics change.
inline constexpr int kCalibAlgorithmVersion = 1;
} // namespace ni::engine
