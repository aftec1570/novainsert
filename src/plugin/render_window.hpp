// Render-dialog presence for the RenderArbiter (F1 §10 Q14 (b)): the message-thread 250 ms poll restores the remembered
// render speed once REAPER's "Render to File" window is gone. The F1 ASSUMPTION (GetMainHwnd child enumeration) is
// implemented on macOS as a scan of the application's own windows (REAPER's SWELL dialogs are NSWindows); validated on the
// bench by f3_6 `dialog_close`. The dialog is found by identity — the window that appears across action 40015 — with the
// English title as a fallback, so a LangPack-translated title still counts; a hidden or minimised dialog is still open
// (S6 review C1). Message thread only.
#pragma once

namespace ni::plugin
{
void renderWindowBeforeDialog();   // 40015 pre-hook: remember the windows that exist now
void renderWindowAfterDialog();    // 40015 post-hook: the window that appeared is the render dialog
bool renderWindowOpen();
}
