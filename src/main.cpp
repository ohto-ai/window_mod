/**
 * main.cpp  –  Windows window modifier tool
 *
 * Features:
 *  - Dark theme (DWM immersive dark mode + custom WM_CTLCOLOR handling)
 *  - Screen preview with per-monitor tab switching and "Show desktop preview" toggle
 *  - Enumerate visible windows (including own); checkbox per row toggles WDA_EXCLUDEFROMCAPTURE
 *  - Window list refreshed asynchronously via injector worker thread on focus gain
 *  - Screen preview captured continuously (~5 fps) via capture worker thread;
 *    starts/stops on focus gain/loss, monitor tab change, "Show desktop preview" toggle
 *  - Two independent background threads (injector + capture) keep the UI responsive
 *  - TopMost toggle button (selected window)
 *  - Hide windows (SW_HIDE) tracked in a separate list for recovery
 *  - Inject wda_inject.dll to set/clear WDA_EXCLUDEFROMCAPTURE
 *  - System tray icon: close button hides to tray; exit only via tray menu
 *  - Restore all hidden windows when exiting
 *  - Process icon shown per list row (rows without an icon show no icon)
 */

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <psapi.h>
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <sstream>
#include <iomanip>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <chrono>

#include "resource.h"
#include "window_list.h"
#include "window_ops.h"
#include "injector.h"
#include "logger.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

// ============================================================================
// Custom window messages posted by background threads to the dialog
// ============================================================================
#define WM_APP_WINDOWS_READY  (WM_APP + 1)   // injector thread: window list ready
#define WM_APP_PREVIEW_READY  (WM_APP + 2)   // capture thread:  preview bitmap ready
#define WM_APP_WATCH_APPLIED  (WM_APP + 3)   // injector thread: watch rule applied

// ============================================================================
// Thread-safe channel (analogous to Rust's crossbeam_channel::unbounded)
// ============================================================================
template<typename T>
class Channel {
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<T>           q_;
    bool                    closed_ = false;
public:
    void send(T val) {
        { std::lock_guard<std::mutex> lk(mtx_); q_.push_back(std::move(val)); }
        cv_.notify_one();
    }
    // Blocks until an item is available or the channel is closed.
    // Returns false when closed and the queue is empty.
    bool recv(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this]{ return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    // Waits up to `ms` milliseconds for an item.
    // Returns true if an item was received, false on timeout or channel close.
    bool recv_timeout(T& out, unsigned ms) {
        std::unique_lock<std::mutex> lk(mtx_);
        bool ready = cv_.wait_for(lk, std::chrono::milliseconds(ms),
                                  [this]{ return !q_.empty() || closed_; });
        if (!ready || q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(mtx_); closed_ = true; }
        cv_.notify_all();
    }
};

// ============================================================================
// Injector worker events
// ============================================================================
enum class InjectorEventType { Update, WatchCheck, Quit };
struct InjectorEvent {
    InjectorEventType type = InjectorEventType::Update;
};

// ============================================================================
// Capture worker events
// ============================================================================
enum class CaptureEventType { Capture, StopCapture, Quit };
struct CaptureEvent {
    CaptureEventType type       = CaptureEventType::StopCapture;
    RECT             monitorRect = {};
    bool             showCursor = false;
};

// ============================================================================
// Dark theme colours
// ============================================================================
static const COLORREF CLR_BG        = RGB(0x1e, 0x1e, 0x2e);
static const COLORREF CLR_TEXT      = RGB(0xe0, 0xe0, 0xe0);
static const COLORREF CLR_SUBTEXT   = RGB(0x88, 0x88, 0xaa);
static const COLORREF CLR_LIST_BG   = RGB(0x22, 0x22, 0x35);
static const COLORREF CLR_BTN_BG    = RGB(0x31, 0x32, 0x4a);
static const COLORREF CLR_BTN_PRESS = RGB(0x45, 0x47, 0x6b);
static const COLORREF CLR_BTN_BORDER= RGB(0x58, 0x5b, 0x70);
static const COLORREF CLR_BTN_FOCUS = RGB(0x89, 0xb4, 0xfa);

// Preview geometry constants
static const int PREVIEW_H_MIN   = 80;   // minimum preview height in pixels
static const int PREVIEW_H_MAX   = 200;  // maximum preview height in pixels
static const int PREVIEW_H_PCT   = 30;   // preview height as % of window height

// LVS_EX_CHECKBOXES state-image index constants (LVIS_STATEIMAGEMASK >> 12)
static const UINT STATE_IMAGE_SHIFT     = 12;
static const UINT STATE_IMAGE_UNCHECKED = 1;
static const UINT STATE_IMAGE_CHECKED   = 2;

// ============================================================================
// State
// ============================================================================
static HINSTANCE g_hInst       = nullptr;
static HWND      g_hDlg        = nullptr;

static std::vector<WindowInfo> g_windows;       // current window snapshot (UI thread)
static std::vector<WindowInfo> g_hiddenWindows; // windows we've hidden

// Monitor / screen preview
static std::vector<RECT> g_monitors;
static int               g_currentMonitor = 0;
static HBITMAP           g_previewBmp     = nullptr;

// Suppress LVN_ITEMCHANGED side-effects during programmatic list updates
static bool g_populatingList = false;

// True when the dialog is the active (foreground) window
static bool g_hasFocus = true;

// Whether the desktop preview is shown (mirrors IDC_CHK_SHOW_PREVIEW)
static bool g_showDesktopPreview = true;

// Whether to auto-unload the DLL after each injection (mirrors IDC_CHK_AUTO_UNLOAD)
static bool g_autoUnloadDll = true;

// Dark theme GDI resources
static HBRUSH g_hbrBg            = nullptr;
static HBRUSH g_hbrListBg        = nullptr;
static HFONT  g_hFontBold        = nullptr;
static HFONT  g_hFontPlaceholder = nullptr; // large font for the focus-lost screen

// Tray icon
static NOTIFYICONDATA g_nid       = {};
static bool           g_trayAdded = false;

// ── Injector worker thread ──────────────────────────────────────────────────
// Receives InjectorEvent::Update, calls EnumerateWindows(), stores result in
// g_pendingWindows, then posts WM_APP_WINDOWS_READY to the dialog.
static Channel<InjectorEvent>   g_injectorChannel;
static std::thread               g_injectorThread;
static std::mutex                g_pendingWindowsMutex;
static std::vector<WindowInfo>   g_pendingWindows;

// ── Process watch ───────────────────────────────────────────────────────────
// g_watchedExeNames: exe filenames to monitor (UI-thread owned; copied under lock).
// g_watchedPids:     PIDs already injected (updated by injector thread; cleaned on exit).
static std::vector<std::wstring> g_watchedExeNames;
static std::mutex                g_watchedExeMutex;
static std::set<DWORD>           g_watchedPids;
static std::mutex                g_watchedPidsMutex;

// ── Capture worker thread ───────────────────────────────────────────────────
// Continuously captures frames (via BitBlt) while in capturing state and posts
// WM_APP_PREVIEW_READY to the dialog for each frame.
// CaptureEvent::Capture starts/restarts continuous capture for a given monitor rect.
// CaptureEvent::StopCapture stops the continuous loop.
static Channel<CaptureEvent>    g_captureChannel;
static std::thread               g_captureThread;
static std::mutex                g_pendingPreviewMutex;
static HBITMAP                   g_pendingPreviewBmp = nullptr;
// Mirrors IDC_CHK_SHOW_CURSOR; updated atomically so the capture thread can
// read it on every frame without touching the UI thread.
static std::atomic<bool>         g_captureShowCursor{false};

// ============================================================================
// Forward declarations
// ============================================================================
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Injector worker thread
// Handles InjectorEvent::Update: enumerates top-level windows asynchronously
// and notifies the UI thread via WM_APP_WINDOWS_READY.
// ============================================================================
static void InjectorWorkerProc()
{
    InjectorEvent evt;
    while (g_injectorChannel.recv(evt)) {
        if (evt.type == InjectorEventType::Quit) break;

        if (evt.type == InjectorEventType::Update) {
            // Enumerate top-level windows and notify the UI thread.
            auto windows = EnumerateWindows();
            {
                std::lock_guard<std::mutex> lk(g_pendingWindowsMutex);
                g_pendingWindows = std::move(windows);
            }
            if (g_hDlg)
                PostMessage(g_hDlg, WM_APP_WINDOWS_READY, 0, 0);
        }
        else if (evt.type == InjectorEventType::WatchCheck) {
            // Get a snapshot of the current watch list.
            std::vector<std::wstring> watchNames;
            {
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                watchNames = g_watchedExeNames;
            }
            if (watchNames.empty()) continue;

            // Clean up PIDs that are no longer alive.
            {
                std::lock_guard<std::mutex> lk(g_watchedPidsMutex);
                for (auto it = g_watchedPids.begin(); it != g_watchedPids.end(); ) {
                    HANDLE hP = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *it);
                    if (!hP) {
                        it = g_watchedPids.erase(it);
                    } else {
                        DWORD code = 0;
                        GetExitCodeProcess(hP, &code);
                        CloseHandle(hP);
                        it = (code != STILL_ACTIVE) ? g_watchedPids.erase(it) : ++it;
                    }
                }
            }

            // Enumerate all running process IDs.
            DWORD pids[4096]; DWORD needed = 0;
            if (!EnumProcesses(pids, sizeof(pids), &needed)) continue;
            DWORD count = needed / sizeof(DWORD);

            int applied = 0;
            for (DWORD i = 0; i < count; ++i) {
                DWORD pid = pids[i];
                if (!pid) continue;

                // Skip already-injected PIDs.
                {
                    std::lock_guard<std::mutex> lk(g_watchedPidsMutex);
                    if (g_watchedPids.count(pid)) continue;
                }

                // Check process name against the watch list.
                std::wstring procName = GetProcessName(pid);
                if (procName.empty() || procName == L"<unknown>") continue;

                bool matched = false;
                for (const auto& w : watchNames) {
                    if (_wcsicmp(procName.c_str(), w.c_str()) == 0) { matched = true; break; }
                }
                if (!matched) continue;

                // Find all visible, titled top-level windows for this PID.
                struct FindCtx { DWORD pid; std::vector<HWND> hwnds; };
                FindCtx ctx = { pid };
                EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                    auto* c = reinterpret_cast<FindCtx*>(lp);
                    DWORD wpid = 0;
                    GetWindowThreadProcessId(hwnd, &wpid);
                    if (wpid == c->pid && IsWindowVisible(hwnd)) {
                        wchar_t t[8] = {};
                        GetWindowTextW(hwnd, t, 8);
                        if (t[0]) c->hwnds.push_back(hwnd);
                    }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&ctx));

                if (ctx.hwnds.empty()) continue; // process not ready yet; retry next tick

                // Apply ExcludeFromCapture to each window of this process.
                for (HWND hwnd : ctx.hwnds)
                    InjectWDASetAffinity(hwnd, 0x00000011u, true);

                // Mark PID as processed so we don't re-inject on subsequent ticks.
                {
                    std::lock_guard<std::mutex> lk(g_watchedPidsMutex);
                    g_watchedPids.insert(pid);
                }
                ++applied;
            }

