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
    GetWindowTextW(hwnd, title, 256);
    if (title[0] == L'\0')
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    ctx->list->push_back({ hwnd, title, GetProcessName(pid), pid });
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
