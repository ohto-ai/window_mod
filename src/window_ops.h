#pragma once

#include <windows.h>

/// Set or remove the TOPMOST flag on a window.
bool SetWindowTopMost(HWND hwnd, bool topMost);

/// Hide a window (SW_HIDE).
bool HideWindow(HWND hwnd);

/// Restore a hidden window (SW_SHOW + bring to foreground).
bool ShowWindowRestore(HWND hwnd);