            if (g_hDlg && applied > 0)
                PostMessage(g_hDlg, WM_APP_WATCH_APPLIED, static_cast<WPARAM>(applied), 0);
        }
    }
}

// ============================================================================
// Capture worker thread
// Implements a continuous BitBlt capture loop, analogous to Invisiwind's
// ScreenCapture::start_free_threaded streaming model:
//   • CaptureEvent::Capture  → enter/restart continuous capture for the given rect
//   • CaptureEvent::StopCapture → exit continuous capture, discard pending bitmap
//   • CaptureEvent::Quit     → terminate thread
//
// While capturing, a frame is taken every ~200 ms (≈5 fps) and posted to the
// UI thread via WM_APP_PREVIEW_READY; the cursor overlay is drawn when
// g_captureShowCursor is set.  Between frames the thread waits on the channel
// so that a new event (monitor switch, stop, quit) is acted on immediately.
// ============================================================================
static void CaptureWorkerProc()
{
    bool capturing  = false;
    RECT activeRect = {};

    auto takeFrame = [&]() {
        int w = activeRect.right - activeRect.left;
        int h = activeRect.bottom - activeRect.top;
        if (w <= 0 || h <= 0) return;

        HDC     hScreen = GetDC(nullptr);
        HDC     hMem    = CreateCompatibleDC(hScreen);
        HBITMAP hBmp    = CreateCompatibleBitmap(hScreen, w, h);
        HGDIOBJ old     = SelectObject(hMem, hBmp);
        BitBlt(hMem, 0, 0, w, h, hScreen, activeRect.left, activeRect.top, SRCCOPY | CAPTUREBLT);

        if (g_captureShowCursor.load()) {
            CURSORINFO ci = {};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
                DrawIconEx(hMem,
                    ci.ptScreenPos.x - activeRect.left,
                    ci.ptScreenPos.y - activeRect.top,
                    ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            }
        }

        SelectObject(hMem, old);
        DeleteDC(hMem);
        ReleaseDC(nullptr, hScreen);

        // Replace any unconsumed frame (bounded-1 behaviour).
        HBITMAP discarded = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_pendingPreviewMutex);
            discarded = g_pendingPreviewBmp;
            g_pendingPreviewBmp = hBmp;
        }
        if (discarded) DeleteObject(discarded);

        if (g_hDlg)
            PostMessage(g_hDlg, WM_APP_PREVIEW_READY, 0, 0);
    };

    while (true) {
        CaptureEvent evt;
        bool hasEvent;

        if (capturing) {
            // Take a frame, then wait up to 200 ms for the next event.
            takeFrame();
            hasEvent = g_captureChannel.recv_timeout(evt, 200);
            if (!hasEvent) continue;  // timeout → capture another frame
        } else {
            // Idle: block indefinitely until an event arrives.
            hasEvent = g_captureChannel.recv(evt);
            if (!hasEvent) break;  // channel closed
        }

        if (evt.type == CaptureEventType::Quit) break;

        if (evt.type == CaptureEventType::StopCapture) {
            capturing = false;
            // Discard any pending (not-yet-consumed) preview bitmap.
            HBITMAP old = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_pendingPreviewMutex);
                old = g_pendingPreviewBmp;
                g_pendingPreviewBmp = nullptr;
            }
            if (old) DeleteObject(old);
        } else if (evt.type == CaptureEventType::Capture) {
            // Start or restart continuous capture (e.g. monitor switched).
            capturing  = true;
            activeRect = evt.monitorRect;
            // Next iteration will call takeFrame() immediately.
        }
    }
}

// ============================================================================
// Settings persistence (HKCU\Software\WindowModifier)
// ============================================================================
static const wchar_t* const REG_APP_KEY = L"Software\\WindowModifier";

static void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr)
        != ERROR_SUCCESS)
        return;

    DWORD showPreview = g_showDesktopPreview ? 1u : 0u;
    RegSetValueExW(hKey, L"ShowDesktopPreview", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&showPreview), sizeof(showPreview));

    DWORD showCursor = g_captureShowCursor.load() ? 1u : 0u;
    RegSetValueExW(hKey, L"ShowCursorInPreview", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&showCursor), sizeof(showCursor));

    // Build REG_MULTI_SZ: each name null-terminated, list ends with extra null.
    {
        std::lock_guard<std::mutex> lk(g_watchedExeMutex);
        std::vector<wchar_t> buf;
        for (const auto& name : g_watchedExeNames) {
            buf.insert(buf.end(), name.begin(), name.end());
            buf.push_back(L'\0');
        }
        buf.push_back(L'\0');
        RegSetValueExW(hKey, L"WatchedExeNames", 0, REG_MULTI_SZ,
            reinterpret_cast<const BYTE*>(buf.data()),
            static_cast<DWORD>(buf.size() * sizeof(wchar_t)));
    }

    RegCloseKey(hKey);
}

static void LoadSettings(HWND hDlg)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP_KEY,
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;

    // ShowDesktopPreview
    {
        DWORD val = 1, size = sizeof(val), type = 0;
        if (RegQueryValueExW(hKey, L"ShowDesktopPreview", nullptr, &type,
                reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS
            && type == REG_DWORD)
        {
            g_showDesktopPreview = (val != 0);
            CheckDlgButton(hDlg, IDC_CHK_SHOW_PREVIEW,
                g_showDesktopPreview ? BST_CHECKED : BST_UNCHECKED);
            // Show/hide preview-related controls to match the loaded state
            int sw = g_showDesktopPreview ? SW_SHOW : SW_HIDE;
            ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_SUBTEXT), sw);
            ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_STATIC),  sw);
            ShowWindow(GetDlgItem(hDlg, IDC_TAB_SCREENS),     sw);
            ShowWindow(GetDlgItem(hDlg, IDC_CHK_SHOW_CURSOR), sw);
        }
    }

    // ShowCursorInPreview
    {
        DWORD val = 0, size = sizeof(val), type = 0;
        if (RegQueryValueExW(hKey, L"ShowCursorInPreview", nullptr, &type,
                reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS
            && type == REG_DWORD)
        {
            g_captureShowCursor.store(val != 0);
            CheckDlgButton(hDlg, IDC_CHK_SHOW_CURSOR,
                val ? BST_CHECKED : BST_UNCHECKED);
        }
    }

    // WatchedExeNames (REG_MULTI_SZ)
    {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(hKey, L"WatchedExeNames", nullptr, &type,
                nullptr, &size) == ERROR_SUCCESS
            && type == REG_MULTI_SZ && size > 0)
        {
            std::vector<wchar_t> buf(size / sizeof(wchar_t));
            if (RegQueryValueExW(hKey, L"WatchedExeNames", nullptr, nullptr,
                    reinterpret_cast<BYTE*>(buf.data()), &size) == ERROR_SUCCESS)
            {
                HWND hList = GetDlgItem(hDlg, IDC_WATCH_LIST);
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                g_watchedExeNames.clear();
                for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
                    std::wstring name(p);
                    g_watchedExeNames.push_back(name);
                    LVITEMW lvi = {};
                    lvi.mask    = LVIF_TEXT;
                    lvi.iItem   = ListView_GetItemCount(hList);
                    lvi.pszText = const_cast<LPWSTR>(name.c_str());
                    ListView_InsertItem(hList, &lvi);
                }
            }
        }
    }

    RegCloseKey(hKey);
}

