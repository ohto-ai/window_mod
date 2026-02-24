#include "injector.h"
#include <string>
#include <filesystem>

// Named shared-memory object used to pass the target HWND to the injected DLL.
// The same name is referenced in inject_dll/dllmain.cpp.
#define WDA_SHARED_MEM_NAME  L"Local\\WdaInjectHwnd_WindowMod"

static bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// ---------------------------------------------------------------------------
// Helper: create a shared-memory object and write hwnd into it.
// Returns INVALID_HANDLE_VALUE on failure.
static HANDLE CreateSharedHwnd(HWND hwnd)
{
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(HWND)),
        WDA_SHARED_MEM_NAME);
    if (!hMap)
        return INVALID_HANDLE_VALUE;

    void* pView = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(HWND));
    if (!pView) {
        CloseHandle(hMap);
        return INVALID_HANDLE_VALUE;
    }
    *reinterpret_cast<HWND*>(pView) = hwnd;
    UnmapViewOfFile(pView);
    return hMap;
}

// ---------------------------------------------------------------------------
bool InjectWDAExcludeFromCapture(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    // --- 1. Determine DLL path (same directory as the running executable) ---
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring dllPath =
        (std::filesystem::path(exePath).parent_path() / L"wda_inject.dll")
        .wstring();

    if (!FileExists(dllPath)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // --- 2. Put the target HWND in named shared memory --------------------
    HANDLE hMap = CreateSharedHwnd(hwnd);
    if (hMap == INVALID_HANDLE_VALUE)
        return false;

    bool success = false;

    do {
        // --- 3. Open target process with the rights needed for injection ---
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid)
            break;

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
            FALSE, pid);
        if (!hProcess)
            break;

        do {
            // --- 4. Write the DLL path into the target process memory -----
            const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
            LPVOID pRemote = VirtualAllocEx(
                hProcess, nullptr, pathBytes,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!pRemote)
                break;

            if (!WriteProcessMemory(hProcess, pRemote,
                                    dllPath.c_str(), pathBytes, nullptr)) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            // --- 5. Spawn a remote thread that calls LoadLibraryW ----------
            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (!hK32) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }
            auto pfnLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(hK32, "LoadLibraryW"));

            HANDLE hThread = CreateRemoteThread(
                hProcess, nullptr, 0,
                pfnLoadLib, pRemote,
                0, nullptr);

            if (!hThread) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            // Wait up to 8 seconds for DllMain to finish.
            WaitForSingleObject(hThread, 8000);

            // A non-zero exit code means LoadLibraryW returned a valid HMODULE,
            // i.e. DllMain ran successfully (which called SetWindowDisplayAffinity).
            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode);
            success = (exitCode != 0);

            CloseHandle(hThread);
            VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        } while (false);

        CloseHandle(hProcess);
    } while (false);

    CloseHandle(hMap);
    return success;
}

