#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct WindowInfo {
    HWND        hwnd;
    std::wstring title;
    std::wstring processName;
    DWORD       pid;
};

/// Return a snapshot of all visible top-level windows that have a title.
std::vector<WindowInfo> EnumerateWindows();

/// Same, but omit skipHwnd (e.g. our own dialog).
std::vector<WindowInfo> EnumerateWindows(HWND skipHwnd);

/// Return just the filename (e.g. "notepad.exe") for the given process id.
std::wstring GetProcessName(DWORD pid);
