#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/// Inject wda_inject.dll into the process that owns `hwnd` and call
/// SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) from within
/// that process (required because the API only succeeds for the owning process).
///
/// The DLL must be placed next to the executable.
/// Returns true if the injection and the affinity call succeeded.
bool InjectWDAExcludeFromCapture(HWND hwnd);
