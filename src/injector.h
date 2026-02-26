#pragma once

#include <windows.h>
#include "window_ops.h"

/// Inject wda_inject.dll into the process that owns `hwnd` and call
/// SetWindowDisplayAffinity(hwnd, affinity) from within that process
/// (required because the API only succeeds for the owning process).
///
/// affinity: WDA_NONE (0x00000000) to remove, or
///           WDA_EXCLUDEFROMCAPTURE (0x00000011) to exclude from capture.
/// autoUnload: if true (default), FreeLibrary the DLL after the affinity call
///             so that it does not remain loaded in the target process.
/// The DLL (and same-arch launcher) must be placed next to the executable.
/// Returns true if the injection and the affinity call succeeded.
bool InjectWDASetAffinity(HWND hwnd, DWORD affinity, bool autoUnload = true);

/// Unload wda_inject.dll from the process that owns `hwnd` (if loaded).
/// Useful for cleaning up DLLs left by a previous session or when auto-unload
/// was disabled.  Returns true if the DLL was found and successfully unloaded
/// (or was not loaded at all).
bool UnloadInjectedDll(HWND hwnd);

/// Convenience wrapper – sets WDA_EXCLUDEFROMCAPTURE.
inline bool InjectWDAExcludeFromCapture(HWND hwnd, bool autoUnload = true)
{
    return InjectWDASetAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE, autoUnload);
}
