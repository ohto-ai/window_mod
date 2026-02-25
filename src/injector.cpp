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
// Return the directory that contains the running executable (no trailing backslash).
static std::filesystem::path ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
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
// Helper: scan target process module list for a DLL (case-insensitive filename
// match) and return its remote HMODULE, or nullptr.
// Requires hProcess to have PROCESS_QUERY_INFORMATION | PROCESS_VM_READ.
static HMODULE FindRemoteDll(HANDLE hProcess, const std::wstring& dllFilename)
{
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
// Helper: returns true when the target process is a different CPU bitness from
// the current process.
static bool IsArchMismatch(HANDLE hProcess)
{
#ifdef _WIN64
    BOOL targetIsWow64 = FALSE;
    IsWow64Process(hProcess, &targetIsWow64);
    return (targetIsWow64 != FALSE);
#else
    // 32-bit process on a 64-bit OS runs under WOW64.
    BOOL selfIsWow64 = FALSE;
    BOOL targetIsWow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfIsWow64);
    IsWow64Process(hProcess, &targetIsWow64);
    // Mismatch = we are WOW64 (32-bit on 64-bit OS) and target is NOT WOW64 (native 64-bit).
    return (selfIsWow64 && !targetIsWow64);
#endif
}

// ---------------------------------------------------------------------------
// Helper: inject dllPath into hProcess using a remote LoadLibraryW thread.
// Returns the HMODULE exit code (non-zero = success), or 0 on failure.
static DWORD RemoteLoadLibrary(HANDLE hProcess, const std::wstring& dllPath, DWORD pid)
{
    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID pRemote = VirtualAllocEx(hProcess, nullptr, pathBytes,
                                    MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemote) {
        spdlog::error("RemoteLoadLibrary: VirtualAllocEx failed for PID {} (error {})",
                      pid, GetLastError());
        return 0;
    }

    if (!WriteProcessMemory(hProcess, pRemote, dllPath.c_str(), pathBytes, nullptr)) {
        spdlog::error("RemoteLoadLibrary: WriteProcessMemory failed for PID {} (error {})",
                      pid, GetLastError());
        VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return 0;
    }

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) {
        VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return 0;
    }
    auto pfnLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hK32, "LoadLibraryW"));

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        pfnLoadLib, pRemote, 0, nullptr);
    if (!hThread) {
        spdlog::error("RemoteLoadLibrary: CreateRemoteThread failed for PID {} (error {})",
                      pid, GetLastError());
        VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
        return 0;
    }

    spdlog::debug("RemoteLoadLibrary: remote thread created for PID {}; waiting...", pid);
    DWORD waitRes = WaitForSingleObject(hThread, 8000);
    if (waitRes != WAIT_OBJECT_0)
        spdlog::warn("RemoteLoadLibrary: wait returned {} for PID {}", waitRes, pid);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
    return exitCode;
}

