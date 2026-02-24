/**
 * main.cpp  –  Windows window modifier tool
 *
 * Features:
 *  - Enumerate visible windows (title + process name)
 *  - Auto-refresh list when focused; show ":)" placeholder when unfocused
 *  - TopMost and ExcludeCapture columns with inline click-to-toggle support
 *  - Hide windows (tracked in a separate list for recovery)
 *  - Inject wda_inject.dll to set/clear WDA_EXCLUDEFROMCAPTURE
 *  - System tray icon: close button hides to tray; exit only via tray menu
 *  - Restore all hidden windows when exiting
 */

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

#include "resource.h"
#include "window_list.h"
#include "window_ops.h"
#include "injector.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// ============================================================================
// State
// ============================================================================
static HINSTANCE g_hInst       = nullptr;
static HWND      g_hDlg        = nullptr;

static std::vector<WindowInfo> g_windows;       // current window snapshot
static std::vector<WindowInfo> g_hiddenWindows; // windows we've hidden

// Tray icon
static NOTIFYICONDATA g_nid       = {};
static bool           g_trayAdded = false;

// ============================================================================
// Forward declarations
// ============================================================================
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// WinMain
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInstance;

    // Enable visual styles (Common Controls v6)
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES;
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
// ListView column setup for the main window list (6 columns: +TopMost +Excl.Cap).

static void InitMainListViewColumns(HWND hList)
{
    InitListViewColumns(hList);

    static const struct { const wchar_t* name; int cx; int idx; } extra[] = {
        { L"TopMost",  60, 4 },
        { L"Excl.Cap", 60, 5 },
    };
    LVCOLUMNW lvc = {};
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (auto& c : extra) {
        lvc.cx       = c.cx;
        lvc.pszText  = const_cast<LPWSTR>(c.name);
        lvc.iSubItem = c.idx;
        ListView_InsertColumn(hList, c.idx, &lvc);
    }
}

// ---------------------------------------------------------------------------
// Populate (or refresh) the main window list.
// If preserveSelection is true the currently-selected HWND is re-selected after
// the refresh (used for timer-driven updates).

static void PopulateWindowList(HWND hDlg, bool preserveSelection = false)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);

    // Remember selected HWND before clearing
    HWND selHwnd = nullptr;
    if (preserveSelection) {
        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (sel >= 0 && sel < static_cast<int>(g_windows.size()))
            selHwnd = g_windows[sel].hwnd;
    }

    ListView_DeleteAllItems(hList);
    g_windows = EnumerateWindows(hDlg); // skip our own dialog

    LVITEMW lvi = {};
    lvi.mask = LVIF_TEXT | LVIF_PARAM;

    for (int i = 0; i < static_cast<int>(g_windows.size()); ++i) {
        const auto& w = g_windows[i];

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

        // TopMost column (click to toggle)
        ListView_SetItemText(hList, i, 4,
            const_cast<LPWSTR>(IsWindowTopMost(w.hwnd) ? L"\u2713" : L""));

        // ExcludeCapture column (click to toggle via injection)
        ListView_SetItemText(hList, i, 5,
            const_cast<LPWSTR>(IsWindowExcludeFromCapture(w.hwnd) ? L"\u2713" : L""));
    }

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
// Show a ":)" placeholder in the window list when the app loses focus.

