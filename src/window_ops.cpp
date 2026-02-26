#include "window_ops.h"

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

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
    return IsWindowVisible(hwnd) != FALSE;
}

bool IsWindowTopMost(HWND hwnd)
{
    return (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

bool IsWindowExcludeFromCapture(HWND hwnd)
{
    // GetWindowDisplayAffinity is available from Windows 7; use runtime lookup
    // so the binary runs on older SDKs too.
    typedef BOOL(WINAPI* PFN_GWDA)(HWND, DWORD*);
    static PFN_GWDA pfn = reinterpret_cast<PFN_GWDA>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDisplayAffinity"));
    if (!pfn)
        return false;
    DWORD affinity = WDA_NONE;
    pfn(hwnd, &affinity);
    return affinity == WDA_EXCLUDEFROMCAPTURE;
}