// ---------------------------------------------------------------------------
// Helper: spawn the opposite-arch launcher to inject dllPath into pid.
// The shared memory is already set up by the caller.
// Returns true on success.
static bool SpawnLauncherForPid(DWORD pid, const std::wstring& dllPath)
{
#ifdef _WIN64
    const wchar_t* launcherName = L"wda_launcher_x86.exe";
#else
    const wchar_t* launcherName = L"wda_launcher_x64.exe";
#endif

    std::wstring launcherPath = (ExeDir() / launcherName).wstring();
    if (!FileExists(launcherPath)) {
        spdlog::error("SpawnLauncher: opposite-arch launcher not found at {}",
                      WtoU8(launcherPath));
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // Build command line: "<launcher>" <pid> "<dll_path>"
    std::wstring cmdLine = L"\"" + launcherPath + L"\" "
                         + std::to_wstring(pid)
                         + L" \"" + dllPath + L"\"";

    spdlog::debug("SpawnLauncher: cmd = {}", WtoU8(cmdLine));

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        spdlog::error("SpawnLauncher: CreateProcessW failed (error {})", GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 12000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        spdlog::error("SpawnLauncher: launcher exited with code {} for PID {}", exitCode, pid);
        return false;
    }
    spdlog::debug("SpawnLauncher: launcher succeeded for PID {}", pid);
    return true;
}

// ---------------------------------------------------------------------------
bool InjectWDASetAffinity(HWND hwnd, DWORD affinity, bool autoUnload)
{
    if (!hwnd || !IsWindow(hwnd)) {
        spdlog::warn("InjectWDASetAffinity: invalid HWND {:#x}", reinterpret_cast<uintptr_t>(hwnd));
        return false;
    }

    spdlog::info("InjectWDASetAffinity: hwnd={:#x}, affinity={:#x}, autoUnload={}",
                 reinterpret_cast<uintptr_t>(hwnd), affinity, autoUnload);

    // --- 1. Resolve DLL paths -------------------------------------------------
    std::filesystem::path exeDir = ExeDir();

    // Same-arch DLL (used when architectures match).
#ifdef _WIN64
    const wchar_t* sameDllName = L"wda_inject_x64.dll";
    const wchar_t* oppDllName  = L"wda_inject_x86.dll";
#else
    const wchar_t* sameDllName = L"wda_inject_x86.dll";
    const wchar_t* oppDllName  = L"wda_inject_x64.dll";
#endif

    // Fall back to the legacy name "wda_inject.dll" if the arch-named file is absent.
    std::wstring sameDllPath = (exeDir / sameDllName).wstring();
    if (!FileExists(sameDllPath))
        sameDllPath = (exeDir / L"wda_inject.dll").wstring();

    std::wstring oppDllPath = (exeDir / oppDllName).wstring();

    spdlog::debug("InjectWDASetAffinity: same-arch DLL = {}", WtoU8(sameDllPath));

    if (!FileExists(sameDllPath)) {
        spdlog::error("InjectWDASetAffinity: same-arch DLL not found at {}",
                      WtoU8(sameDllPath));
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }

    // --- 2. Write shared memory (HWND + affinity) ----------------------------
    HANDLE hMap = CreateSharedData(hwnd, affinity);
    if (hMap == INVALID_HANDLE_VALUE) {
        spdlog::error("InjectWDASetAffinity: CreateSharedData failed (error {})",
                      GetLastError());
        return false;
    }

    bool success = false;

    do {
        // --- 3. Identify target process --------------------------------------
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) {
            spdlog::error("InjectWDASetAffinity: GetWindowThreadProcessId returned 0 (error {})",
                          GetLastError());
            break;
        }

        spdlog::info("InjectWDASetAffinity: target PID = {}", pid);

        // --- 4. Open target process ------------------------------------------
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
            // --- 5. Detect architecture mismatch ----------------------------
            bool archMismatch = IsArchMismatch(hProcess);

            if (archMismatch) {
#ifdef _WIN64
                spdlog::info("InjectWDASetAffinity: PID {} is 32-bit (WOW64); "
                             "using opposite-arch launcher + x86 DLL.", pid);
#else
                spdlog::info("InjectWDASetAffinity: PID {} is native 64-bit; "
                             "using opposite-arch launcher + x64 DLL.", pid);
#endif
                // Verify the opposite-arch DLL exists.
                if (!FileExists(oppDllPath)) {
                    spdlog::error("InjectWDASetAffinity: opposite-arch DLL not found at {}",
                                  WtoU8(oppDllPath));
                    SetLastError(ERROR_FILE_NOT_FOUND);
                    break;
                }

                // Spawn the opposite-arch launcher which will LoadLibrary the
                // opposite-arch DLL into the target process.
                // Shared memory (with HWND + affinity) is already populated.
                success = SpawnLauncherForPid(pid, oppDllPath);
                if (!success) break;

                // For auto-unload of the opposite-arch DLL we would need the
                // opposite-arch launcher again (FreeLibrary path); leave for a
                // future enhancement – the DLL is very small and transient.
                // Skip the affinity verification below if the launcher succeeded.
                // (The launcher doesn't support verification natively; we rely
                //  on the DLL having run DllMain successfully.)

                // Best-effort affinity check from this side.
                typedef BOOL(WINAPI* PFN_GWDA)(HWND, DWORD*);
                static PFN_GWDA pfnGetWDA2 = reinterpret_cast<PFN_GWDA>(
                    GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                   "GetWindowDisplayAffinity"));
                if (pfnGetWDA2) {
                    DWORD actual = 0xFFFFFFFF;
                    if (pfnGetWDA2(hwnd, &actual)) {
                        success = (actual == affinity);
                        if (success)
                            spdlog::info("InjectWDASetAffinity: verified (cross-arch) – "
                                         "affinity is now {:#x}", actual);
                        else
                            spdlog::error("InjectWDASetAffinity: cross-arch affinity mismatch: "
                                          "expected {:#x}, got {:#x}", affinity, actual);
                    }
                }
                break; // finished cross-arch path
            }

            // --- 6. Same-arch path: unload stale copy, then inject ----------

            // Unload any previously loaded copy of either DLL variant so the
            // upcoming LoadLibraryW triggers a fresh DllMain.
            for (const wchar_t* n : { sameDllName, L"wda_inject.dll" }) {
                HMODULE hRemote = FindRemoteDll(hProcess, n);
                if (hRemote) {
                    spdlog::debug("InjectWDASetAffinity: unloading stale '{}' from PID {}",
                                  WtoU8(n), pid);
                    RemoteFreeLibrary(hProcess, hRemote);
                }
            }

            // --- 7. Load the same-arch DLL in the target process ------------
            DWORD exitCode = RemoteLoadLibrary(hProcess, sameDllPath, pid);
            if (exitCode == 0) {
                spdlog::error("InjectWDASetAffinity: LoadLibraryW returned NULL in PID {} "
                              "(missing dependency, AV blocked injection?)", pid);
                break;
            }

            spdlog::debug("InjectWDASetAffinity: DLL loaded in PID {} (HMODULE={:#x})",
                          pid, exitCode);

            // --- 8. Verify affinity ------------------------------------------
            typedef BOOL(WINAPI* PFN_GWDA)(HWND, DWORD*);
            static PFN_GWDA pfnGetWDA = reinterpret_cast<PFN_GWDA>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"),
                               "GetWindowDisplayAffinity"));

            if (pfnGetWDA) {
                DWORD actual = 0xFFFFFFFF;
                if (pfnGetWDA(hwnd, &actual)) {
                    success = (actual == affinity);
                    if (success)
                        spdlog::info("InjectWDASetAffinity: verified – affinity is now {:#x}",
                                     actual);
                    else
                        spdlog::error("InjectWDASetAffinity: affinity mismatch: "
                                      "expected {:#x}, got {:#x} – "
                                      "SetWindowDisplayAffinity may have failed inside target.",
                                      affinity, actual);
                } else {
                    spdlog::warn("InjectWDASetAffinity: GetWindowDisplayAffinity failed "
                                 "(error {}); assuming success.", GetLastError());
                    success = true;
                }
            } else {
                success = true; // API not available; trust exit code
            }

            // --- 9. Auto-unload the DLL if requested -------------------------
            if (autoUnload) {
                HMODULE hRemote = FindRemoteDll(hProcess, sameDllName);
                if (!hRemote) hRemote = FindRemoteDll(hProcess, L"wda_inject.dll");
                if (hRemote) {
                    spdlog::debug("InjectWDASetAffinity: auto-unloading DLL from PID {}", pid);
                    RemoteFreeLibrary(hProcess, hRemote);
                }
            }

        } while (false);

        CloseHandle(hProcess);
    } while (false);

    CloseHandle(hMap);

    if (success)
        spdlog::info("InjectWDASetAffinity: SUCCESS for HWND {:#x}",
                     reinterpret_cast<uintptr_t>(hwnd));
    else
        spdlog::error("InjectWDASetAffinity: FAILED for HWND {:#x}",
                      reinterpret_cast<uintptr_t>(hwnd));

    return success;
}

