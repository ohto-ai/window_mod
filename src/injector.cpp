#include "injector.h"
#include <string>
#include <vector>
#include <filesystem>
#include <psapi.h>

// Named shared-memory object used to pass the target HWND and desired affinity
// to the injected DLL.  The same name/layout is referenced in inject_dll/dllmain.cpp.
#define WDA_SHARED_MEM_NAME  L"Local\\WdaInjectHwnd_WindowMod"

// Layout of the shared-memory block (must match dllmain.cpp).
struct WdaSharedData {
    HWND  hwnd;
    DWORD affinity;
};

static bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// ---------------------------------------------------------------------------
// Helper: create a shared-memory object and write the payload.
// Returns INVALID_HANDLE_VALUE on failure.
static HANDLE CreateSharedData(HWND hwnd, DWORD affinity)
{
    HANDLE hMap = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(WdaSharedData)),
        WDA_SHARED_MEM_NAME);
    if (!hMap)
        return INVALID_HANDLE_VALUE;

    void* pView = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(WdaSharedData));
    if (!pView) {
        CloseHandle(hMap);
        return INVALID_HANDLE_VALUE;
    }
    auto* pData = reinterpret_cast<WdaSharedData*>(pView);
    pData->hwnd     = hwnd;
    pData->affinity = affinity;
    UnmapViewOfFile(pView);
    return hMap;
}

// ---------------------------------------------------------------------------
// Helper: scan target process module list for our DLL (case-insensitive on
// the filename part only) and return its remote HMODULE, or nullptr.
// Requires hProcess to have PROCESS_QUERY_INFORMATION | PROCESS_VM_READ.
static HMODULE FindRemoteDll(HANDLE hProcess, const std::wstring& dllFilename)
{
    // First call to get required buffer size, then allocate dynamically.
    DWORD needed = 0;
    EnumProcessModules(hProcess, nullptr, 0, &needed);
    if (!needed) return nullptr;

    std::vector<HMODULE> mods(needed / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, mods.data(),
                            static_cast<DWORD>(mods.size() * sizeof(HMODULE)),
                            &needed))
        return nullptr;

    DWORD count = needed / sizeof(HMODULE);
    if (count < static_cast<DWORD>(mods.size())) mods.resize(count);

    for (HMODULE hMod : mods) {
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleFileNameExW(hProcess, hMod, name, MAX_PATH))
            continue;
        std::wstring modName(name);
        auto pos = modName.rfind(L'\\');
        if (pos != std::wstring::npos)
            modName = modName.substr(pos + 1);
        if (_wcsicmp(modName.c_str(), dllFilename.c_str()) == 0)
            return hMod;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Helper: inject a FreeLibrary call into the target process to unload hMod.
// DllMain(DLL_PROCESS_DETACH) will run in the target as the refcount reaches
// zero, which is harmless for our inject DLL. After this, a fresh LoadLibrary
// will trigger DllMain(DLL_PROCESS_ATTACH) again with the new shared-memory
// payload.
static void RemoteFreeLibrary(HANDLE hProcess, HMODULE hMod)
{
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) return;
    auto pfnFreeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hK32, "FreeLibrary"));
    if (!pfnFreeLib) return;

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0,
        pfnFreeLib, reinterpret_cast<LPVOID>(hMod), 0, nullptr);
    if (!hThread) return;
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
}

// ---------------------------------------------------------------------------
bool InjectWDASetAffinity(HWND hwnd, DWORD affinity)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    // --- 1. Determine DLL path (same directory as the running executable) ---
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::filesystem::path dllFsPath =
        std::filesystem::path(exePath).parent_path() / L"wda_inject.dll";
    std::wstring dllPath = dllFsPath.wstring();

    if (!FileExists(dllPath)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // --- 2. Put the target HWND + affinity in named shared memory ----------
    HANDLE hMap = CreateSharedData(hwnd, affinity);
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
            // --- 4. If the DLL is already loaded, FreeLibrary it first so
            //        the upcoming LoadLibraryW triggers a fresh DllMain. ---
            HMODULE hRemote = FindRemoteDll(hProcess,
                                            dllFsPath.filename().wstring());
            if (hRemote)
                RemoteFreeLibrary(hProcess, hRemote);

            // --- 5. Write the DLL path into the target process memory -----
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

            // --- 6. Spawn a remote thread that calls LoadLibraryW ----------
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
