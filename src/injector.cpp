#include "injector.h"
#include <string>
#include <vector>
#include <filesystem>
#include <psapi.h>
#include <spdlog/spdlog.h>

// Named shared-memory object used to pass the target HWND and desired affinity
// to the injected DLL.  The same name/layout is referenced in inject_dll/dllmain.cpp.
#define WDA_SHARED_MEM_NAME  L"Local\\WdaInjectHwnd_WindowMod"

// Layout of the shared-memory block (must match dllmain.cpp).
struct WdaSharedData {
    HWND  hwnd;
    DWORD affinity;
};

// ---------------------------------------------------------------------------
// Narrow (UTF-8) representation of a wide string – used for spdlog messages.
static std::string WtoU8(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                 nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return {};
    std::string result(static_cast<size_t>(sz) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                        &result[0], sz, nullptr, nullptr);
    return result;
}

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
// Helper: check whether the current process and the target process have the
// same CPU architecture (both native 64-bit, or both 32-bit).
// Returns true when they are compatible; false (and sets LastError to
// ERROR_EXE_MACHINE_TYPE_MISMATCH) when they differ.
static bool CheckArchitectureMatch(HANDLE hProcess, DWORD pid)
{
#ifdef _WIN64
    // We are a native 64-bit process.
    // A WOW64 (32-bit) target is incompatible: a 64-bit DLL cannot be loaded
    // into a 32-bit process and CreateRemoteThread with a 64-bit address fails.
    BOOL targetIsWow64 = FALSE;
    if (IsWow64Process(hProcess, &targetIsWow64) && targetIsWow64) {
        spdlog::error(
            "InjectWDASetAffinity: architecture mismatch for PID {}. "
            "The injector is 64-bit but the target process is 32-bit (WOW64). "
            "Use the 32-bit (x86) build of window_mod to inject into 32-bit targets.",
            pid);
        SetLastError(ERROR_EXE_MACHINE_TYPE_MISMATCH);
        return false;
    }
#else
    // We are a 32-bit process.  On a 64-bit OS we run under WOW64.
    // A native 64-bit target is incompatible for the same reason.
    BOOL selfIsWow64   = FALSE;
    BOOL targetIsWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfIsWow64);
    IsWow64Process(hProcess, &targetIsWow64);
    if (selfIsWow64 && !targetIsWow64) {
        spdlog::error(
            "InjectWDASetAffinity: architecture mismatch for PID {}. "
            "The injector is 32-bit (WOW64) but the target process is native 64-bit. "
            "Use the 64-bit (x64) build of window_mod to inject into 64-bit targets.",
            pid);
        SetLastError(ERROR_EXE_MACHINE_TYPE_MISMATCH);
        return false;
    }
#endif
    return true;
}

