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
#include <algorithm>
#include <string>
#include <vector>
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
enum class InjectorEventType { Update, Quit };
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

// Dark theme GDI resources
static HBRUSH g_hbrBg            = nullptr;
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
        // InjectorEventType::Update
        auto windows = EnumerateWindows();
        {
            std::lock_guard<std::mutex> lk(g_pendingWindowsMutex);
            g_pendingWindows = std::move(windows);
        }
        if (g_hDlg)
            PostMessage(g_hDlg, WM_APP_WINDOWS_READY, 0, 0);
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

static void InitListViewColumns(HWND hList)
{
    static const struct { const wchar_t* name; int cx; } cols[] = {
        { L"Title",   200 },
        { L"Process",  90 },
        { L"PID",      50 },
        { L"Handle",   80 },
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
// Main window list: Title, Process, TopMost columns.
// ExcludeCapture is represented by the LVS_EX_CHECKBOXES checkbox.

static void InitMainListViewColumns(HWND hList)
{
    static const struct { const wchar_t* name; int cx; } cols[] = {
        { L"Title",   220 },
        { L"Process",  90 },
        { L"TopMost",  60 },
    };
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (int i = 0; i < 3; ++i) {
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

        // ExcludeCapture state = checkbox state
        ListView_SetCheckState(hList, i,
            IsWindowExcludeFromCapture(w.hwnd) ? TRUE : FALSE);
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
    IDC_GRP_OPS, IDC_BTN_HIDE, IDC_BTN_TOPMOST, IDC_GRP_HIDDEN, IDC_HIDDEN_LIST,
    IDC_BTN_SHOW, IDC_STATUS_TEXT, IDC_CHK_SHOW_CURSOR,
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
}

// ---------------------------------------------------------------------------
// Sync the hidden-windows list.

static void PopulateHiddenList(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_HIDDEN_LIST);
    ListView_DeleteAllItems(hList);

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;

    for (int i = 0; i < static_cast<int>(g_hiddenWindows.size()); ++i) {
        const auto& w = g_hiddenWindows[i];

        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.lParam   = static_cast<LPARAM>(i);
        lvi.pszText  = const_cast<LPWSTR>(w.title.c_str());
        ListView_InsertItem(hList, &lvi);

        ListView_SetItemText(hList, i, 1,
            const_cast<LPWSTR>(w.processName.c_str()));

        auto pidStr = std::to_wstring(w.pid);
        ListView_SetItemText(hList, i, 2,
            const_cast<LPWSTR>(pidStr.c_str()));

        auto hndStr = FmtHandle(w.hwnd);
        ListView_SetItemText(hList, i, 3,
            const_cast<LPWSTR>(hndStr.c_str()));
    }
}

// ---------------------------------------------------------------------------
// Restore every window currently in the hidden list (called on exit).

static void RestoreAllHiddenWindows()
{
    for (auto& w : g_hiddenWindows) {
        if (IsWindow(w.hwnd))
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
    HICON hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    const int statusH  = 16;
    const int hidBtnH  = btnH;
    const int hidListH = 80;
    const int hidLblH  = lblH;
    const int opsH     = 30;

    int y = H - mY;

    int statusY  = y - statusH;   y = statusY  - DY;
    int hidBtnY  = y - hidBtnH;   y = hidBtnY  - DY;
    int hidListY = y - hidListH;  y = hidListY - DY;
    int hidLblY  = y - hidLblH;   y = hidLblY  - DY;
    int opsY     = y - opsH;      y = opsY     - DY;
    int selInfoY = y - lblH;      y = selInfoY - DY;

    // --- Top zone (computed top-down) ---
    int top = mY;

    int prevLblY = top;  top += bigH + 2;
    int prevSubY = top;  top += subH + DY;
    int previewY = top;
    // Window list fills the gap between top and bottom zones
    int previewH = std::max(PREVIEW_H_MIN,
                            std::min(PREVIEW_H_MAX, H * PREVIEW_H_PCT / 100));
    top += previewH + DY;
    int tabY     = top;  top += 22 + DY;
    int prevChkY = top;  top += 14 + DY;  // "Show desktop preview" checkbox
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

    // Preview section – label on the left, cursor checkbox on the right
    static const int chkW = 140;
    Move(IDC_PREVIEW_LABEL,   mX, prevLblY, listW - chkW - 4, bigH);
    Move(IDC_CHK_SHOW_CURSOR, mX + listW - chkW, prevLblY, chkW, bigH);
    Move(IDC_PREVIEW_SUBTEXT, mX, prevSubY, listW, subH);
    Move(IDC_PREVIEW_STATIC,  mX, previewY, listW, previewH);
    Move(IDC_TAB_SCREENS,     mX, tabY,     listW, 22);
    Move(IDC_CHK_SHOW_PREVIEW, mX, prevChkY, listW, 14);

    // Hide applications section
    Move(IDC_HIDE_APPS_LABEL, mX, hideAppY, listW, bigH);
    Move(IDC_HIDE_APPS_SUB,   mX, hideSubY, listW, subH);

    // Window list
    Move(IDC_WINDOW_LIST, mX, listY, listW, listH);

    // Selected info label
    Move(IDC_SELECTED_INFO, mX, selInfoY, listW, lblH);

    // Operations group + buttons
    if (HWND hGrp = GetDlgItem(hDlg, IDC_GRP_OPS))
        MoveWindow(hGrp, mX, opsY, listW, opsH, FALSE);
    {
        const int bx = mX + 8;
        const int by = opsY + (opsH - btnH) / 2;
        Move(IDC_BTN_HIDE,    bx,        by,  90, btnH);
        Move(IDC_BTN_TOPMOST, bx + 96,   by,  90, btnH);
    }

    // Hidden windows section
    Move(IDC_GRP_HIDDEN,  mX, hidLblY,  listW, hidLblH + 2);
    Move(IDC_HIDDEN_LIST, mX, hidListY, listW, hidListH);
    Move(IDC_BTN_SHOW,    mX, hidBtnY,  110,   hidBtnH);

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

        // Dark title bar (Windows 10 v2004+ uses value 20; older builds used 19)
        BOOL dark = TRUE;
        HRESULT hr = DwmSetWindowAttribute(hDlg,
            20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        if (FAILED(hr))
            DwmSetWindowAttribute(hDlg, 19, &dark, sizeof(dark));

        // Dark theme brush
        g_hbrBg = CreateSolidBrush(CLR_BG);

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
            const int dlgW = 480, dlgH = 680;
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

        // Init hidden list view
        {
            HWND hList = GetDlgItem(hDlg, IDC_HIDDEN_LIST);
            InitListViewColumns(hList);
            ListView_SetExtendedListViewStyle(hList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
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
        for (int btnId : {IDC_BTN_HIDE, IDC_BTN_TOPMOST, IDC_BTN_SHOW}) {
            HWND hBtn = GetDlgItem(hDlg, btnId);
            LONG_PTR style = GetWindowLongPtrW(hBtn, GWL_STYLE);
            style = (style & ~static_cast<LONG_PTR>(BS_TYPEMASK))
                  | static_cast<LONG_PTR>(BS_OWNERDRAW);
            SetWindowLongPtrW(hBtn, GWL_STYLE, style);
        }

        // "Show desktop preview" checkbox – default on
        g_showDesktopPreview = true;
        CheckDlgButton(hDlg, IDC_CHK_SHOW_PREVIEW,
                       g_showDesktopPreview ? BST_CHECKED : BST_UNCHECKED);
        // "Show cursor in preview" checkbox – default off; sync the atomic.
        g_captureShowCursor.store(
            IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_CURSOR) == BST_CHECKED);

        // Tray icon
        CreateTrayIcon(hDlg);

        // Start the two background worker threads.
        g_injectorThread = std::thread(InjectorWorkerProc);
        g_captureThread  = std::thread(CaptureWorkerProc);

        // Request initial window enumeration (async).
        g_injectorChannel.send(InjectorEvent{InjectorEventType::Update});

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
        mmi->ptMinTrackSize = { 360, 560 };
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
                        if (InjectWDASetAffinity(w.hwnd, affinity)) {
                            SetStatus(hDlg, shouldExclude
                                ? L"ExcludeCapture enabled: \"" + w.title + L"\""
                                : L"ExcludeCapture disabled: \"" + w.title + L"\"");
                        } else {
                            DWORD err = GetLastError();
                            std::wstring errMsg;
                            if (err == ERROR_EXE_MACHINE_TYPE_MISMATCH) {
#ifdef _WIN64
                                errMsg = L"Injection failed: architecture mismatch \u2013 "
                                         L"target is a 32-bit process. "
                                         L"Use the x86 build of window_mod for 32-bit targets.";
#else
                                errMsg = L"Injection failed: architecture mismatch \u2013 "
                                         L"target is a 64-bit process. "
                                         L"Use the x64 build of window_mod for 64-bit targets.";
#endif
                            } else {
                                errMsg = L"Injection failed (error "
                                    + std::to_wstring(err)
                                    + L"). Run as Administrator, ensure "
                                      L"wda_inject.dll is beside the exe, "
                                      L"and check window_mod.log for details.";
                            }
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
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        switch (id)
        {
        case IDC_BTN_HIDE:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }

            for (auto& h : g_hiddenWindows)
                if (h.hwnd == w->hwnd) {
                    SetStatus(hDlg, L"Already in hidden list.");
                    goto done_hide;
                }

            if (HideWindow(w->hwnd)) {
                g_hiddenWindows.push_back(*w);
                PopulateHiddenList(hDlg);
                // Immediate sync re-enumeration (user action, needs instant feedback).
                g_windows = EnumerateWindows();
                PopulateWindowList(hDlg);
                UpdateSelectedInfo(hDlg);
                SetStatus(hDlg, L"Hidden: \"" + w->title + L"\"");
            } else {
                SetStatus(hDlg, L"Failed to hide window.");
            }
        done_hide:;
            break;
        }

        case IDC_BTN_TOPMOST:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            bool newState = !IsWindowTopMost(w->hwnd);
            if (SetWindowTopMost(w->hwnd, newState)) {
                HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (sel >= 0) {
                    g_populatingList = true;
                    ListView_SetItemText(hList, sel, 2,
                        const_cast<LPWSTR>(newState ? L"\u2713" : L""));
                    g_populatingList = false;
                }
                SetStatus(hDlg, newState
                    ? L"Set TOPMOST: \""    + w->title + L"\""
                    : L"Removed TOPMOST: \"" + w->title + L"\"");
            }
            break;
        }

        case IDC_BTN_SHOW:
        {
            HWND hList = GetDlgItem(hDlg, IDC_HIDDEN_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= static_cast<int>(g_hiddenWindows.size())) {
                SetStatus(hDlg, L"No hidden window selected.");
                break;
            }
            const WindowInfo& w = g_hiddenWindows[sel];
            if (!IsWindow(w.hwnd)) {
                SetStatus(hDlg, L"Window no longer exists.");
                g_hiddenWindows.erase(g_hiddenWindows.begin() + sel);
                PopulateHiddenList(hDlg);
                break;
            }
            if (ShowWindowRestore(w.hwnd)) {
                SetStatus(hDlg, L"Restored: \"" + w.title + L"\"");
                g_hiddenWindows.erase(g_hiddenWindows.begin() + sel);
                PopulateHiddenList(hDlg);
                // Immediate sync re-enumeration (user action, needs instant feedback).
                g_windows = EnumerateWindows();
                PopulateWindowList(hDlg);
                UpdateSelectedInfo(hDlg);
            } else {
                SetStatus(hDlg, L"Failed to show window.");
            }
            break;
        }

        case IDC_CHK_SHOW_PREVIEW:
        {
            // Invisiwind: toggling "Show desktop preview" sends Capture or StopCapture.
            g_showDesktopPreview =
                (IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_PREVIEW) == BST_CHECKED);
            if (g_showDesktopPreview) {
                SendCaptureEvent(g_currentMonitor);
            } else {
                // Clear existing preview bitmap immediately.
                if (g_previewBmp) {
                    DeleteObject(g_previewBmp);
                    g_previewBmp = nullptr;
                }
                if (HWND hPrev = GetDlgItem(hDlg, IDC_PREVIEW_STATIC))
                    InvalidateRect(hPrev, nullptr, FALSE);
                SendStopCaptureEvent();
            }
            break;
        }

        case IDC_CHK_SHOW_CURSOR:
            // Keep the atomic in sync so the capture worker reads the new value
            // on the very next frame without any channel round-trip.
            g_captureShowCursor.store(
                IsDlgButtonChecked(hDlg, IDC_CHK_SHOW_CURSOR) == BST_CHECKED);
            break;

        case IDM_TRAY_SHOW:
            if (IsWindowVisible(hDlg)) {
                ShowWindow(hDlg, SW_HIDE);
            } else {
                ShowWindow(hDlg, SW_SHOW);
                SetForegroundWindow(hDlg);
            }
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
    // Injector thread: window list ready – swap and refresh the ListView.
    case WM_APP_WINDOWS_READY:
    {
        {
            std::lock_guard<std::mutex> lk(g_pendingWindowsMutex);
            g_windows = std::move(g_pendingWindows);
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

