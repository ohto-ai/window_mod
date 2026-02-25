/**
 * launcher_main.cpp  –  wda_launcher.exe
 *
 * Minimal Win32 CLI injector helper.  Used by window_mod.exe when the target
 * process is a different CPU architecture (e.g. the x64 main exe needs to
 * inject into a 32-bit WOW64 process, so it spawns the x86 launcher and vice
 * versa).
 *
 * Usage:
 *   wda_launcher_<arch>.exe  <pid>  <dll_path>          – inject (unload first if loaded)
 *   wda_launcher_<arch>.exe  <pid>  <dll_path>  unload  – unload only
 *
 * Exit code: 0 = success, 1 = any failure.
 *
 * The shared-memory block (WdaInjectHwnd_WindowMod) is written by the main
 * process BEFORE spawning the launcher, so the injected DLL will find the
 * correct HWND and affinity values already in place.
 */

#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Find the HMODULE for a DLL loaded in hProcess by filename (case-insensitive).
static HMODULE FindRemoteDll(HANDLE hProcess, const wchar_t* dllFilename)
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
        // Compare filename part only.
        const wchar_t* slash = wcsrchr(name, L'\\');
        const wchar_t* base  = slash ? slash + 1 : name;
        if (_wcsicmp(base, dllFilename) == 0)
            return hMod;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Inject a FreeLibrary remote thread call into hProcess to unload hMod.
static void RemoteFreeLibrary(HANDLE hProcess, HMODULE hMod)
{
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) return;
    auto pfnFreeLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(hK32, "FreeLibrary"));
    if (!pfnFreeLib) return;

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        pfnFreeLib,
                                        reinterpret_cast<LPVOID>(hMod),
                                        0, nullptr);
    if (!hThread) return;
    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
}

// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3)
        return 1;

    // argv[1] = PID (decimal)
    DWORD pid = static_cast<DWORD>(_wtol(argv[1]));
    if (!pid)
        return 1;

    // argv[2] = DLL path (may contain spaces; passed as a single quoted argument)
    const wchar_t* dllPath = argv[2];

    // argv[3] (optional) = "unload"  →  unload-only mode
    bool unloadOnly = (argc >= 4 && _wcsicmp(argv[3], L"unload") == 0);

    // Extract filename from the full path for FindRemoteDll.
    const wchar_t* slash    = wcsrchr(dllPath, L'\\');
    const wchar_t* dllName  = slash ? slash + 1 : dllPath;

    // Open the target process.
    DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    if (!unloadOnly)
        access |= PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE;
    else
        access |= PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION;

    HANDLE hProcess = OpenProcess(access, FALSE, pid);
    if (!hProcess)
        return 1;

    int result = 0; // assume success for unload path

    // Always unload any existing copy first (in inject mode this ensures DllMain
    // is called fresh; in unload-only mode this is the entire operation).
    HMODULE hExisting = FindRemoteDll(hProcess, dllName);
    if (hExisting)
        RemoteFreeLibrary(hProcess, hExisting);

    if (!unloadOnly) {
        result = 1; // need successful load to claim success

        do {
            const size_t pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);

            LPVOID pRemote = VirtualAllocEx(hProcess, nullptr, pathBytes,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!pRemote) break;

            if (!WriteProcessMemory(hProcess, pRemote, dllPath, pathBytes, nullptr)) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
            if (!hK32) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }
            auto pfnLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(hK32, "LoadLibraryW"));

            HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                                pfnLoadLib, pRemote, 0, nullptr);
            if (!hThread) {
                VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);
                break;
            }

            WaitForSingleObject(hThread, 10000);

            DWORD exitCode = 0;
            GetExitCodeThread(hThread, &exitCode);
            CloseHandle(hThread);
            VirtualFreeEx(hProcess, pRemote, 0, MEM_RELEASE);

            result = (exitCode != 0) ? 0 : 1;
        } while (false);
    }

    CloseHandle(hProcess);
    return result;
}