static void ShowPlaceholder(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
    ListView_DeleteAllItems(hList);
    g_windows.clear();

    LVITEMW lvi = {};
    lvi.mask    = LVIF_TEXT;
    lvi.iItem   = 0;
    lvi.pszText = const_cast<LPWSTR>(L":)");
    ListView_InsertItem(hList, &lvi);

    SetDlgItemTextW(hDlg, IDC_SELECTED_INFO, L"Selected: (none)");
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
// Return the WindowInfo for the currently selected row (main list),
// or nullptr if nothing is selected.

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
// WM_SIZE – resize/move controls to fill the dialog (all values in pixels)
// ============================================================================

static void OnSize(HWND hDlg, int /*cx*/, int /*cy*/)
{
    RECT r = {};
    GetClientRect(hDlg, &r);
    int W = r.right, H = r.bottom;
    if (W <= 0 || H <= 0)
        return;

    // Fixed pixel measurements
    const int mX      = 10;   // horizontal margin
    const int mY      = 8;    // top margin
    const int DY      = 5;    // small vertical gap
    const int btnH    = 24;   // button height
    const int lblH    = 14;   // label height

    // Bottom-anchored zone heights
    const int statusH  = 16;
    const int hidBtnH  = btnH;
    const int hidListH = 100;

    // Compute Y coordinates bottom-up
    const int statusY  = H - statusH - 4;
    const int hidBtnY  = statusY - DY - hidBtnH;
    const int hidListY = hidBtnY - DY - hidListH;
    const int hidLblY  = hidListY - DY - lblH;

    const int opsH     = 44;
    const int opsY     = hidLblY - DY - opsH;
    const int selInfoY = opsY - DY - lblH;

    // Top list fills all available width (no side buttons)
    const int topListY = mY + lblH + 3;
    const int topListH = selInfoY - DY - topListY;
    const int listW    = W - 2*mX;

    // Helper
    auto Move = [&](int id, int x, int y, int w, int h) {
        if (HWND hCtrl = GetDlgItem(hDlg, id))
            MoveWindow(hCtrl, x, y, w, h, FALSE);
    };

    // Window list – full width
    Move(IDC_WINDOW_LIST, mX, topListY, listW, topListH);

    // Selected info label
    Move(IDC_SELECTED_INFO, mX, selInfoY, W - 2*mX, lblH);

    // Operations group + its buttons
    if (HWND hGrp = GetDlgItem(hDlg, IDC_GRP_OPS))
        MoveWindow(hGrp, mX, opsY, W - 2*mX, opsH, FALSE);
    {
        const int bx = mX + 8, by = opsY + 18;
        const int bw0 = 100, bw1 = 118, bw2 = 96;
        const int bw3 = W - 2*mX - 8 - bw0 - 4 - bw1 - 4 - bw2 - 4 - 8;
        Move(IDC_BTN_TOPMOST,    bx,                         by, bw0, btnH);
        Move(IDC_BTN_NO_TOPMOST, bx + bw0 + 4,               by, bw1, btnH);
        Move(IDC_BTN_HIDE,       bx + bw0+4 + bw1+4,         by, bw2, btnH);
        Move(IDC_BTN_WDA,        bx + bw0+4 + bw1+4 + bw2+4, by,
             (bw3 > 80 ? bw3 : 80), btnH);
    }

    // Hidden windows section – "Show Selected" button only (no "Remove from List")
    Move(IDC_GRP_HIDDEN,  mX, hidLblY,  W - 2*mX, lblH + 2);
    Move(IDC_HIDDEN_LIST, mX, hidListY, W - 2*mX, hidListH);
    Move(IDC_BTN_SHOW,    mX, hidBtnY,  110, hidBtnH);

    // Status bar
    Move(IDC_STATUS_TEXT, mX, statusY, W - 2*mX, statusH);

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

        // Centre the dialog on the primary monitor.
        {
            const int dlgW = 680, dlgH = 560;
            int scW = GetSystemMetrics(SM_CXSCREEN);
            int scH = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hDlg, nullptr,
                         (scW - dlgW) / 2, (scH - dlgH) / 2,
                         dlgW, dlgH,
                         SWP_NOZORDER);
        }

        // Init ListView columns.
        InitMainListViewColumns(GetDlgItem(hDlg, IDC_WINDOW_LIST));
        InitListViewColumns(GetDlgItem(hDlg, IDC_HIDDEN_LIST));

        // Enable full-row selection and grid lines.
        auto SetLVStyle = [](HWND h) {
            DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES;
            ListView_SetExtendedListViewStyle(h, ex);
        };
        SetLVStyle(GetDlgItem(hDlg, IDC_WINDOW_LIST));
        SetLVStyle(GetDlgItem(hDlg, IDC_HIDDEN_LIST));

        // Add tray icon.
        CreateTrayIcon(hDlg);

        // Initial list population and start the refresh timer.
        PopulateWindowList(hDlg);
        SetTimer(hDlg, IDT_REFRESH, 2000, nullptr);

        // Trigger initial layout.
        {
            RECT rc; GetClientRect(hDlg, &rc);
            OnSize(hDlg, rc.right, rc.bottom);
        }
        return TRUE;
    }

    // --------------------------------------------------------------------
    // Auto-refresh on focus; placeholder when unfocused.
    case WM_ACTIVATE:
    {
        if (LOWORD(wParam) == WA_INACTIVE) {
            KillTimer(hDlg, IDT_REFRESH);
            ShowPlaceholder(hDlg);
        } else {
            PopulateWindowList(hDlg);
            UpdateSelectedInfo(hDlg);
            SetTimer(hDlg, IDT_REFRESH, 2000, nullptr);
        }
        return FALSE;
    }

    // --------------------------------------------------------------------
    case WM_TIMER:
        if (wParam == IDT_REFRESH) {
            PopulateWindowList(hDlg, true); // preserve selection
            UpdateSelectedInfo(hDlg);
        }
        return TRUE;

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
        mmi->ptMinTrackSize = { 520, 420 };
        return TRUE;
    }

    // --------------------------------------------------------------------
    case WM_NOTIFY:
    {
        auto* pNMHDR = reinterpret_cast<LPNMHDR>(lParam);
        if (pNMHDR->idFrom == IDC_WINDOW_LIST) {
            if (pNMHDR->code == LVN_ITEMCHANGED) {
                UpdateSelectedInfo(hDlg);
            } else if (pNMHDR->code == NM_CLICK) {
                // Click on TopMost or Excl.Cap column toggles state inline.
                auto* pIA = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (pIA->iItem >= 0 &&
                    pIA->iItem < static_cast<int>(g_windows.size()))
                {
                    const WindowInfo& w = g_windows[pIA->iItem];
                    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);

                    if (pIA->iSubItem == 4) {
                        // Toggle TopMost
                        bool newState = !IsWindowTopMost(w.hwnd);
                        if (SetWindowTopMost(w.hwnd, newState)) {
                            ListView_SetItemText(hList, pIA->iItem, 4,
                                const_cast<LPWSTR>(newState ? L"\u2713" : L""));
                            SetStatus(hDlg, newState
                                ? L"Set TOPMOST: \"" + w.title + L"\""
                                : L"Removed TOPMOST: \"" + w.title + L"\"");
                        }
                    } else if (pIA->iSubItem == 5) {
                        // Toggle ExcludeCapture via injection
                        bool curState = IsWindowExcludeFromCapture(w.hwnd);
                        // WDA_NONE=0 to remove, WDA_EXCLUDEFROMCAPTURE=0x11 to set
                        DWORD newAffinity = curState ? 0x00000000u : 0x00000011u;
                        SetStatus(hDlg, L"Injecting \u2026");
                        if (InjectWDASetAffinity(w.hwnd, newAffinity)) {
                            bool newState = !curState;
                            ListView_SetItemText(hList, pIA->iItem, 5,
                                const_cast<LPWSTR>(newState ? L"\u2713" : L""));
                            SetStatus(hDlg, newState
                                ? L"ExcludeCapture enabled: \"" + w.title + L"\""
                                : L"ExcludeCapture disabled: \"" + w.title + L"\"");
                        } else {
                            SetStatus(hDlg,
                                L"Injection failed (error "
                                + std::to_wstring(GetLastError())
                                + L"). Run as Administrator and ensure "
                                  L"wda_inject.dll is beside the exe.");
                        }
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
        case IDC_BTN_TOPMOST:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            if (SetWindowTopMost(w->hwnd, true)) {
                SetStatus(hDlg, L"Set TOPMOST: \"" + w->title + L"\"");
                HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (sel >= 0)
                    ListView_SetItemText(hList, sel, 4,
                        const_cast<LPWSTR>(L"\u2713"));
            } else {
                SetStatus(hDlg, L"Failed to set TOPMOST (error "
                    + std::to_wstring(GetLastError()) + L")");
            }
            break;
        }

        case IDC_BTN_NO_TOPMOST:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            if (SetWindowTopMost(w->hwnd, false)) {
                SetStatus(hDlg, L"Removed TOPMOST: \"" + w->title + L"\"");
                HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (sel >= 0)
                    ListView_SetItemText(hList, sel, 4,
                        const_cast<LPWSTR>(L""));
            } else {
                SetStatus(hDlg, L"Failed to remove TOPMOST (error "
                    + std::to_wstring(GetLastError()) + L")");
            }
            break;
        }

        case IDC_BTN_HIDE:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }

            // Check not already hidden.
            for (auto& h : g_hiddenWindows)
                if (h.hwnd == w->hwnd) {
                    SetStatus(hDlg, L"Already in hidden list.");
                    goto done_hide;
                }

            if (HideWindow(w->hwnd)) {
                g_hiddenWindows.push_back(*w);
                PopulateHiddenList(hDlg);
                PopulateWindowList(hDlg); // window disappears from main list
                UpdateSelectedInfo(hDlg);
                SetStatus(hDlg, L"Hidden: \"" + w->title + L"\"");
            } else {
                SetStatus(hDlg, L"Failed to hide window.");
            }
        done_hide:;
            break;
        }

        case IDC_BTN_WDA:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            SetStatus(hDlg, L"Injecting DLL into \""
                + w->title + L"\" \u2026");
            if (InjectWDAExcludeFromCapture(w->hwnd)) {
                SetStatus(hDlg, L"WDA_EXCLUDEFROMCAPTURE applied to \""
                    + w->title + L"\"");
                HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
                int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (sel >= 0)
                    ListView_SetItemText(hList, sel, 5,
                        const_cast<LPWSTR>(L"\u2713"));
            } else {
                SetStatus(hDlg,
                    L"Injection failed (error "
                    + std::to_wstring(GetLastError())
                    + L"). Run as Administrator and ensure "
                      L"wda_inject.dll is beside the exe.");
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
                PopulateWindowList(hDlg);
                UpdateSelectedInfo(hDlg);
            } else {
                SetStatus(hDlg, L"Failed to show window.");
            }
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

        case IDM_TRAY_EXIT:
            // Restore hidden windows, then actually quit.
            KillTimer(hDlg, IDT_REFRESH);
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
    case WM_CLOSE:
        // Close button hides to tray; actual exit is via tray menu "Exit".
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }

    return FALSE;
}