// ============================================================================
// Auto-start helpers (HKCU\...\Run registry key)
// ============================================================================

static bool IsAutoStartEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(hKey, L"WindowModifier",
                       nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

static bool SetAutoStart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (enable) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        ok = (RegSetValueExW(hKey, L"WindowModifier", 0, REG_SZ,
                  reinterpret_cast<const BYTE*>(path),
                  static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)))
              == ERROR_SUCCESS);
    } else {
        LSTATUS st = RegDeleteValueW(hKey, L"WindowModifier");
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(hKey);
    return ok;
}

// ============================================================================
// WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    InitLogger();

    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    DialogBoxW(hInstance, MAKEINTRESOURCEW(IDD_MAIN_DIALOG), nullptr, DlgProc);
    return 0;
}

// ============================================================================
// Utility helpers
// ============================================================================

static void SetStatus(HWND hDlg, const std::wstring& msg)
{
    SetDlgItemTextW(hDlg, IDC_STATUS_TEXT, msg.c_str());
}

static std::wstring FmtHandle(HWND hwnd)
{
    std::wostringstream oss;
    oss << L"0x" << std::uppercase << std::hex
        << reinterpret_cast<uintptr_t>(hwnd);
    return oss.str();
}

// ============================================================================
// Monitor enumeration
// ============================================================================

static BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT lprc, LPARAM lParam)
{
    reinterpret_cast<std::vector<RECT>*>(lParam)->push_back(*lprc);
    return TRUE;
}

static void EnumerateMonitors()
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                        reinterpret_cast<LPARAM>(&g_monitors));
    if (g_monitors.empty()) {
        RECT r = { 0, 0,
                   GetSystemMetrics(SM_CXSCREEN),
                   GetSystemMetrics(SM_CYSCREEN) };
        g_monitors.push_back(r);
    }
    if (g_currentMonitor >= static_cast<int>(g_monitors.size()))
        g_currentMonitor = 0;
}

// ============================================================================
// Screen capture – helpers
// ============================================================================

// Send a Capture event to the capture worker thread.
// The cursor-overlay state is tracked via g_captureShowCursor (atomic) so
// the worker reads the up-to-date value on every frame without touching the UI.
static void SendCaptureEvent(int monitorIdx)
{
    if (monitorIdx < 0 || monitorIdx >= static_cast<int>(g_monitors.size()))
        return;
    CaptureEvent evt;
    evt.type        = CaptureEventType::Capture;
    evt.monitorRect = g_monitors[monitorIdx];
    g_captureChannel.send(evt);
}

// Send a StopCapture event (e.g. on focus loss).
static void SendStopCaptureEvent()
{
    CaptureEvent evt;
    evt.type = CaptureEventType::StopCapture;
    g_captureChannel.send(evt);
}

// ---------------------------------------------------------------------------
// ListView column setup for the hidden-windows list (4 columns).

// ---------------------------------------------------------------------------
// Main window list: Title, Process, TopMost, Hidden columns.
// ExcludeCapture is represented by the LVS_EX_CHECKBOXES checkbox.

static void InitMainListViewColumns(HWND hList)
{
    static const struct { const wchar_t* name; int cx; } cols[] = {
        { L"Title",   200 },
        { L"Process",  90 },
        { L"TopMost",  60 },
        { L"Hidden",   50 },
    };
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (int i = 0; i < 4; ++i) {
        lvc.cx       = cols[i].cx;
        lvc.pszText  = const_cast<LPWSTR>(cols[i].name);
        lvc.iSubItem = i;
        ListView_InsertColumn(hList, i, &lvc);
    }
}

// ---------------------------------------------------------------------------
// Populate (or refresh) the main window list from the current g_windows snapshot.
// The caller is responsible for ensuring g_windows is up-to-date before calling.
// Use g_injectorChannel.send({InjectorEventType::Update}) to request an async
// refresh; the injector thread will post WM_APP_WINDOWS_READY when done.

static void PopulateWindowList(HWND hDlg, bool preserveSelection = false)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);

    HWND selHwnd = nullptr;
    if (preserveSelection) {
        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (sel >= 0 && sel < static_cast<int>(g_windows.size()))
            selHwnd = g_windows[sel].hwnd;
    }

    g_populatingList = true;
    ListView_DeleteAllItems(hList);

    // Build an image list from per-window icons.
    int n = static_cast<int>(g_windows.size());
    HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, n, 0);
    std::vector<int> imgIdx(n, -1);
    for (int i = 0; i < n; ++i) {
        if (g_windows[i].hIcon)
            imgIdx[i] = ImageList_AddIcon(hImgList, g_windows[i].hIcon);
    }
    HIMAGELIST hOld = ListView_SetImageList(hList, hImgList, LVSIL_SMALL);
    if (hOld) ImageList_Destroy(hOld);

    LVITEMW lvi = {};

    for (int i = 0; i < n; ++i) {
        const auto& w = g_windows[i];

        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.lParam   = static_cast<LPARAM>(i);
        lvi.pszText  = const_cast<LPWSTR>(w.title.c_str());
        lvi.iImage   = (imgIdx[i] >= 0) ? imgIdx[i] : I_IMAGENONE;
        lvi.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE; // always set image to avoid defaulting to 0
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, i, 1,
            const_cast<LPWSTR>(w.processName.c_str()));

        // TopMost column
        ListView_SetItemText(hList, i, 2,
            const_cast<LPWSTR>(IsWindowTopMost(w.hwnd) ? L"\u2713" : L""));

        // Hidden column
        ListView_SetItemText(hList, i, 3,
            const_cast<LPWSTR>(w.isHidden ? L"\u25cf" : L""));

        // ExcludeCapture state = checkbox state (skip for hidden windows)
        ListView_SetCheckState(hList, i,
            (!w.isHidden && IsWindowExcludeFromCapture(w.hwnd)) ? TRUE : FALSE);
    }
    g_populatingList = false;

    // Restore selection
    if (selHwnd) {
        for (int i = 0; i < static_cast<int>(g_windows.size()); ++i) {
            if (g_windows[i].hwnd == selHwnd) {
                ListView_SetItemState(hList, i,
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, i, FALSE);
                break;
            }
        }
    }

    if (!preserveSelection) {
        SetStatus(hDlg,
            std::wstring(L"Refreshed \u2013 ") + std::to_wstring(g_windows.size())
            + L" windows found.");
    }
}

// ---------------------------------------------------------------------------
// All regular control IDs – used to show/hide them en masse.
static const int s_allControls[] = {
    IDC_PREVIEW_LABEL, IDC_PREVIEW_SUBTEXT, IDC_PREVIEW_STATIC, IDC_TAB_SCREENS,
    IDC_CHK_SHOW_PREVIEW,
    IDC_HIDE_APPS_LABEL, IDC_HIDE_APPS_SUB, IDC_WINDOW_LIST, IDC_SELECTED_INFO,
    IDC_CHK_AUTO_UNLOAD,
    IDC_GRP_WATCH, IDC_WATCH_EDIT, IDC_BTN_WATCH_ADD, IDC_BTN_WATCH_REMOVE, IDC_WATCH_LIST,
    IDC_STATUS_TEXT, IDC_CHK_SHOW_CURSOR,
};

// Show a full-page ":)" placeholder when the app loses focus.
static void ShowPlaceholder(HWND hDlg)
{
    g_windows.clear();
    for (int id : s_allControls)
        if (HWND h = GetDlgItem(hDlg, id))
            ShowWindow(h, SW_HIDE);

    HWND hPh = GetDlgItem(hDlg, IDC_PLACEHOLDER_LABEL);
    RECT rc;
    GetClientRect(hDlg, &rc);
    MoveWindow(hPh, rc.left, rc.top, rc.right, rc.bottom, FALSE);
    ShowWindow(hPh, SW_SHOW);
    InvalidateRect(hDlg, nullptr, TRUE);
}

