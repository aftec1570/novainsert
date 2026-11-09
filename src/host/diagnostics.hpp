// DiagLog — opt-in diagnostics CSV (F1 §21 diagnostics). Off unless settings.json has "diagnostics": true.
//   message thread (and other non-RT threads): text() writes a formatted row directly under a mutex to
//   <dir>/f3-plugin-p<pid>.csv. Never called from the hook or processBlock (RT rule: no IO/formatting).
// Row format: t,kind,inst,<text>  (the header names v0..v3 for the historic fixed-column rows the harness still accepts)
#pragma once
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include "host/platform.hpp"

namespace ni::host
{


class DiagLog
{
public:
    static DiagLog& get();

    // Message thread. Opens <dir>/f3-plugin-p<pid>.csv and starts the writer; idempotent.
    void enable(const std::string& dir);
    void disable();
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Non-RT threads only: formatted text row written immediately.
    void text(const char* kind, int inst, const char* fmt, ...) noexcept NI_PRINTF_LIKE(4, 5);

    static double now() noexcept;   // monotonic seconds

    // Reads "diagnostics": true / "diagnosticsDir": "<path>" / "maxConcurrentProbes" from settings.json (key scan, no parser).
    // maxConcurrentProbes 0 = unlimited (F1 §11); allowAnyDriver lets an unsupported driver through (test setups only)
    struct Settings { bool diagnostics = false; std::string dir; int maxConcurrentProbes = 0; bool allowAnyDriver = false; };
    static Settings probeSettings(const std::string& settingsPath);

private:
    DiagLog() = default;
    ~DiagLog();

    std::atomic<bool> enabled_ { false };
    std::mutex fileMutex_;
    std::FILE* fp_ = nullptr;
};

} // namespace ni::host
