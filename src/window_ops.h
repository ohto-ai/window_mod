#pragma once

#include <windows.h>

/// Set or remove the TOPMOST flag on a window.
bool SetWindowTopMost(HWND hwnd, bool topMost);

/// Hide a window (SW_HIDE).
bool HideWindow(HWND hwnd);

/// Restore a hidden window (SW_SHOW + bring to foreground).
bool ShowWindowRestore(HWND hwnd);

/// Return true if the window has the WS_EX_TOPMOST style.
bool IsWindowTopMost(HWND hwnd);

/// Return true if the window has WDA_EXCLUDEFROMCAPTURE display affinity.
bool IsWindowExcludeFromCapture(HWND hwnd);
