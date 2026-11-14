# P0.4 – SWELL window API under REAPER Linux (2026-11-09)

**Verdict: GO.** The Linux branch of `render_window_win.cpp` can detect the Render to File dialog the same way as
Windows; the §5.6 fallback is not needed (it stays in the code for a null pointer).

Run: throwaway REAPER extension (`spikes/swell_probe/`, not committed) built with `g++ -shared -fPIC` in the
`novainsert-linux` container, REAPER 7.85 x86_64 under Xvfb, Dummy Audio, a ReaScript calling `Main_OnCommand(40015)`.

## How an extension gets SWELL functions

Not through `reaper_plugin_info_t::GetFunc` (all window names return null there). REAPER calls an exported

    extern "C" int SWELL_dllMain(HINSTANCE, DWORD callMode, LPVOID getFunc)   // callMode 1 = DLL_PROCESS_ATTACH

before `ReaperPluginEntry`; `getFunc` is `void* (*)(const char* name)` (the `SWELLAPI_GetFunc` of
`swell-modstub-generic.cpp`). NovaInsert is a VST3, not an extension: REAPER's VST host calls `SWELL_dllMain` in a
VST's module too when exported (same mechanism as for swell-based plug-ins), so the plugin exports it from
`src/host/render_window_win.cpp` on Linux and keeps the pointer. **Task 8 must confirm this for the VST3 `.so`**; if
REAPER does not call it for VST3 modules, the code takes the fallback and logs `RENDERWIN fallback`.

## Exported names (via SWELL `getFunc`)

| Name | Present | Note |
|---|---|---|
| `EnumWindows` | yes | `BOOL EnumWindows(BOOL (*)(HWND, LPARAM), LPARAM)`, top-level windows |
| `GetDlgItemText` | yes | SWELL's `GetWindowText(h, t, n)` is a macro for `GetDlgItemText(h, 0, t, n)`; `GetWindowText`/`GetWindowTextW` are **not** exported |
| `IsWindowVisible` | yes | |
| `GetParent` | yes | the render dialog's parent is the main window |
| `IsWindow` | yes | |
| `IsIconic` | **no** | SWELL has no minimise: "open" = still a window and visible |
| `FindWindowEx`, `EnumChildWindows`, `GetWindowLong` | yes | not needed |

## Observed windows

- Main window `REAPER v7.85 - EVALUATION LICENSE` (parent null), the evaluation `About REAPER v7.85/linux-x86_64 …`
  nag (parent = main) – both exist **before** 40015, so identity-first detection ignores them.
- After `Main_OnCommand(40015)`: a new visible window titled exactly **`Render to File`**, parent = main window.
  40015 returns immediately (the dialog is modeless on Linux, as on macOS).
- Titles come back as UTF-8 (SWELL is UTF-8 throughout); the LangPack title check uses the same matcher.