// Restore all regular controls (called when the app regains focus).
static void HidePlaceholder(HWND hDlg)
{
    ShowWindow(GetDlgItem(hDlg, IDC_PLACEHOLDER_LABEL), SW_HIDE);
    for (int id : s_allControls)
        if (HWND h = GetDlgItem(hDlg, id))
            ShowWindow(h, SW_SHOW);
    // Re-hide preview-related controls if desktop preview is disabled
    if (!g_showDesktopPreview) {
        ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_SUBTEXT), SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_STATIC),  SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_TAB_SCREENS),     SW_HIDE);
        ShowWindow(GetDlgItem(hDlg, IDC_CHK_SHOW_CURSOR), SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// Restore every window currently in the hidden list (called on exit).

static void RestoreAllHiddenWindows()
{
    for (auto& w : g_windows) {
        if (w.isHidden && IsWindow(w.hwnd))
            ShowWindowRestore(w.hwnd);
    }
    // Also restore any tracked in g_hiddenWindows that may not be in g_windows
    // (e.g. if the app never got a focus-in after hiding)
    for (auto& w : g_hiddenWindows) {
        bool alreadyRestored = false;
        for (const auto& gw : g_windows)
            if (gw.hwnd == w.hwnd) { alreadyRestored = true; break; }
        if (!alreadyRestored && IsWindow(w.hwnd))
            ShowWindowRestore(w.hwnd);
    }
    g_hiddenWindows.clear();
}

// ---------------------------------------------------------------------------
// Return the WindowInfo for the currently selected row (main list).

static const WindowInfo* GetSelectedWindow(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(g_windows.size()))
        return nullptr;
    return &g_windows[sel];
}

// ---------------------------------------------------------------------------
// Update the "Selected:" info label.

static void UpdateSelectedInfo(HWND hDlg)
{
    const WindowInfo* w = GetSelectedWindow(hDlg);
    if (!w) {
        SetDlgItemTextW(hDlg, IDC_SELECTED_INFO, L"Selected: (none)");
        return;
    }
    std::wstring info = L"Selected: \"" + w->title + L"\"   "
        + w->processName + L"  PID:" + std::to_wstring(w->pid)
        + L"  HWND:" + FmtHandle(w->hwnd);
    SetDlgItemTextW(hDlg, IDC_SELECTED_INFO, info.c_str());
}

// ============================================================================
// Tray icon management
// ============================================================================

static void CreateTrayIcon(HWND hDlg)
{
    HICON hIcon = static_cast<HICON>(
        LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON),
                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!hIcon)
        hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    g_nid        = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd   = hDlg;
    g_nid.uID    = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon  = hIcon;
    wcscpy_s(g_nid.szTip, L"Window Modifier");
    g_trayAdded = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
}

