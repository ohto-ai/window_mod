/**
 * launcher_main.cpp  –  wda_launcher.exe
 *
 * Minimal Win32 CLI injector helper.  Used by window_mod.exe when the target
 * process is a different CPU architecture (e.g. the x64 main exe needs to
 * inject into a 32-bit WOW64 process, so it spawns the x86 launcher and vice
 * versa).
 *
 * Usage:
 *   wda_launcher_<arch>.exe  <pid>  <dll_path>
 *
 * Exit code: 0 = LoadLibraryW returned a non-NULL HMODULE (DLL loaded),
 *            1 = any failure.
 *
 * The shared-memory block (WdaInjectHwnd_WindowMod) is written by the main
 * process BEFORE spawning the launcher, so the injected DLL will find the
 * correct HWND and affinity values already in place.
 */

#include <windows.h>
#include <string>

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3)
        return 1;

    // argv[1] = PID (decimal)
    DWORD pid = static_cast<DWORD>(_wtol(argv[1]));
    if (!pid)
        return 1;

    // argv[2] = DLL path (may contain spaces; passed as a single argument)
    const wchar_t* dllPath = argv[2];

    // Open the target process.
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE |
        PROCESS_VM_READ,
        FALSE, pid);
    if (!hProcess)
        return 1;

    int result = 1; // assume failure

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

    CloseHandle(hProcess);
    return result;
}