// ---------------------------------------------------------------------------
bool UnloadInjectedDll(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd)) {
        spdlog::warn("UnloadInjectedDll: invalid HWND {:#x}",
                     reinterpret_cast<uintptr_t>(hwnd));
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) {
        spdlog::error("UnloadInjectedDll: GetWindowThreadProcessId returned 0 (error {})",
                      GetLastError());
        return false;
    }

    spdlog::info("UnloadInjectedDll: hwnd={:#x}, PID={}",
                 reinterpret_cast<uintptr_t>(hwnd), pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess) {
        spdlog::error("UnloadInjectedDll: OpenProcess failed for PID {} (error {})",
                      pid, GetLastError());
        return false;
    }

    bool found = false;
    // Try all known DLL names (arch-named and legacy).
    for (const wchar_t* name : { L"wda_inject_x64.dll", L"wda_inject_x86.dll",
                                  L"wda_inject.dll" })
    {
        HMODULE hMod = FindRemoteDll(hProcess, name);
        if (hMod) {
            spdlog::debug("UnloadInjectedDll: found '{}' in PID {}; unloading...",
                          WtoU8(name), pid);
            RemoteFreeLibrary(hProcess, hMod);
            found = true;
        }
    }

    CloseHandle(hProcess);

    if (found)
        spdlog::info("UnloadInjectedDll: unloaded DLL(s) from PID {}", pid);
    else
        spdlog::debug("UnloadInjectedDll: no wda_inject DLL found in PID {}", pid);

    return true; // "success" means the operation ran, even if DLL was already absent
}
