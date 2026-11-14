// Small platform seam for the host layer: process id and the printf-format attribute. macOS today; the Windows branches are
// the port's entry points (review 2026-11-04 portability) and are compiled on Windows only.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NI_PRINTF_LIKE(fmtIdx, firstArg) __attribute__((format(printf, fmtIdx, firstArg)))
#else
#define NI_PRINTF_LIKE(fmtIdx, firstArg)
#endif

#include <string>

namespace ni::host
{
namespace platform
{
// The one audio driver family each build supports (spec 2026-11-09 §4): REAPER's GetAudioDeviceInfo("MODE") starts with it.
#if defined(__APPLE__)
inline constexpr const char* kSupportedDriver = "CoreAudio";
#elif defined(_WIN32)
inline constexpr const char* kSupportedDriver = "ASIO";
#else
inline constexpr const char* kSupportedDriver = "ALSA";
#endif
// Windows only (user 2026-11-12): ASIO4ALL / FlexASIO count as unsupported, and an unsupported driver bypasses the instance
// (dry pass-through) instead of the silent fallback. macOS and Linux keep the 1.2.0 behaviour.
#if defined(_WIN32)
inline constexpr bool kWindowsDriverRules = true;
#else
inline constexpr bool kWindowsDriverRules = false;
#endif

// Names the calling thread (truncated where the OS limits it: 15 bytes on Linux) and lowers its priority so it never
// competes with audio or UI: QoS utility (macOS), THREAD_PRIORITY_BELOW_NORMAL (Windows), nice +10 (Linux, per thread).
void setLowPriorityWorker(const char* name) noexcept;
// The calling thread's name as the OS reports it; "" where it cannot be read.
std::string currentThreadName();
} // namespace platform

// Exclusive advisory lock on one file for the scope (shared by every process using a library directory): flock on POSIX,
// LockFileEx on Windows. Best effort – a lock that cannot be taken does not stop the caller (same as the flock original).
class FileLock
{
public:
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    bool held() const noexcept { return held_; }
#ifdef _WIN32
    explicit FileLock(const std::string& path) noexcept
    {
        h_ = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { h_ = nullptr; return; }
        OVERLAPPED ov {};
        held_ = LockFileEx(h_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) != 0;
    }
    ~FileLock()
    {
        if (h_ == nullptr) return;
        if (held_) { OVERLAPPED ov {}; UnlockFileEx(h_, 0, 1, 0, &ov); }
        CloseHandle(h_);
    }
private:
    HANDLE h_ = nullptr;
#else
    explicit FileLock(const std::string& path) noexcept
    {
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd_ >= 0) held_ = ::flock(fd_, LOCK_EX) == 0;
    }
    ~FileLock()
    {
        if (fd_ < 0) return;
        if (held_) ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
private:
    int fd_ = -1;
#endif
    bool held_ = false;
};

inline long processId() noexcept
{
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}
} // namespace ni::host
