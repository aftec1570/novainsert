// Small platform seam for the host layer: process id and the printf-format attribute. macOS today; the Windows branches are
// the port's entry points (review 2026-11-04 portability) and are compiled on Windows only.
#pragma once
#ifdef _WIN32
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
// Exclusive advisory lock on one file for the scope (shared by every process using a library directory): flock on POSIX,
// LockFileEx on Windows. Best effort — a lock that cannot be taken does not stop the caller (same as the flock original).
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
