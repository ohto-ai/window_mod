#include "window_ops.h"

bool SetWindowTopMost(HWND hwnd, bool topMost)
{
    HWND insertAfter = topMost ? HWND_TOPMOST : HWND_NOTOPMOST;
    return SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}

bool HideWindow(HWND hwnd)
{
    return ShowWindow(hwnd, SW_HIDE) != FALSE;
}

bool ShowWindowRestore(HWND hwnd)
{
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    return IsWindowVisible(hwnd) != FALSE;
}