static void DestroyTrayIcon()
{
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

// ============================================================================
// WM_SIZE – resize/move controls to fill the dialog
// ============================================================================

static void OnSize(HWND hDlg, int /*cx*/, int /*cy*/)
{
    RECT r = {};
    GetClientRect(hDlg, &r);
    int W = r.right, H = r.bottom;
    if (W <= 0 || H <= 0) return;

    const int mX    = 10;   // horizontal margin
    const int mY    = 8;    // top margin
    const int DY    = 4;    // small vertical gap
    const int btnH  = 24;   // button height
    const int lblH  = 14;   // small label height
    const int bigH  = 18;   // section header height
    const int subH  = 13;   // subtitle height
    int listW = W - 2 * mX;

    // --- Bottom zone (computed bottom-up) ---
    const int statusH    = 16;
    // Watch group: edit+buttons row + list (list height is 20% of window height, min 80).
    const int watchListH = std::max(80, H * 20 / 100);
    const int watchGrpH  = 14 + btnH + 4 + watchListH + 8;
    // Auto-unload DLL checkbox sits between the window list and the watch group
    const int autoUnloadH = lblH;

    int y = H - mY;

    int statusY     = y - statusH;     y = statusY     - DY;
    int watchY      = y - watchGrpH;   y = watchY      - DY;
    int autoUnloadY = y - autoUnloadH; y = autoUnloadY - DY;
    int selInfoY    = y - lblH;        y = selInfoY    - DY;

    // --- Top zone (computed top-down) ---
    int top = mY;

    int prevLblY = top;  top += bigH + 2;
    // Preview-related controls only occupy vertical space when the preview is shown
    int prevSubY = 0, previewY = 0, previewH = 0, tabY = 0;
    if (g_showDesktopPreview) {
        prevSubY = top;  top += subH + DY;
        previewY = top;
        previewH = std::max(PREVIEW_H_MIN,
                            std::min(PREVIEW_H_MAX, H * PREVIEW_H_PCT / 100));
        top += previewH + DY;
        tabY = top;  top += 22 + DY;
    }
    int hideAppY = top;  top += bigH + 2;
    int hideSubY = top;  top += subH + DY;
    int listY    = top;

    // Window list fills the gap between top and bottom zones
    int listH = selInfoY - DY - listY;
    if (listH < 40) listH = 40;

    auto Move = [&](int id, int x, int cy2, int cw, int ch) {
        if (HWND hCtrl = GetDlgItem(hDlg, id))
            MoveWindow(hCtrl, x, cy2, cw, ch, FALSE);
    };

    // Preview section – label and both checkboxes share the top row
    static const int chkW  = 140; // "Show cursor in preview" width
    static const int chkW2 = 160; // "Show desktop preview" width
    Move(IDC_PREVIEW_LABEL,    mX, prevLblY, listW - chkW2 - chkW - 8, bigH);
    Move(IDC_CHK_SHOW_PREVIEW, mX + listW - chkW2 - chkW - 4, prevLblY, chkW2, bigH);
    Move(IDC_CHK_SHOW_CURSOR,  mX + listW - chkW, prevLblY, chkW, bigH);
    if (g_showDesktopPreview) {
        Move(IDC_PREVIEW_SUBTEXT, mX, prevSubY, listW, subH);
        Move(IDC_PREVIEW_STATIC,  mX, previewY, listW, previewH);
        Move(IDC_TAB_SCREENS,     mX, tabY,     listW, 22);
    }

    // Hide applications section
    Move(IDC_HIDE_APPS_LABEL, mX, hideAppY, listW, bigH);
    Move(IDC_HIDE_APPS_SUB,   mX, hideSubY, listW, subH);

    // Window list
    Move(IDC_WINDOW_LIST, mX, listY, listW, listH);

    // Selected info label
    Move(IDC_SELECTED_INFO, mX, selInfoY, listW, lblH);

    // Auto-unload DLL checkbox (between window list and watch group)
    Move(IDC_CHK_AUTO_UNLOAD, mX, autoUnloadY, listW, autoUnloadH);

    // Process watch section
    if (HWND hGrp = GetDlgItem(hDlg, IDC_GRP_WATCH))
        MoveWindow(hGrp, mX, watchY, listW, watchGrpH, FALSE);
    {
        const int wx    = mX + 8;
        const int wy    = watchY + 14;
        const int addW  = 44;
        const int remW  = 60;
        const int editW = listW - 16 - 8 - addW - 4 - remW;
        Move(IDC_WATCH_EDIT,         wx,                        wy, editW, btnH);
        Move(IDC_BTN_WATCH_ADD,      wx + editW + 4,            wy, addW,  btnH);
        Move(IDC_BTN_WATCH_REMOVE,   wx + editW + 4 + addW + 4, wy, remW,  btnH);
        Move(IDC_WATCH_LIST,         wx, wy + btnH + 4, listW - 16, watchListH);
        // Stretch the single Process column to fill the list width
        if (HWND hWatchList = GetDlgItem(hDlg, IDC_WATCH_LIST)) {
            int colW = (listW - 16) - GetSystemMetrics(SM_CXVSCROLL) - 2;
            if (colW > 0) ListView_SetColumnWidth(hWatchList, 0, colW);
        }
    }

    // Stretch the Title column of the main window list to fill available width
    // Columns: Title (dynamic) | Process (90) | TopMost (60) | Hidden (50)
    if (HWND hWinList = GetDlgItem(hDlg, IDC_WINDOW_LIST)) {
        int scrollW = GetSystemMetrics(SM_CXVSCROLL);
        int titleW  = listW - 90 - 60 - 50 - scrollW - 4;
        if (titleW > 40) ListView_SetColumnWidth(hWinList, 0, titleW);
    }

    // Status bar
    Move(IDC_STATUS_TEXT, mX, statusY, listW, statusH);

    // Placeholder: always fill the entire client area (it is hidden when focused).
    if (HWND hPh = GetDlgItem(hDlg, IDC_PLACEHOLDER_LABEL))
        MoveWindow(hPh, 0, 0, W, H, FALSE);

    InvalidateRect(hDlg, nullptr, TRUE);
}

// ============================================================================
// Dialog procedure
// ============================================================================

INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    // --------------------------------------------------------------------
    case WM_INITDIALOG:
    {
        g_hDlg = hDlg;

        // Set window title-bar icon (both large and small)
        {
            HICON hBig = static_cast<HICON>(
                LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON),
                           IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
            HICON hSm  = static_cast<HICON>(
                LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON),
                           IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
            if (hBig) SendMessageW(hDlg, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hBig));
            if (hSm)  SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSm));
        }

        // Dark title bar (Windows 10 v2004+ uses value 20; older builds used 19)
        BOOL dark = TRUE;
        HRESULT hr = DwmSetWindowAttribute(hDlg,
            20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        if (FAILED(hr))
            DwmSetWindowAttribute(hDlg, 19, &dark, sizeof(dark));

        // Dark theme brush
        g_hbrBg    = CreateSolidBrush(CLR_BG);
        g_hbrListBg = CreateSolidBrush(CLR_LIST_BG);

        // Bold font for section headers
        NONCLIENTMETRICSW ncm = {};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfWeight = FW_BOLD;
        ncm.lfMessageFont.lfHeight = -15;
        g_hFontBold = CreateFontIndirectW(&ncm.lfMessageFont);
        if (g_hFontBold) {
            SendDlgItemMessageW(hDlg, IDC_PREVIEW_LABEL,
                WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), FALSE);
            SendDlgItemMessageW(hDlg, IDC_HIDE_APPS_LABEL,
                WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), FALSE);
        }

                // Large font for the full-page focus-lost placeholder (72 px ≈ 54 pt at 96 dpi)
        static const int PLACEHOLDER_FONT_HEIGHT = -72;
        g_hFontPlaceholder = CreateFontW(PLACEHOLDER_FONT_HEIGHT, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (g_hFontPlaceholder)
            SendDlgItemMessageW(hDlg, IDC_PLACEHOLDER_LABEL,
                WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontPlaceholder), FALSE);
        // Ensure placeholder starts hidden
        ShowWindow(GetDlgItem(hDlg, IDC_PLACEHOLDER_LABEL), SW_HIDE);

        // Centre the dialog (portrait aspect ratio)
        {
            const int dlgW = 480, dlgH = 780;
            int scW = GetSystemMetrics(SM_CXSCREEN);
            int scH = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg, nullptr,
                         (scW - dlgW) / 2, (scH - dlgH) / 2,
                         dlgW, dlgH, SWP_NOZORDER);
        }

        // Init main list view (Title, Process, TopMost + checkboxes)
        {
            HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
            InitMainListViewColumns(hList);
            DWORD exStyle = LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES
                          | LVS_EX_DOUBLEBUFFER;
            ListView_SetExtendedListViewStyle(hList, exStyle);
            ListView_SetBkColor(hList, CLR_LIST_BG);
            ListView_SetTextBkColor(hList, CLR_LIST_BG);
            ListView_SetTextColor(hList, CLR_TEXT);
            SetWindowTheme(hList, L"DarkMode_Explorer", nullptr);
        }

        // Enumerate monitors and populate tabs
        EnumerateMonitors();
        {
            HWND hTab = GetDlgItem(hDlg, IDC_TAB_SCREENS);
            SetWindowTheme(hTab, L"DarkMode_Explorer", nullptr);
            for (int i = 0; i < static_cast<int>(g_monitors.size()); ++i) {
                wchar_t label[32];
                swprintf_s(label, L"Screen %d", i + 1);
                TCITEMW tie = {};
                tie.mask    = TCIF_TEXT;
                tie.pszText = label;
                TabCtrl_InsertItem(hTab, i, &tie);
            }
            TabCtrl_SetCurSel(hTab, 0);
        }

        // Apply owner-draw style to buttons for flat dark appearance
        for (int btnId : {IDC_BTN_WATCH_ADD, IDC_BTN_WATCH_REMOVE}) {
            HWND hBtn = GetDlgItem(hDlg, btnId);
            if (!hBtn) continue;
            LONG_PTR style = GetWindowLongPtrW(hBtn, GWL_STYLE);
            style = (style & ~static_cast<LONG_PTR>(BS_TYPEMASK))
                  | static_cast<LONG_PTR>(BS_OWNERDRAW);
            SetWindowLongPtrW(hBtn, GWL_STYLE, style);
        }

        // Init process watch list view (single "Process" column)
        // LVS_EX_NOHORIZONTALSCROLL prevents the horizontal scrollbar from appearing.
        {
            HWND hList = GetDlgItem(hDlg, IDC_WATCH_LIST);
            LVCOLUMNW lvc = {};
            lvc.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.cx      = 300;
            lvc.pszText = const_cast<LPWSTR>(L"Process (exe name)");
            ListView_InsertColumn(hList, 0, &lvc);
            static const DWORD LVS_EX_NOHORIZONTALSCROLL = 0x04000000;
            ListView_SetExtendedListViewStyle(hList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_NOHORIZONTALSCROLL);
            ListView_SetBkColor(hList, CLR_LIST_BG);
            ListView_SetTextBkColor(hList, CLR_LIST_BG);
            ListView_SetTextColor(hList, CLR_TEXT);
            SetWindowTheme(hList, L"DarkMode_Explorer", nullptr);
        }

        // "Show desktop preview" checkbox – default on
        g_showDesktopPreview = true;
        CheckDlgButton(hDlg, IDC_CHK_SHOW_PREVIEW,
                       g_showDesktopPreview ? BST_CHECKED : BST_UNCHECKED);
        // "Show cursor in preview" checkbox – default off; sync the atomic.
        g_captureShowCursor.store(
            IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_CURSOR) == BST_CHECKED);
        // "Auto-unload DLL" checkbox – default on.
        g_autoUnloadDll = true;
        CheckDlgButton(hDlg, IDC_CHK_AUTO_UNLOAD, BST_CHECKED);

        // Load persisted settings (may override the defaults set above).
        LoadSettings(hDlg);

        // Tray icon
        CreateTrayIcon(hDlg);

        // Start the two background worker threads.
        g_injectorThread = std::thread(InjectorWorkerProc);
        g_captureThread  = std::thread(CaptureWorkerProc);

        // Request initial window enumeration (async).
        g_injectorChannel.send(InjectorEvent{InjectorEventType::Update});

        // Process watch timer: fires every 2 s to check for new matching processes.
        SetTimer(hDlg, IDT_WATCH, 2000, nullptr);

        // Start the initial screen preview if enabled (async).
        if (g_showDesktopPreview && !g_monitors.empty())
            SendCaptureEvent(0);

        // Trigger initial layout
        {
            RECT rc; GetClientRect(hDlg, &rc);
            OnSize(hDlg, rc.right, rc.bottom);
        }
        return TRUE;
    }

    // --------------------------------------------------------------------
    // Window gain/lose focus: drive injector-worker updates and capture.
    case WM_ACTIVATE:
    {
        if (LOWORD(wParam) == WA_INACTIVE) {
            g_hasFocus = false;
            // Stop the screen preview while unfocused.
            if (g_previewBmp) {
                DeleteObject(g_previewBmp);
                g_previewBmp = nullptr;
            }
            SendStopCaptureEvent();
            ShowPlaceholder(hDlg);
        } else {
            g_hasFocus = true;
            HidePlaceholder(hDlg);
            // Trigger async window-list refresh (Invisiwind: InjectorWorkerEvent::Update).
            g_injectorChannel.send(InjectorEvent{InjectorEventType::Update});
            // Restart screen preview if enabled (Invisiwind: CaptureWorkerEvent::Capture).
            if (g_showDesktopPreview)
                SendCaptureEvent(g_currentMonitor);
            UpdateSelectedInfo(hDlg);
        }
        return FALSE;
    }

    // --------------------------------------------------------------------
    // Owner-draw: preview static + flat dark buttons.
    case WM_DRAWITEM:
    {
        auto* di = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);

        // ---- Flat owner-draw buttons ----------------------------------------
        if (di->CtlType == ODT_BUTTON) {
            HDC  hDC = di->hDC;
            RECT rc  = di->rcItem;
            bool pressed  = (di->itemState & ODS_SELECTED) != 0;
            bool focused  = (di->itemState & ODS_FOCUS)    != 0;
            bool disabled = (di->itemState & ODS_DISABLED) != 0;

            // Off-screen buffer to avoid flicker
            int bw = rc.right - rc.left, bh = rc.bottom - rc.top;
            HDC     hBuf    = CreateCompatibleDC(hDC);
            HBITMAP hBufBmp = CreateCompatibleBitmap(hDC, bw, bh);
            HGDIOBJ hBufOld = SelectObject(hBuf, hBufBmp);

            RECT rcBuf = { 0, 0, bw, bh };
            COLORREF bgCol = pressed ? CLR_BTN_PRESS : CLR_BTN_BG;
            HBRUSH hBrBg = CreateSolidBrush(bgCol);
            FillRect(hBuf, &rcBuf, hBrBg);
            DeleteObject(hBrBg);

            COLORREF borderCol = focused ? CLR_BTN_FOCUS : CLR_BTN_BORDER;
            HPEN hPen = CreatePen(PS_SOLID, 1, borderCol);
            HGDIOBJ oldPen   = SelectObject(hBuf, hPen);
            HGDIOBJ oldBrush = SelectObject(hBuf, GetStockObject(NULL_BRUSH));
            Rectangle(hBuf, 0, 0, bw, bh);
            SelectObject(hBuf, oldPen);
            SelectObject(hBuf, oldBrush);
            DeleteObject(hPen);

            wchar_t text[256] = {};
            GetWindowTextW(di->hwndItem, text, 256);
            SetBkMode(hBuf, TRANSPARENT);
            SetTextColor(hBuf, disabled ? CLR_SUBTEXT : CLR_TEXT);
            HFONT hFont = reinterpret_cast<HFONT>(
                SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont = SelectObject(hBuf, hFont ? hFont : GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(hBuf, text, -1, &rcBuf, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hBuf, oldFont);

            BitBlt(hDC, rc.left, rc.top, bw, bh, hBuf, 0, 0, SRCCOPY);
            SelectObject(hBuf, hBufOld);
            DeleteObject(hBufBmp);
            DeleteDC(hBuf);
            return TRUE;
        }

        // ---- Screen preview static control ----------------------------------
        if (di->CtlType == ODT_STATIC && di->CtlID == IDC_PREVIEW_STATIC) {
            HDC  hDC = di->hDC;
            RECT rc  = di->rcItem;
            int  dw  = rc.right  - rc.left;
            int  dh  = rc.bottom - rc.top;

            // Double-buffer: render into off-screen DC, then blit once
            HDC     hBuf    = CreateCompatibleDC(hDC);
            HBITMAP hBufBmp = CreateCompatibleBitmap(hDC, dw, dh);
            HGDIOBJ hBufOld = SelectObject(hBuf, hBufBmp);
            RECT rcBuf = { 0, 0, dw, dh };

            FillRect(hBuf, &rcBuf, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

            if (!g_hasFocus) {
                // Show ":)" placeholder when unfocused – no screen capture
                SetBkMode(hBuf, TRANSPARENT);
                SetTextColor(hBuf, CLR_SUBTEXT);
                HFONT hBig = CreateFontW(dh / 2, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                HGDIOBJ oldF = SelectObject(hBuf, hBig);
                DrawTextW(hBuf, L":)", -1, &rcBuf, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hBuf, oldF);
                DeleteObject(hBig);
            } else if (g_previewBmp && !g_monitors.empty()) {
                int mi = (g_currentMonitor < static_cast<int>(g_monitors.size()))
                         ? g_currentMonitor : 0;
                const RECT& mr = g_monitors[mi];
                int sw = mr.right  - mr.left;
                int sh = mr.bottom - mr.top;
                if (sw > 0 && sh > 0) {
                    // Letterbox: preserve aspect ratio
                    double srcAsp = static_cast<double>(sw) / sh;
                    double dstAsp = static_cast<double>(dw) / dh;
                    int drawW, drawH, drawX, drawY;
                    if (dstAsp > srcAsp) {
                        drawH = dh;
                        drawW = static_cast<int>(dh * srcAsp);
                        drawX = (dw - drawW) / 2;
                        drawY = 0;
                    } else {
                        drawW = dw;
                        drawH = static_cast<int>(dw / srcAsp);
                        drawX = 0;
                        drawY = (dh - drawH) / 2;
                    }
                    HDC     hMem = CreateCompatibleDC(hBuf);
                    HGDIOBJ old  = SelectObject(hMem, g_previewBmp);
                    SetStretchBltMode(hBuf, HALFTONE);
                    SetBrushOrgEx(hBuf, 0, 0, nullptr);
                    StretchBlt(hBuf, drawX, drawY, drawW, drawH,
                               hMem, 0, 0, sw, sh, SRCCOPY);
                    SelectObject(hMem, old);
                    DeleteDC(hMem);
                }
            }

            // Single blit to the real DC – no intermediate flash
            BitBlt(hDC, rc.left, rc.top, dw, dh, hBuf, 0, 0, SRCCOPY);
            SelectObject(hBuf, hBufOld);
            DeleteObject(hBufBmp);
            DeleteDC(hBuf);
            return TRUE;
        }
        break;
    }

    // --------------------------------------------------------------------
    // Dark theme: dialog background
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(g_hbrBg);

    // --------------------------------------------------------------------
    // Dark theme: static text and group-box labels
    case WM_CTLCOLORSTATIC:
    {
        HDC  hDC   = reinterpret_cast<HDC>(wParam);
        HWND hCtrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hDC, TRANSPARENT);
        int ctlId = GetDlgCtrlID(hCtrl);
        if (ctlId == IDC_PREVIEW_SUBTEXT ||
            ctlId == IDC_HIDE_APPS_SUB  ||
            ctlId == IDC_STATUS_TEXT)
        {
            SetTextColor(hDC, CLR_SUBTEXT);
        } else {
            SetTextColor(hDC, CLR_TEXT);
        }
        return reinterpret_cast<INT_PTR>(g_hbrBg);
    }

    // --------------------------------------------------------------------
    // Dark theme: button / group-box backgrounds
    case WM_CTLCOLORBTN:
    {
        HDC hDC = reinterpret_cast<HDC>(wParam);
        SetBkMode(hDC, TRANSPARENT);
        SetTextColor(hDC, CLR_TEXT);
        return reinterpret_cast<INT_PTR>(g_hbrBg);
    }

    // --------------------------------------------------------------------
    // Dark theme: edit control (text box) background
    case WM_CTLCOLOREDIT:
    {
        HDC hDC = reinterpret_cast<HDC>(wParam);
        SetBkColor(hDC, CLR_LIST_BG);
        SetTextColor(hDC, CLR_TEXT);
        return reinterpret_cast<INT_PTR>(g_hbrListBg);
    }

    // --------------------------------------------------------------------
    case WM_TRAYICON:
    {
        switch (lParam)
        {
        case WM_RBUTTONUP:
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW,
                IsWindowVisible(hDlg) ? L"Hide Window" : L"Show Window");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                IDM_TRAY_AUTOSTART, L"Start on Boot");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
            SetForegroundWindow(hDlg);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hDlg, nullptr);
            DestroyMenu(hMenu);
            break;
        }
        case WM_LBUTTONDBLCLK:
            ShowWindow(hDlg, SW_SHOW);
            SetForegroundWindow(hDlg);
            break;
        }
        return TRUE;
    }

    // --------------------------------------------------------------------
    case WM_SIZE:
        OnSize(hDlg, LOWORD(lParam), HIWORD(lParam));
        return TRUE;

    // --------------------------------------------------------------------
    case WM_GETMINMAXINFO:
    {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize = { 360, 700 };
        return TRUE;
    }

    // --------------------------------------------------------------------
    case WM_NOTIFY:
    {
        auto* pNMHDR = reinterpret_cast<LPNMHDR>(lParam);

        // Tab control: switch preview monitor
        if (pNMHDR->idFrom == IDC_TAB_SCREENS &&
            pNMHDR->code   == TCN_SELCHANGE)
        {
            g_currentMonitor = TabCtrl_GetCurSel(
                GetDlgItem(hDlg, IDC_TAB_SCREENS));
            // Invisiwind: clicking a monitor tab sends CaptureWorkerEvent::Capture(*monitor)
            if (g_showDesktopPreview)
                SendCaptureEvent(g_currentMonitor);
        }

        // Main window list notifications
        if (pNMHDR->idFrom == IDC_WINDOW_LIST) {

            if (pNMHDR->code == LVN_ITEMCHANGED && !g_populatingList) {
                auto* pnm = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (pnm->uChanged & LVIF_STATE) {
                    // Detect checkbox toggle (state-image index changed)
                    UINT oldImg = (pnm->uOldState & LVIS_STATEIMAGEMASK)
                                  >> STATE_IMAGE_SHIFT;
                    UINT newImg = (pnm->uNewState & LVIS_STATEIMAGEMASK)
                                  >> STATE_IMAGE_SHIFT;
                    if (oldImg != newImg &&
                        oldImg != 0 && newImg != 0 &&
                        pnm->iItem >= 0 &&
                        pnm->iItem < static_cast<int>(g_windows.size()))
                    {
                        bool shouldExclude = (newImg == STATE_IMAGE_CHECKED);
                        const WindowInfo& w = g_windows[pnm->iItem];
                        DWORD affinity = shouldExclude ? 0x00000011u : 0x00000000u;
                        SetStatus(hDlg, L"Injecting \u2026");
                        if (InjectWDASetAffinity(w.hwnd, affinity, g_autoUnloadDll)) {
                            SetStatus(hDlg, shouldExclude
                                ? L"ExcludeCapture enabled: \"" + w.title + L"\""
                                : L"ExcludeCapture disabled: \"" + w.title + L"\"");
                        } else {
                            DWORD err = GetLastError();
                            std::wstring errMsg =
                                L"Injection failed (error "
                                + std::to_wstring(err)
                                + L"). Run as Administrator, ensure "
                                  L"wda_inject_x64.dll / wda_inject_x86.dll and "
                                  L"wda_launcher_x86.exe / wda_launcher_x64.exe "
                                  L"are beside the exe. Check window_mod.log for details.";
                            SetStatus(hDlg, errMsg);
                            // Revert the checkbox
                            g_populatingList = true;
                            HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                            ListView_SetCheckState(hList, pnm->iItem,
                                !shouldExclude ? TRUE : FALSE);
                            g_populatingList = false;
                        }
                    }
                    // Selection change → update info label
                    if (pnm->uNewState & LVIS_SELECTED)
                        UpdateSelectedInfo(hDlg);
                }

            } else if (pNMHDR->code == NM_CLICK) {
                // Click on TopMost column (col 2) toggles TopMost inline
                auto* pIA = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (pIA->iItem >= 0 &&
                    pIA->iItem < static_cast<int>(g_windows.size()) &&
                    pIA->iSubItem == 2)
                {
                    const WindowInfo& w = g_windows[pIA->iItem];
                    bool newState = !IsWindowTopMost(w.hwnd);
                    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                    if (SetWindowTopMost(w.hwnd, newState)) {
                        g_populatingList = true;
                        ListView_SetItemText(hList, pIA->iItem, 2,
                            const_cast<LPWSTR>(newState ? L"\u2713" : L""));
                        g_populatingList = false;
                        SetStatus(hDlg, newState
                            ? L"Set TOPMOST: \""    + w.title + L"\""
                            : L"Removed TOPMOST: \"" + w.title + L"\"");
                    }
                }
            }
        }
        break;
    }

    // --------------------------------------------------------------------
    // Right-click context menu on the window list
    case WM_CONTEXTMENU:
    {
        HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
        if (reinterpret_cast<HWND>(wParam) != hList) break;

        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (sel < 0 || sel >= static_cast<int>(g_windows.size())) break;

        const WindowInfo& w = g_windows[sel];
        bool isHid     = w.isHidden;
        bool isTopMost = !isHid && IsWindowTopMost(w.hwnd);
        bool isExclude = !isHid && IsWindowExcludeFromCapture(w.hwnd);

        HMENU hMenu = CreatePopupMenu();
        if (isHid) {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_SHOW_WINDOW,   L"Show");
        } else {
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_HIDE_WINDOW,   L"Hide");
        }
        AppendMenuW(hMenu, MF_STRING | (isTopMost ? MF_CHECKED : 0),
                    IDM_CTX_TOPMOST, L"TopMost");
        AppendMenuW(hMenu, MF_STRING | (isExclude ? MF_CHECKED : 0),
                    IDM_CTX_EXCLUDE, L"Exclude from capture");
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_WATCH,             L"Watch");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CTX_UNLOAD_DLL,        L"Unload DLL");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hDlg);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hDlg, nullptr);
        DestroyMenu(hMenu);

        // Re-fetch since the selection may have changed
        if (sel < 0 || sel >= static_cast<int>(g_windows.size())) break;
        WindowInfo& wi = g_windows[sel];

        switch (cmd)
        {
        case IDM_CTX_HIDE_WINDOW:
        {
            // Check not already hidden
            for (const auto& h : g_hiddenWindows)
                if (h.hwnd == wi.hwnd) goto done_ctx_hide;
            if (HideWindow(wi.hwnd)) {
                wi.isHidden = true;
                g_hiddenWindows.push_back(wi);
                // Update the hidden column in place (no full re-enumeration)
                g_populatingList = true;
                ListView_SetItemText(hList, sel, 3,
                    const_cast<LPWSTR>(L"\u25cf"));
                // Clear checkbox for hidden windows
                ListView_SetCheckState(hList, sel, FALSE);
                g_populatingList = false;
                SetStatus(hDlg, L"Hidden: \"" + wi.title + L"\"");
            } else {
                SetStatus(hDlg, L"Failed to hide window.");
            }
        done_ctx_hide:;
            break;
        }

        case IDM_CTX_SHOW_WINDOW:
        {
            if (!IsWindow(wi.hwnd)) {
                SetStatus(hDlg, L"Window no longer exists.");
                // Remove from hidden list
                wi.isHidden = false;
                g_hiddenWindows.erase(
                    std::remove_if(g_hiddenWindows.begin(), g_hiddenWindows.end(),
                        [&wi](const WindowInfo& h){ return h.hwnd == wi.hwnd; }),
                    g_hiddenWindows.end());
                ListView_SetItemText(hList, sel, 3, const_cast<LPWSTR>(L""));
                break;
            }
            if (ShowWindowRestore(wi.hwnd)) {
                wi.isHidden = false;
                g_hiddenWindows.erase(
                    std::remove_if(g_hiddenWindows.begin(), g_hiddenWindows.end(),
                        [&wi](const WindowInfo& h){ return h.hwnd == wi.hwnd; }),
                    g_hiddenWindows.end());
                g_populatingList = true;
                ListView_SetItemText(hList, sel, 3, const_cast<LPWSTR>(L""));
                // Restore TopMost column
                ListView_SetItemText(hList, sel, 2,
                    const_cast<LPWSTR>(IsWindowTopMost(wi.hwnd) ? L"\u2713" : L""));
                g_populatingList = false;
                SetStatus(hDlg, L"Restored: \"" + wi.title + L"\"");
                UpdateSelectedInfo(hDlg);
            } else {
                SetStatus(hDlg, L"Failed to show window.");
            }
            break;
        }

        case IDM_CTX_TOPMOST:
        {
            bool newState = !IsWindowTopMost(wi.hwnd);
            if (SetWindowTopMost(wi.hwnd, newState)) {
                g_populatingList = true;
                ListView_SetItemText(hList, sel, 2,
                    const_cast<LPWSTR>(newState ? L"\u2713" : L""));
                g_populatingList = false;
                SetStatus(hDlg, newState
                    ? L"Set TOPMOST: \""     + wi.title + L"\""
                    : L"Removed TOPMOST: \"" + wi.title + L"\"");
            }
            break;
        }

        case IDM_CTX_EXCLUDE:
        {
            bool newExclude = !IsWindowExcludeFromCapture(wi.hwnd);
            DWORD affinity = newExclude ? 0x00000011u : 0x00000000u;
            SetStatus(hDlg, L"Injecting \u2026");
            if (InjectWDASetAffinity(wi.hwnd, affinity, g_autoUnloadDll)) {
                // Sync the checkbox
                g_populatingList = true;
                ListView_SetCheckState(hList, sel, newExclude ? TRUE : FALSE);
                g_populatingList = false;
                SetStatus(hDlg, newExclude
                    ? L"ExcludeCapture enabled: \""  + wi.title + L"\""
                    : L"ExcludeCapture disabled: \"" + wi.title + L"\"");
            } else {
                SetStatus(hDlg, L"Injection failed (error "
                          + std::to_wstring(GetLastError()) + L").");
            }
            break;
        }

        case IDM_CTX_WATCH:
        {
            std::wstring name = wi.processName;
            if (name.empty()) { SetStatus(hDlg, L"No process name available."); break; }
            {
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                for (const auto& e : g_watchedExeNames)
                    if (_wcsicmp(e.c_str(), name.c_str()) == 0) {
                        SetStatus(hDlg, L"Already watching: " + name);
                        goto done_ctx_watch;
                    }
                g_watchedExeNames.push_back(name);
            }
            {
                HWND hWatchList = GetDlgItem(hDlg, IDC_WATCH_LIST);
                LVITEMW lvi = {};
                lvi.mask    = LVIF_TEXT;
                lvi.iItem   = ListView_GetItemCount(hWatchList);
                lvi.pszText = const_cast<LPWSTR>(name.c_str());
                ListView_InsertItem(hWatchList, &lvi);
            }
            SetStatus(hDlg, L"Watching: " + name);
            SaveSettings();
        done_ctx_watch:;
            break;
        }

        case IDM_CTX_UNLOAD_DLL:
        {
            SetStatus(hDlg, L"Unloading DLL \u2026");
            if (UnloadInjectedDll(wi.hwnd)) {
                SetStatus(hDlg, L"DLL unloaded from: \"" + wi.title + L"\"");
            } else {
                SetStatus(hDlg, L"Unload failed (error "
                          + std::to_wstring(GetLastError())
                          + L"). Run as Administrator and check window_mod.log.");
            }
            break;
        }
        }
        return TRUE;
    }

    // --------------------------------------------------------------------
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        switch (id)
        {
        case IDC_CHK_AUTO_UNLOAD:
            g_autoUnloadDll =
                (IsDlgButtonChecked(hDlg, IDC_CHK_AUTO_UNLOAD) == BST_CHECKED);
            break;

        case IDC_CHK_SHOW_PREVIEW:
        {
            // Invisiwind: toggling "Show desktop preview" sends Capture or StopCapture.
            g_showDesktopPreview =
                (IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_PREVIEW) == BST_CHECKED);
            // Show or hide preview-related controls based on the new state
            int sw = g_showDesktopPreview ? SW_SHOW : SW_HIDE;
            ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_SUBTEXT), sw);
            ShowWindow(GetDlgItem(hDlg, IDC_PREVIEW_STATIC),  sw);
            ShowWindow(GetDlgItem(hDlg, IDC_TAB_SCREENS),     sw);
            ShowWindow(GetDlgItem(hDlg, IDC_CHK_SHOW_CURSOR), sw);
            // Relayout so the window list expands/contracts accordingly
            {
                RECT rc; GetClientRect(hDlg, &rc);
                OnSize(hDlg, rc.right, rc.bottom);
            }
            if (g_showDesktopPreview) {
                SendCaptureEvent(g_currentMonitor);
            } else {
                // Clear existing preview bitmap immediately.
                if (g_previewBmp) {
                    DeleteObject(g_previewBmp);
                    g_previewBmp = nullptr;
                }
                SendStopCaptureEvent();
            }
            SaveSettings();
            break;
        }

        case IDC_CHK_SHOW_CURSOR:
            // Keep the atomic in sync so the capture worker reads the new value
            // on the very next frame without any channel round-trip.
            g_captureShowCursor.store(
                IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_CURSOR) == BST_CHECKED);
            SaveSettings();
            break;

        case IDC_BTN_WATCH_ADD:
        {
            wchar_t buf[MAX_PATH] = {};
            GetDlgItemTextW(hDlg, IDC_WATCH_EDIT, buf, MAX_PATH);
            std::wstring name(buf);
            // Trim whitespace
            while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
            while (!name.empty() && iswspace(name.back()))  name.pop_back();
            if (name.empty()) { SetStatus(hDlg, L"Enter an exe name to watch."); break; }

            // Deduplicate (case-insensitive)
            {
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                for (const auto& e : g_watchedExeNames)
                    if (_wcsicmp(e.c_str(), name.c_str()) == 0) {
                        SetStatus(hDlg, L"Already watching: " + name);
                        goto done_watch_add;
                    }
                g_watchedExeNames.push_back(name);
            }
            {
                HWND hList = GetDlgItem(hDlg, IDC_WATCH_LIST);
                LVITEMW lvi = {};
                lvi.mask    = LVIF_TEXT;
                lvi.iItem   = ListView_GetItemCount(hList);
                lvi.pszText = const_cast<LPWSTR>(name.c_str());
                ListView_InsertItem(hList, &lvi);
            }
            SetDlgItemTextW(hDlg, IDC_WATCH_EDIT, L"");
            SetStatus(hDlg, L"Watching: " + name);
            SaveSettings();
        done_watch_add:;
            break;
        }

        case IDC_BTN_WATCH_REMOVE:
        {
            HWND hList = GetDlgItem(hDlg, IDC_WATCH_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0) { SetStatus(hDlg, L"No entry selected."); break; }
            {
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                if (sel < static_cast<int>(g_watchedExeNames.size()))
                    g_watchedExeNames.erase(g_watchedExeNames.begin() + sel);
            }
            ListView_DeleteItem(hList, sel);
            SetStatus(hDlg, L"Watch entry removed.");
            SaveSettings();
            break;
        }

        case IDM_TRAY_SHOW:
            if (IsWindowVisible(hDlg)) {
                ShowWindow(hDlg, SW_HIDE);
            } else {
                ShowWindow(hDlg, SW_SHOW);
                SetForegroundWindow(hDlg);
            }
            break;

        case IDM_TRAY_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            break;

        case IDM_TRAY_EXIT:
            RestoreAllHiddenWindows();
            DestroyTrayIcon();
            EndDialog(hDlg, 0);
            break;

        case IDCANCEL:
            // ESC or dialog cancel – hide to tray instead of closing.
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        break;
    }

    // --------------------------------------------------------------------
    // Process watch timer: trigger a watch-check in the injector thread.
    case WM_TIMER:
        if (wParam == IDT_WATCH) {
            bool hasEntries;
            {
                std::lock_guard<std::mutex> lk(g_watchedExeMutex);
                hasEntries = !g_watchedExeNames.empty();
            }
            if (hasEntries)
                g_injectorChannel.send(InjectorEvent{InjectorEventType::WatchCheck});
        }
        return TRUE;

    // --------------------------------------------------------------------
    // Injector thread: watch rule applied – update status bar.
    case WM_APP_WATCH_APPLIED:
    {
        WPARAM count = wParam;
        SetStatus(hDlg, L"Watch: applied ExcludeCapture to "
                        + std::to_wstring(count)
                        + (count == 1 ? L" new process." : L" new processes."));
        // Refresh the window list so the checkboxes reflect the new state.
        g_injectorChannel.send(InjectorEvent{InjectorEventType::Update});
        return TRUE;
    }

    // --------------------------------------------------------------------
    // Injector thread: window list ready – swap and refresh the ListView.
    case WM_APP_WINDOWS_READY:
    {
        std::vector<WindowInfo> newWindows;
        {
            std::lock_guard<std::mutex> lk(g_pendingWindowsMutex);
            newWindows = std::move(g_pendingWindows);
        }
        // Build set of enumerated HWNDs
        std::set<HWND> enumSet;
        for (const auto& w : newWindows) enumSet.insert(w.hwnd);

        // Check if any hidden windows were shown by external means
        // (they appear in the new enumeration → remove from g_hiddenWindows).
        g_hiddenWindows.erase(
            std::remove_if(g_hiddenWindows.begin(), g_hiddenWindows.end(),
                [&enumSet](const WindowInfo& h){ return enumSet.count(h.hwnd) > 0; }),
            g_hiddenWindows.end());

        // Append hidden windows (not in enumeration) with isHidden=true
        g_windows = std::move(newWindows);
        for (const auto& h : g_hiddenWindows) {
            WindowInfo hi = h;
            hi.isHidden = true;
            g_windows.push_back(hi);
        }

        PopulateWindowList(hDlg);
        UpdateSelectedInfo(hDlg);
        return TRUE;
    }

    // --------------------------------------------------------------------
    // Capture thread: new preview bitmap ready – swap and repaint.
    case WM_APP_PREVIEW_READY:
    {
        HBITMAP newBmp = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_pendingPreviewMutex);
            newBmp = g_pendingPreviewBmp;
            g_pendingPreviewBmp = nullptr;
        }
        if (newBmp) {
            if (g_previewBmp) DeleteObject(g_previewBmp);
            g_previewBmp = newBmp;
            if (HWND hPrev = GetDlgItem(hDlg, IDC_PREVIEW_STATIC))
                InvalidateRect(hPrev, nullptr, FALSE);
        }
        return TRUE;
    }

    // --------------------------------------------------------------------
    case WM_DESTROY:
        KillTimer(hDlg, IDT_WATCH);
        // Shut down worker threads cleanly before releasing GDI resources.
        g_hDlg = nullptr;   // prevent PostMessage from racing during teardown
        g_injectorChannel.send(InjectorEvent{InjectorEventType::Quit});
        g_captureChannel.send(CaptureEvent{CaptureEventType::Quit});
        if (g_injectorThread.joinable()) g_injectorThread.join();
        if (g_captureThread.joinable())  g_captureThread.join();
        // Clean up any pending preview bitmap that was never consumed.
        {
            std::lock_guard<std::mutex> lk(g_pendingPreviewMutex);
            if (g_pendingPreviewBmp) { DeleteObject(g_pendingPreviewBmp); g_pendingPreviewBmp = nullptr; }
        }
        if (g_hbrBg)            { DeleteObject(g_hbrBg);            g_hbrBg            = nullptr; }
        if (g_hbrListBg)        { DeleteObject(g_hbrListBg);        g_hbrListBg        = nullptr; }
        if (g_hFontBold)        { DeleteObject(g_hFontBold);        g_hFontBold        = nullptr; }
        if (g_hFontPlaceholder) { DeleteObject(g_hFontPlaceholder); g_hFontPlaceholder = nullptr; }
        if (g_previewBmp)       { DeleteObject(g_previewBmp);       g_previewBmp       = nullptr; }
        // Release the image list attached to the window list view.
        if (HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST)) {
            HIMAGELIST hImgList = ListView_GetImageList(hList, LVSIL_SMALL);
            if (hImgList) ImageList_Destroy(hImgList);
        }
        break;

    // --------------------------------------------------------------------
    case WM_CLOSE:
        // Close button hides to tray; actual exit is via tray menu "Exit".
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}

