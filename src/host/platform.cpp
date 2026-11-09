#include "host/platform.hpp"

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <vector>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <cstring>
#endif

namespace ni::host::platform
{
#if defined(__APPLE__)
void setLowPriorityWorker(const char* name) noexcept
{
    pthread_setname_np(name);
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}

std::string currentThreadName()
{
    char buf[64] = {};
    if (pthread_getname_np(pthread_self(), buf, sizeof buf) != 0) return {};
    return buf;
}
#elif defined(_WIN32)
void setLowPriorityWorker(const char* name) noexcept
{
    wchar_t wide[64] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 63) == 0) wide[0] = L'\0';
    SetThreadDescription(GetCurrentThread(), wide);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

std::string currentThreadName()
{
    PWSTR wide = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &wide)) || wide == nullptr) return {};
    std::string out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1)
    {
        std::vector<char> buf(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf.data(), n, nullptr, nullptr);
        out.assign(buf.data());
    }
    LocalFree(wide);
    return out;
}
#else
void setLowPriorityWorker(const char* name) noexcept
{
    char buf[16] = {};   // Linux limit: 15 bytes + NUL
    std::strncpy(buf, name, sizeof buf - 1);
    pthread_setname_np(pthread_self(), buf);
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);   // per-thread nice on Linux
}

std::string currentThreadName()
{
    char buf[64] = {};
    if (pthread_getname_np(pthread_self(), buf, sizeof buf) != 0) return {};
    return buf;
}
#endif
} // namespace ni::host::platform
