// Render-dialog detection on Windows (Win32) and Linux (SWELL): see render_window.hpp. Same identity-first logic as the
// AppKit version. Linux: REAPER hands SWELL's function table to a module through its exported SWELL_dllMain (P0.4,
// docs/port/p0-4-swell-windows.md); reaper_plugin_info_t::GetFunc does not resolve the window functions. Without them the
// spec §5.6 fallback applies: the dialog counts as closed once action 40015 has returned (logged once as RENDERWIN).
#include "host/render_window.hpp"
#include "host/render_window_title.hpp"
#include "host/diagnostics.hpp"
#include "host/reaper_types.hpp"   // HWND (Win32 <windows.h>, or the SWELL-compatible typedef)

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ni::host::platform
{
namespace
{
struct TopWindow { HWND h; bool visible; };

#ifdef _WIN32
// ---- Win32: this process's top-level windows -------------------------------------------------------------------------
BOOL CALLBACK collect (HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId (h, &pid);
    if (pid == GetCurrentProcessId()) reinterpret_cast<std::vector<TopWindow>*> (lp)->push_back ({ h, IsWindowVisible (h) != FALSE });
    return TRUE;
}
bool available() { return true; }
std::vector<TopWindow> topLevel()
{
    std::vector<TopWindow> out;
    EnumWindows (&collect, reinterpret_cast<LPARAM> (&out));
    return out;
}
std::string titleOf (HWND h)
{
    wchar_t w[256] = {};
    const int n = GetWindowTextW (h, w, 255);
    if (n <= 0) return {};
    char u[1024] = {};
    const int m = WideCharToMultiByte (CP_UTF8, 0, w, n, u, static_cast<int> (sizeof u) - 1, nullptr, nullptr);
    return m > 0 ? std::string (u, static_cast<size_t> (m)) : std::string();
}
bool alive (HWND h) { return IsWindow (h) != FALSE; }
bool present (HWND h) { return IsWindowVisible (h) != FALSE || IsIconic (h) != FALSE; }   // shown or minimised
#else
// ---- SWELL (Linux): function table from SWELL_dllMain ----------------------------------------------------------------
using SwellBool = int;   // swell-types.h: BOOL is int off Apple
using EnumProc = SwellBool (*) (HWND, intptr_t);
struct Swell
{
    SwellBool (*EnumWindows) (EnumProc, intptr_t) = nullptr;
    SwellBool (*GetDlgItemText) (HWND, int, char*, int) = nullptr;   // GetWindowText(h,…) is GetDlgItemText(h,0,…) in SWELL
    SwellBool (*IsWindowVisible) (HWND) = nullptr;
    SwellBool (*IsWindow) (HWND) = nullptr;
};
Swell g_swell;

template <typename F> void resolve (void* (*getFunc) (const char*), F& f, const char* name)
{
    f = reinterpret_cast<F> (getFunc (name));
}
SwellBool collect (HWND h, intptr_t lp)
{
    reinterpret_cast<std::vector<TopWindow>*> (lp)->push_back ({ h, g_swell.IsWindowVisible (h) != 0 });
    return 1;
}
bool available() { return g_swell.EnumWindows && g_swell.GetDlgItemText && g_swell.IsWindowVisible && g_swell.IsWindow; }
std::vector<TopWindow> topLevel()
{
    std::vector<TopWindow> out;
    g_swell.EnumWindows (&collect, reinterpret_cast<intptr_t> (&out));
    return out;
}
std::string titleOf (HWND h)
{
    char b[1024] = {};
    g_swell.GetDlgItemText (h, 0, b, static_cast<int> (sizeof b) - 1);
    return b;
}
bool alive (HWND h) { return g_swell.IsWindow (h) != 0; }
bool present (HWND h) { return g_swell.IsWindowVisible (h) != 0; }   // SWELL has no minimise (no IsIconic)
#endif

std::vector<HWND> g_before;     // top-level windows before 40015
HWND g_dialog = nullptr;        // the adopted render dialog, nullptr = none
std::string g_title;            // LangPack title, "" = English only
bool g_fallbackLogged = false;

bool fallback()
{
    if (available()) return false;
    if (! g_fallbackLogged) { g_fallbackLogged = true; DiagLog::get().text ("RENDERWIN", 0, "fallback"); }
    return true;
}
} // namespace

void renderWindowBeforeDialog()
{
    g_before.clear();
    if (fallback()) return;
    for (const auto& w : topLevel()) g_before.push_back (w.h);
}

void renderWindowAfterDialog()
{
    g_dialog = nullptr;
    if (fallback()) { g_before.clear(); return; }
    const auto now = topLevel();
    for (const auto& w : now)
        if (w.visible && std::find (g_before.begin(), g_before.end(), w.h) == g_before.end()) { g_dialog = w.h; break; }
    if (g_dialog == nullptr)
        for (const auto& w : now)
            if (renderTitleMatches (titleOf (w.h), g_title)) { g_dialog = w.h; break; }
    g_before.clear();
}

bool renderWindowOpen()
{
    if (fallback()) return false;   // §5.6: closed as soon as 40015 has returned
    if (g_dialog != nullptr)
    {
        if (alive (g_dialog) && present (g_dialog)) return true;
        g_dialog = nullptr;   // closed (or the handle reused later for another window): forget it
    }
    for (const auto& w : topLevel())
        if (present (w.h) && renderTitleMatches (titleOf (w.h), g_title)) return true;
    return false;
}

void renderWindowSetTitle (std::string langpackTitle) { g_title = std::move (langpackTitle); }
} // namespace ni::host::platform

#ifndef _WIN32
// REAPER calls this in every module it loads that exports it (DLL_PROCESS_ATTACH = 1, DLL_PROCESS_DETACH = 0), passing
// SWELL's resolver. Returning 0 on attach would make REAPER refuse the module, so this always accepts.
extern "C" __attribute__ ((visibility ("default"))) int SWELL_dllMain (void* /*hInst*/, unsigned int callMode, void* getFunc)
{
    using namespace ni::host::platform;
    if (callMode == 1 && getFunc != nullptr)
    {
        auto gf = reinterpret_cast<void* (*) (const char*)> (getFunc);
        resolve (gf, g_swell.EnumWindows, "EnumWindows");
        resolve (gf, g_swell.GetDlgItemText, "GetDlgItemText");
        resolve (gf, g_swell.IsWindowVisible, "IsWindowVisible");
        resolve (gf, g_swell.IsWindow, "IsWindow");
    }
    else if (callMode == 0)
    {
        g_swell = {};
    }
    return 1;
}
#endif
