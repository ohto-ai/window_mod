/**
 * dllmain.cpp  –  wda_inject.dll
 *
 * This DLL is injected into a target process by window_mod.exe.
 * On DLL_PROCESS_ATTACH it reads the target HWND and desired affinity from a
 * named shared-memory object (written by the injector before injection), then
 * calls SetWindowDisplayAffinity(hwnd, affinity) from within the target process
 * (the only process that is allowed to make this call for its own windows).
 *
 * Requires Windows 10 version 2004 (build 19041) or later for
 * WDA_EXCLUDEFROMCAPTURE.
 */

#include <windows.h>

// Same name as in src/injector.cpp
#define WDA_SHARED_MEM_NAME  L"Local\\WdaInjectHwnd_WindowMod"

#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Layout of the shared-memory block (must match src/injector.cpp).
struct WdaSharedData {
    HWND  hwnd;
    DWORD affinity;
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReason, LPVOID /*lpReserved*/)
{
    if (ulReason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(hModule);

    // Open the shared memory created by the injector.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, WDA_SHARED_MEM_NAME);
    if (!hMap) {
        OutputDebugStringA("wda_inject: OpenFileMappingW failed – shared memory not found.\n");
        return TRUE; // nothing we can do; return TRUE so the DLL loads
    }

    const void* pView = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(WdaSharedData));
    if (!pView) {
        OutputDebugStringA("wda_inject: MapViewOfFile failed.\n");
        CloseHandle(hMap);
        return TRUE;
    }

    const auto* pData = reinterpret_cast<const WdaSharedData*>(pView);
    HWND  hwnd     = pData->hwnd;
    DWORD affinity = pData->affinity;
    UnmapViewOfFile(pView);
    CloseHandle(hMap);

    if (!hwnd || !IsWindow(hwnd)) {
        OutputDebugStringA("wda_inject: HWND is invalid.\n");
        return TRUE;
    }

    BOOL ok = SetWindowDisplayAffinity(hwnd, affinity);
    if (ok) {
        OutputDebugStringA("wda_inject: SetWindowDisplayAffinity succeeded.\n");
    } else {
        char buf[128];
        wsprintfA(buf, "wda_inject: SetWindowDisplayAffinity failed (error %lu).\n",
                  GetLastError());
        OutputDebugStringA(buf);
    }

    return TRUE;
}
