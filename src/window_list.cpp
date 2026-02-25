#include "window_list.h"
#include <psapi.h>

std::wstring GetProcessName(DWORD pid)
{
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return L"<unknown>";

    wchar_t path[MAX_PATH] = {};
    DWORD   size = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        CloseHandle(hProcess);
        return L"<unknown>";
    }
    CloseHandle(hProcess);

    // Return only the filename portion.
    std::wstring full(path);
    auto pos = full.rfind(L'\\');
    return (pos != std::wstring::npos) ? full.substr(pos + 1) : full;
}

struct EnumCtx {
    std::vector<WindowInfo>* list;
    HWND                     skipHwnd; // our own dialog – skip it
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<EnumCtx*>(lParam);

    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (hwnd == ctx->skipHwnd)
        return TRUE;

    wchar_t title[256] = {};
    // Use SendMessageTimeoutW so a hung/closing window cannot stall the caller.
    SendMessageTimeoutW(hwnd, WM_GETTEXT, static_cast<WPARAM>(ARRAYSIZE(title)),
                        reinterpret_cast<LPARAM>(title),
                        SMTO_ABORTIFHUNG, 100, nullptr);
    if (title[0] == L'\0')
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    // Try to get the window's small icon (do not destroy – shared handle).
    // Use SendMessageTimeoutW so a hung/closing window cannot stall the caller.
    DWORD_PTR iconResult = 0;
    HICON hIcon = nullptr;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0,
                            SMTO_ABORTIFHUNG, 100, &iconResult) && iconResult)
        hIcon = reinterpret_cast<HICON>(iconResult);
    if (!hIcon) {
        iconResult = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0,
                                SMTO_ABORTIFHUNG, 100, &iconResult) && iconResult)
            hIcon = reinterpret_cast<HICON>(iconResult);
    }
    if (!hIcon)
        hIcon = reinterpret_cast<HICON>(
            GetClassLongPtrW(hwnd, GCLP_HICONSM));

    ctx->list->push_back({ hwnd, title, GetProcessName(pid), pid, hIcon });
    return TRUE;
}

std::vector<WindowInfo> EnumerateWindows(HWND skipHwnd)
{
    std::vector<WindowInfo> windows;
    EnumCtx ctx{ &windows, skipHwnd };
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return windows;
}

std::vector<WindowInfo> EnumerateWindows()
{
    return EnumerateWindows(nullptr);
}