// ---------------------------------------------------------------------------
bool InjectWDASetAffinity(HWND hwnd, DWORD affinity)
{
    if (!hwnd || !IsWindow(hwnd)) {
        spdlog::warn("InjectWDASetAffinity: invalid HWND {:#x}", reinterpret_cast<uintptr_t>(hwnd));
        return false;
    }

    spdlog::info("InjectWDASetAffinity: hwnd={:#x}, affinity={:#x}",
                 reinterpret_cast<uintptr_t>(hwnd), affinity);

    // --- 1. Determine DLL path (same directory as the running executable) ---
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::filesystem::path dllFsPath =
        std::filesystem::path(exePath).parent_path() / L"wda_inject.dll";
    std::wstring dllPath = dllFsPath.wstring();

    spdlog::debug("InjectWDASetAffinity: DLL path = {}", WtoU8(dllPath));

    if (!FileExists(dllPath)) {
        spdlog::error("InjectWDASetAffinity: DLL not found at {}", WtoU8(dllPath));
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // --- 2. Put the target HWND + affinity in named shared memory ----------
    HANDLE hMap = CreateSharedData(hwnd, affinity);
    if (hMap == INVALID_HANDLE_VALUE) {
        spdlog::error("InjectWDASetAffinity: CreateSharedData failed (error {})", GetLastError());
        return false;
    }

    bool success = false;

    do {
        // --- 3. Open target process with the rights needed for injection ---
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
            spdlog::error("InjectWDASetAffinity: GetWindowThreadProcessId returned pid=0 (error {})",
                          GetLastError());
            break;
        }

        spdlog::info("InjectWDASetAffinity: target PID = {}", pid);

        HANDLE hProcess = OpenProcess(
            PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
            FALSE, pid);
        if (!hProcess) {
            spdlog::error("InjectWDASetAffinity: OpenProcess failed for PID {} (error {})",
                          pid, GetLastError());
            break;
        }

        do {
            // --- 4. Check for CPU architecture mismatch (32-bit vs 64-bit) -
            if (!CheckArchitectureMatch(hProcess, pid))
                break;

            // --- 5. If the DLL is already loaded, FreeLibrary it first so
            //        the upcoming LoadLibraryW triggers a fresh DllMain. ---
            HMODULE hRemote = FindRemoteDll(hProcess,
                                            dllFsPath.filename().wstring());
            if (hRemote) {
                spdlog::debug("InjectWDASetAffinity: DLL already loaded in PID {}; unloading first", pid);
                RemoteFreeLibrary(hProcess, hRemote);
            }

            // --- 6. Write the DLL path into the target process memory -----
            const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
            LPVOID pRemote = VirtualAllocEx(
                hProcess, nullptr, pathBytes,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!pRemote) {
                spdlog::error("InjectWDASetAffinity: VirtualAllocEx failed for PID {} (error {})",
                              pid, GetLastError());
                break;
            }

            if (!WriteProcessMemory(hProcess, pRemote,
                                    dllPath.c_str(), pathBytes, nullptr)) {
                spdlog::error("InjectWDASetAffinity: WriteProcessMemory failed for PID {} (error {})",
                              pid, GetLastError());
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            // --- 7. Spawn a remote thread that calls LoadLibraryW ----------
            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (!hK32) {
                spdlog::error("InjectWDASetAffinity: GetModuleHandleW(kernel32.dll) failed (error {})",
                              GetLastError());
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
                spdlog::error("InjectWDASetAffinity: CreateRemoteThread failed for PID {} (error {})",
                              pid, GetLastError());
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            spdlog::debug("InjectWDASetAffinity: remote thread created for PID {}; waiting…", pid);

            // Wait up to 8 seconds for DllMain to finish.
            DWORD waitResult = WaitForSingleObject(hThread, 8000);
            if (waitResult != WAIT_OBJECT_0) {
                spdlog::warn("InjectWDASetAffinity: remote thread wait returned {} for PID {}",
                             waitResult, pid);
            }

            // Check that LoadLibraryW succeeded (non-zero HMODULE as exit code).
            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode);
            CloseHandle(hThread);
            VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);

            if (exitCode == 0) {
                spdlog::error("InjectWDASetAffinity: LoadLibraryW returned NULL in PID {} – "
                              "DLL failed to load (architecture mismatch, missing dependency, "
                              "or anti-cheat/AV blocked the injection).",
                              pid);
                break;
            }

            spdlog::debug("InjectWDASetAffinity: DLL loaded in PID {} (HMODULE={:#x})", pid, exitCode);

            // --- 8. Verify via GetWindowDisplayAffinity that the API call
            //        inside the DLL actually succeeded. ----------------------
            typedef BOOL(WINAPI* PFN_GWDA)(HWND, DWORD*);
            static PFN_GWDA pfnGetWDA = reinterpret_cast<PFN_GWDA>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowDisplayAffinity"));

            if (pfnGetWDA) {
                DWORD actualAffinity = 0xFFFFFFFF;
                if (pfnGetWDA(hwnd, &actualAffinity)) {
                    success = (actualAffinity == affinity);
                    if (success) {
                        spdlog::info("InjectWDASetAffinity: verified – affinity is now {:#x}", actualAffinity);
                    } else {
                        spdlog::error("InjectWDASetAffinity: DLL loaded but affinity mismatch "
                                      "for HWND {:#x}: expected {:#x}, got {:#x} – "
                                      "SetWindowDisplayAffinity may have failed inside the target process.",
                                      reinterpret_cast<uintptr_t>(hwnd), affinity, actualAffinity);
                    }
                } else {
                    // GetWindowDisplayAffinity failed; fall back to trusting the exit code.
                    spdlog::warn("InjectWDASetAffinity: GetWindowDisplayAffinity failed (error {}); "
                                 "assuming success based on DLL load.",
                                 GetLastError());
                    success = true;
                }
            } else {
                // API not available (pre-Win7?); trust the exit code.
                success = true;
            }
        } while (false);

        CloseHandle(hProcess);
    } while (false);

    CloseHandle(hMap);

    if (success)
        spdlog::info("InjectWDASetAffinity: SUCCESS for HWND {:#x}", reinterpret_cast<uintptr_t>(hwnd));
    else
        spdlog::error("InjectWDASetAffinity: FAILED for HWND {:#x}", reinterpret_cast<uintptr_t>(hwnd));

    return success;
}
