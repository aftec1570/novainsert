#include "host/diagnostics.hpp"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ni::host
{

DiagLog& DiagLog::get()
{
    static DiagLog* instance = new DiagLog();   // never destroyed: hook threads may still push during static teardown
    return *instance;
}

DiagLog::~DiagLog() { disable(); }

double DiagLog::now() noexcept
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void DiagLog::enable(const std::string& dir)
{
    std::lock_guard<std::mutex> l(fileMutex_);
    if (fp_ != nullptr) return;
    const std::string path = dir + "/f3-plugin-p" + std::to_string(processId()) + ".csv";
    fp_ = std::fopen(path.c_str(), "a");
    if (fp_ == nullptr) return;
    if (std::ftell(fp_) == 0) std::fprintf(fp_, "t,kind,inst,v0,v1,v2,v3\n");   // re-enable after the last release appends: no second header
    std::fflush(fp_);
    enabled_.store(true, std::memory_order_release);
}

void DiagLog::disable()
{
    if (!enabled_.exchange(false)) return;
    std::lock_guard<std::mutex> l(fileMutex_);
    if (fp_ != nullptr) { std::fclose(fp_); fp_ = nullptr; }
}

void DiagLog::text(const char* kind, int inst, const char* fmt, ...) noexcept
{
    if (!enabled_.load(std::memory_order_acquire)) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(fileMutex_);
    if (fp_ == nullptr) return;
    std::fprintf(fp_, "%.6f,%s,%d,%s\n", now(), kind, inst, buf);
    std::fflush(fp_);
}

DiagLog::Settings DiagLog::probeSettings(const std::string& settingsPath)
{
    Settings s;
    std::ifstream in(settingsPath);
    if (!in) return s;
    std::stringstream ss; ss << in.rdbuf();
    const std::string j = ss.str();
    auto valueAfter = [&](const char* key) -> std::string {
        const auto k = j.find(std::string("\"") + key + "\"");
        if (k == std::string::npos) return {};
        const auto colon = j.find(':', k);
        if (colon == std::string::npos) return {};
        auto p = j.find_first_not_of(" \t\r\n", colon + 1);
        if (p == std::string::npos) return {};
        if (j[p] == '"') { const auto q = j.find('"', p + 1); return q == std::string::npos ? std::string() : j.substr(p + 1, q - p - 1); }
        const auto e = j.find_first_of(",}\r\n \t", p);
        return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };
    s.diagnostics = valueAfter("diagnostics") == "true";
    s.allowAnyDriver = valueAfter("allowAnyDriver") == "true";
    s.dir = valueAfter("diagnosticsDir");
    const std::string mc = valueAfter("maxConcurrentProbes");
    if (!mc.empty()) s.maxConcurrentProbes = std::max(0, std::atoi(mc.c_str()));
    return s;
}

} // namespace ni::host
