# window_mod

A Windows-only GUI tool that lets you inspect and modify properties of any
top-level window on the desktop.

---

## Features

| Feature | Description |
|---|---|
| **Window list** | Lists all visible top-level windows with their title, process name, PID, and HWND. |
| **Pick cursor** | Click *Pick Window*, then click any window on screen to select it. A red highlight border follows the cursor. Press Escape or right-click to cancel. |
| **Set / Remove TopMost** | Applies or removes `HWND_TOPMOST` via `SetWindowPos`. |
| **Hide Window** | Calls `ShowWindow(SW_HIDE)` and remembers the window in the *Hidden Windows* list so it can be restored later. |
| **Restore hidden window** | Select an entry in the *Hidden Windows* list and click *Show Selected*. |
| **WDA_EXCLUDEFROMCAPTURE** | Injects `wda_inject.dll` into the target process, which calls `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` from within that process (required because the API only succeeds for the process that owns the window). Requires Windows 10 version 2004 (build 19041) or later. |

---

## Requirements

- **OS**: Windows 10 or later (Windows 10 2004+ for WDA_EXCLUDEFROMCAPTURE)
- **Architecture**: x64 or x86 – the exe and the DLL must be compiled for the
  **same** architecture as the target process.
- **Privileges**: Administrator rights are needed for:
  - Hiding windows belonging to elevated processes
  - DLL injection into elevated processes

---

## Building

### Prerequisites

- CMake 3.20 or later
- MSVC (Visual Studio 2019 / 2022) **or** MinGW-w64 (MSYS2)

### MSVC (Visual Studio)

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Outputs in `build/src/Release/`:
- `window_mod.exe`
- `wda_inject.dll` (automatically copied next to the exe by CMake)

### MinGW-w64 (MSYS2)

```sh
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

---

## Usage

1. Run `window_mod.exe` (as Administrator for full functionality).
2. The main list shows all visible windows. Click **Refresh** to update it.
3. To select a window by clicking on it:
   - Press **Pick Window** – the cursor changes to a crosshair.
   - Hover over any window; a highlight border shows which window will be
     selected.
   - **Left-click** to confirm the selection.
   - Press **Escape** or **right-click** to cancel.
4. With a window selected, use the buttons in **Window Operations**:
   - **Set TopMost** / **Remove TopMost** – pin or unpin above all other windows.
   - **Hide Window** – hide the window; it appears in the *Hidden Windows* list.
   - **Exclude Capture** – apply `WDA_EXCLUDEFROMCAPTURE` so the window is
     invisible to screen-capture tools (DLL injection required; run as Admin).
5. In the **Hidden Windows** list:
   - **Show Selected** – restore and bring the window to the foreground.
   - **Remove from List** – stop tracking it (the window remains hidden).

---

## Project structure

```
window_mod/
├── CMakeLists.txt           Root CMake
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp             WinMain + dialog procedure
│   ├── window_list.h/.cpp   Window enumeration (EnumWindows)
│   ├── window_ops.h/.cpp    TopMost / Hide / Show operations
│   ├── injector.h/.cpp      DLL-injection logic
│   ├── resource.h           Control / dialog IDs
│   └── window_mod.rc        Dialog template
└── inject_dll/
    ├── CMakeLists.txt
    └── dllmain.cpp          wda_inject.dll – calls SetWindowDisplayAffinity
```

---

## License

MIT – see [LICENSE](LICENSE).
