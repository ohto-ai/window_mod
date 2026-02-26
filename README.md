# window_mod

A Windows-only GUI tool that lets you inspect and modify properties of any
top-level window on the desktop.

---

## Features

| Feature | Description |
|---|---|
| **Dark theme** | Full dark UI using the Catppuccin Mocha palette, rendered via DWM immersive dark mode and custom `WM_CTLCOLOR` handling. |
| **Live desktop preview** | Continuously captures the selected monitor at a target rate of ~5 fps via a background thread and displays it in the app. Supports per-monitor tab switching, an optional cursor overlay, and a show/hide toggle. |
| **Window list** | Lists all visible top-level windows with their title, process name, and process icon. The list is refreshed asynchronously by a background worker thread whenever the app gains focus. |
| **Exclude from capture (checkbox)** | Each row has a checkbox that applies or removes `WDA_EXCLUDEFROMCAPTURE` on that window via DLL injection. Requires Windows 10 version 2004 (build 19041) or later. |
| **Context menu** | Right-click any row for quick access to: Hide, Show, Set/Remove TopMost, Exclude from Capture, Unload DLL, and Add to Process Watch. |
| **TopMost** | Applies or removes `HWND_TOPMOST` via `SetWindowPos`. |
| **Hide / Show** | Hides a window with `ShowWindow(SW_HIDE)`. Hidden windows are tracked and restored when the application exits. |
| **Auto-unload DLL** | Optional checkbox to automatically call `FreeLibrary` on `wda_inject.dll` in the target process after each affinity call, so the DLL does not remain resident. |
| **Process Watch** | Add executable names (e.g. `obs64.exe`) to automatically apply `WDA_EXCLUDEFROMCAPTURE` to any new window belonging to that process. The watch list is checked on a timer and persisted across sessions. |
| **System tray** | Closing the window hides to the tray rather than exiting. The tray menu provides **Show**, **Launch on startup** toggle, and **Exit**. |
| **Settings persistence** | Preview visibility, cursor overlay state, and the watch list are saved to `HKCU\Software\WindowModifier` and restored on next launch. |
| **Logging** | All operations are logged to `window_mod.log` (next to the exe) and to the debugger output stream via [spdlog](https://github.com/gabime/spdlog). |

---

## Requirements

- **OS**: Windows 10 or later (Windows 10 2004 / build 19041+ for `WDA_EXCLUDEFROMCAPTURE`)
- **Architecture**: x64 or x86. Both architectures are bundled; the correct
  DLL and launcher helper are selected automatically at runtime, including
  cross-arch injection (e.g. an x64 host injecting into a 32-bit WOW64 process).
- **Privileges**: Administrator rights are needed for:
  - Hiding or injecting into windows that belong to elevated processes

---

## Building

### Prerequisites

- CMake 3.20 or later
- MSVC (Visual Studio 2019 / 2022)

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Outputs in `build/src/Release/`:
- `window_mod.exe` – main GUI
- `wda_inject.dll` – affinity DLL (auto-copied next to the exe by CMake)
- `wda_launcher_x64.exe` – same-arch injection launcher helper

To also build the 32-bit counterparts needed for cross-arch injection:

```bat
cmake -B build32 -A Win32
cmake --build build32 --config Release --target wda_launcher wda_inject
```

---

## Usage

1. Run `window_mod.exe` (as Administrator for full functionality).
2. The **desktop preview** shows a live capture of the active monitor.
   Switch monitors with the tab bar; toggle visibility with the
   *Show desktop preview* checkbox; toggle the cursor overlay with
   *Show cursor in preview*.
3. The **window list** shows all visible top-level windows.
   - **Tick the checkbox** on any row to apply `WDA_EXCLUDEFROMCAPTURE`
     (the window becomes invisible to screen-capture tools).
   - **Right-click** a row for the context menu (TopMost, Hide, Exclude
     from Capture, Unload DLL, Add to Watch…).
4. The **Process Watch** box lets you add executable filenames
   (e.g. `chrome.exe`). Any new window belonging to a watched process is
   automatically excluded from capture.
5. The **Auto-unload DLL** checkbox (below the window list) controls whether
   `wda_inject.dll` is unloaded from the target process immediately after
   each affinity call.
6. **Closing** the window hides it to the system tray. Use the tray icon menu
   to show the window again, toggle launch-on-startup, or exit.

---

## Installer

A pre-built installer (`WindowModifierInstaller.exe`) is produced by CI for
each release via [Inno Setup](https://jrsoftware.org/isinfo.php) and is
available on the [Releases](https://github.com/ohto-ai/window_mod/releases)
page. It installs both x64 and x86 binaries to `%ProgramFiles%\Window Modifier`.

---

## Project structure

```
window_mod/
├── CMakeLists.txt              Root CMake (fetches spdlog via FetchContent)
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp                WinMain, dialog procedure, background threads
│   ├── window_list.h/.cpp      Window enumeration (EnumWindows)
│   ├── window_ops.h/.cpp       TopMost / Hide / Show / affinity query
│   ├── injector.h/.cpp         DLL-injection logic (same-arch + cross-arch)
│   ├── logger.h/.cpp           Logging helpers (spdlog wrapper)
│   ├── resource.h              Control / dialog / tray / menu IDs
│   └── window_mod.rc           Dialog template + application icon
├── inject_dll/
│   ├── CMakeLists.txt
│   └── dllmain.cpp             wda_inject.dll – calls SetWindowDisplayAffinity
├── inject_launcher/
│   ├── CMakeLists.txt
│   └── launcher_main.cpp       wda_launcher.exe – cross-arch injection helper
└── installer/
    ├── window_mod.iss          Inno Setup installer script
    └── window_mod.wxs          WiX installer script (alternative)
```

---

## License

MIT – see [LICENSE](LICENSE).
