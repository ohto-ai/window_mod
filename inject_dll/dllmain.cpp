/**
 * dllmain.cpp  –  wda_inject.dll
 *
 * This DLL is injected into a target process by window_mod.exe.
 * On DLL_PROCESS_ATTACH it reads the target HWND from a named shared-memory
 * object (written by the injector before injection), then calls
 *   SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)
 * from within the target process (the only process that is allowed to make
 * this call for its own windows).
 *
 * Requires Windows 10 version 2004 (build 19041) or later for
 * WDA_EXCLUDEFROMCAPTURE.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Same name as in src/injector.cpp
#define WDA_SHARED_MEM_NAME  L"Local\\WdaInjectHwnd_WindowMod"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID /*lpReserved*/)
{
    if (ulReason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(hModule);

    // Open the shared memory created by the injector.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, WDA_SHARED_MEM_NAME);
    if (!hMap)
        return TRUE; // nothing we can do; return TRUE so the DLL loads

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(HWND));
    if (!pView) {
        CloseHandle(hMap);
        return TRUE;
    }

    HWND hwnd = *reinterpret_cast<const HWND*>(pView);
    UnmapViewOfFile(pView);
    CloseHandle(hMap);

    if (hwnd && IsWindow(hwnd))
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    return TRUE;
}
