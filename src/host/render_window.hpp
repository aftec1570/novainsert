// Render-dialog presence for the RenderArbiter (F1 §10 Q14 (b)): the message-thread 250 ms poll restores the remembered
// render speed once REAPER's "Render to File" window is gone. The dialog is found by identity — the window that appears
// across action 40015 — with the title as a fallback (renderTitleMatches: English, or the LangPack title when set), so a
// translated title still counts; a hidden or minimised dialog is still open (S6 review C1). Message thread only.
// macOS: AppKit (render_window_mac.mm). Windows: Win32; Linux: SWELL through SWELL_dllMain (render_window_win.cpp), with
// the spec §5.6 fallback (dialog treated as closed once 40015 returns) when SWELL's window functions are unavailable.
#pragma once
#include <string>

namespace ni::host::platform
{
void renderWindowBeforeDialog();   // 40015 pre-hook: remember the windows that exist now
void renderWindowAfterDialog();    // 40015 post-hook: the window that appeared is the render dialog
bool renderWindowOpen();
void renderWindowSetTitle(std::string langpackTitle);   // the translated dialog title, when known ("" = English only)
}
