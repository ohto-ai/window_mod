#pragma once

#include <windows.h>

/// Inject wda_inject.dll into the process that owns `hwnd` and call
/// SetWindowDisplayAffinity(hwnd, affinity) from within that process
/// (required because the API only succeeds for the owning process).
///
/// affinity: WDA_NONE (0x00000000) to remove, or
///           WDA_EXCLUDEFROMCAPTURE (0x00000011) to exclude from capture.
/// The DLL must be placed next to the executable.
/// Returns true if the injection and the affinity call succeeded.
bool InjectWDASetAffinity(HWND hwnd, DWORD affinity);

/// Convenience wrapper – sets WDA_EXCLUDEFROMCAPTURE.
inline bool InjectWDAExcludeFromCapture(HWND hwnd)
{
    return InjectWDASetAffinity(hwnd, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
}
