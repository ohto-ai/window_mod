/**
 * main.cpp  –  Windows window modifier tool
 *
 * Features:
 *  - Enumerate visible windows (title + process name)
 *  - "Pick" cursor: click a live window to select it
 *  - Set / remove TOPMOST
 *  - Hide (track in a separate list for recovery)
 *  - Inject wda_inject.dll to set WDA_EXCLUDEFROMCAPTURE
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE

#include <windows.h>
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

// ============================================================================
// State
// ============================================================================
static HINSTANCE g_hInst       = nullptr;
static HWND      g_hDlg         = nullptr;

static std::vector<WindowInfo> g_windows;       // current window snapshot
static std::vector<WindowInfo> g_hiddenWindows; // windows we've hidden

static bool g_picking          = false;         // picker mode active
static HWND g_lastHighlighted  = nullptr;       // window with highlight drawn

static HCURSOR g_hCross        = nullptr;
static HCURSOR g_hArrow        = nullptr;

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
// ListView column setup

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
// Populate (or refresh) the main window list.

static void PopulateWindowList(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
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
    }
    SetStatus(hDlg,
        std::wstring(L"Refreshed – ") + std::to_wstring(g_windows.size())
        + L" windows found.");
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
// Select a window in the list by HWND (after pick).

static void SelectByHwnd(HWND hDlg, HWND target)
{
    HWND hList = GetDlgItem(hDlg, IDC_WINDOW_LIST);
    for (int i = 0; i < static_cast<int>(g_windows.size()); ++i) {
        if (g_windows[i].hwnd == target) {
            ListView_SetItemState(hList, i,
                LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hList, i, FALSE);
            return;
        }
    }
    // Not in list yet – add it on the fly.
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    wchar_t title[256] = {};
    GetWindowTextW(target, title, 256);
    g_windows.push_back({ target, title, GetProcessName(pid), pid });
    PopulateWindowList(hDlg);
    SelectByHwnd(hDlg, target);
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
// Window highlight drawing (used during pick mode)
// ============================================================================

static void DrawHighlight(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return;

    HDC hdc = GetDC(nullptr); // screen DC
    if (!hdc) return;

    const int border = 3;
    // Draw four edge rectangles in INVERT mode (XOR – calling twice restores).
    RECT edges[4] = {
        { rc.left,          rc.top,            rc.right,          rc.top  + border },
        { rc.left,          rc.bottom - border, rc.right,          rc.bottom        },
        { rc.left,          rc.top,            rc.left  + border, rc.bottom        },
        { rc.right - border,rc.top,            rc.right,          rc.bottom        },
    };
    for (auto& e : edges)
        InvertRect(hdc, &e);

    ReleaseDC(nullptr, hdc);
}

static void EraseHighlight(HWND hwnd)
{
    // InvertRect is its own inverse; calling it again restores the pixels.
    DrawHighlight(hwnd);
}

// ============================================================================
// Picker logic
// ============================================================================

static void StartPicking(HWND hDlg)
{
    g_picking         = true;
    g_lastHighlighted = nullptr;
    SetCapture(hDlg);
    SetCursor(g_hCross);
    SetDlgItemTextW(hDlg, IDC_BTN_PICK, L"Cancel Pick");
    SetStatus(hDlg, L"Pick mode: move mouse over a window and click to select.  "
                    L"Press Escape or right-click to cancel.");
}

static void StopPicking(HWND hDlg, bool cancelled)
{
    if (!g_picking) return;
    g_picking = false;

    if (g_lastHighlighted) {
        EraseHighlight(g_lastHighlighted);
        g_lastHighlighted = nullptr;
    }
    ReleaseCapture();
    SetCursor(g_hArrow);
    SetDlgItemTextW(hDlg, IDC_BTN_PICK, L"Pick Window");
    SetStatus(hDlg, cancelled ? L"Pick cancelled." : L"Window picked.");
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
    const int sideBtnW = 95;  // Refresh / Pick button width

    // Bottom-anchored zone heights
    const int statusH  = 16;
    const int hidBtnH  = btnH;
    const int hidListH = 100;

    // Compute Y coordinates bottom-up
    const int statusY  = H - statusH - 4;
    const int hidBtnY  = statusY - DY - hidBtnH;
    const int hidListY = hidBtnY - DY - hidListH;
    const int hidLblY  = hidListY - DY - lblH;

    const int opsH     = 44;  // Operations group box height
    const int opsY     = hidLblY - DY - opsH;
    const int selInfoY = opsY - DY - lblH;

    // Top list fills remaining space
    const int topListY = mY + lblH + 3;
    const int topListH = selInfoY - DY - topListY;
    const int rightX   = W - mX - sideBtnW;
    const int listW    = rightX - mX - 4;

    // Helper
    auto Move = [&](int id, int x, int y, int w, int h) {
        if (HWND hCtrl = GetDlgItem(hDlg, id))
            MoveWindow(hCtrl, x, y, w, h, FALSE);
    };

    // Window list + side buttons
    Move(IDC_WINDOW_LIST, mX, topListY, listW, topListH);
    Move(IDC_BTN_REFRESH, rightX, mY + lblH + 3,         sideBtnW, btnH);
    Move(IDC_BTN_PICK,    rightX, mY + lblH + 3 + btnH + DY, sideBtnW, btnH);

    // Selected info label
    Move(IDC_SELECTED_INFO, mX, selInfoY, W - 2*mX, lblH);

    // Operations group + its buttons
    if (HWND hGrp = GetDlgItem(hDlg, IDC_GRP_OPS))
        MoveWindow(hGrp, mX, opsY, W - 2*mX, opsH, FALSE);
    {
        const int bx = mX + 8, by = opsY + 18;
        const int bw0 = 100, bw1 = 118, bw2 = 96;
        // WDA button takes the rest
        const int bw3 = W - 2*mX - 8 - bw0 - 4 - bw1 - 4 - bw2 - 4 - 8;
        Move(IDC_BTN_TOPMOST,    bx,                    by, bw0, btnH);
        Move(IDC_BTN_NO_TOPMOST, bx + bw0 + 4,          by, bw1, btnH);
        Move(IDC_BTN_HIDE,       bx + bw0+4 + bw1+4,    by, bw2, btnH);
        Move(IDC_BTN_WDA,        bx + bw0+4 + bw1+4 + bw2+4, by,
             (bw3 > 80 ? bw3 : 80), btnH);
    }

    // Hidden windows section
    Move(IDC_GRP_HIDDEN,        mX, hidLblY,  W - 2*mX, lblH + 2);
    Move(IDC_HIDDEN_LIST,       mX, hidListY, W - 2*mX, hidListH);
    Move(IDC_BTN_SHOW,          mX,      hidBtnY, 110, hidBtnH);
    Move(IDC_BTN_REMOVE_HIDDEN, mX + 116, hidBtnY, 130, hidBtnH);

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
        g_hDlg  = hDlg;
        g_hCross = LoadCursorW(nullptr, IDC_CROSS);
        g_hArrow = LoadCursorW(nullptr, IDC_ARROW);

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
        InitListViewColumns(GetDlgItem(hDlg, IDC_WINDOW_LIST));
        InitListViewColumns(GetDlgItem(hDlg, IDC_HIDDEN_LIST));

        // Enable full-row selection and grid lines.
        auto SetLVStyle = [](HWND h) {
            DWORD ex = LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES;
            ListView_SetExtendedListViewStyle(h, ex);
        };
        SetLVStyle(GetDlgItem(hDlg, IDC_WINDOW_LIST));
        SetLVStyle(GetDlgItem(hDlg, IDC_HIDDEN_LIST));

        PopulateWindowList(hDlg);

        // Trigger initial layout.
        {
            RECT rc; GetClientRect(hDlg, &rc);
            OnSize(hDlg, rc.right, rc.bottom);
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
    case WM_SETCURSOR:
        if (g_picking) {
            SetCursor(g_hCross);
            return TRUE; // prevent DefDlgProc from resetting cursor
        }
        break;

    // --------------------------------------------------------------------
    case WM_MOUSEMOVE:
        if (g_picking) {
            POINT pt;
            GetCursorPos(&pt);

            // Find the real window under the cursor (skip our dialog).
            HWND hHover = WindowFromPoint(pt);
            // Walk up to the top-level window.
            if (hHover) {
                HWND parent = GetAncestor(hHover, GA_ROOT);
                if (parent) hHover = parent;
            }
            if (hHover == hDlg) hHover = nullptr;

            if (hHover != g_lastHighlighted) {
                if (g_lastHighlighted)
                    EraseHighlight(g_lastHighlighted);
                g_lastHighlighted = hHover;
                if (hHover)
                    DrawHighlight(hHover);

                // Show info in status bar while hovering.
                if (hHover) {
                    wchar_t title[256] = {};
                    GetWindowTextW(hHover, title, 256);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hHover, &pid);
                    SetStatus(hDlg,
                        std::wstring(L"Hovering: \"") + title
                        + L"\"  " + GetProcessName(pid)
                        + L"  PID:" + std::to_wstring(pid));
                }
            }
            SetCursor(g_hCross);
        }
        return TRUE;

    // --------------------------------------------------------------------
    case WM_LBUTTONUP:
        if (g_picking) {
            HWND picked = g_lastHighlighted;
            StopPicking(hDlg, false);
            if (picked && IsWindow(picked)) {
                // Make sure the list is up-to-date, then select.
                PopulateWindowList(hDlg);
                SelectByHwnd(hDlg, picked);
                UpdateSelectedInfo(hDlg);
            }
        }
        return TRUE;

    // --------------------------------------------------------------------
    case WM_RBUTTONUP:
        if (g_picking) {
            StopPicking(hDlg, true);
        }
        return TRUE;

    // --------------------------------------------------------------------
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g_picking) {
            StopPicking(hDlg, true);
            return TRUE;
        }
        break;

    // --------------------------------------------------------------------
    case WM_NOTIFY:
    {
        auto* pNMHDR = reinterpret_cast<LPNMHDR>(lParam);
        if (pNMHDR->idFrom == IDC_WINDOW_LIST &&
            pNMHDR->code == LVN_ITEMCHANGED)
        {
            UpdateSelectedInfo(hDlg);
        }
        break;
    }

    // --------------------------------------------------------------------
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        switch (id)
        {
        case IDC_BTN_REFRESH:
            PopulateWindowList(hDlg);
            UpdateSelectedInfo(hDlg);
            break;

        case IDC_BTN_PICK:
            if (g_picking)
                StopPicking(hDlg, true);
            else
                StartPicking(hDlg);
            break;

        case IDC_BTN_TOPMOST:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            if (SetWindowTopMost(w->hwnd, true))
                SetStatus(hDlg, L"Set TOPMOST: \"" + w->title + L"\"");
            else
                SetStatus(hDlg, L"Failed to set TOPMOST (error "
                    + std::to_wstring(GetLastError()) + L")");
            break;
        }

        case IDC_BTN_NO_TOPMOST:
        {
            const WindowInfo* w = GetSelectedWindow(hDlg);
            if (!w) { SetStatus(hDlg, L"No window selected."); break; }
            if (SetWindowTopMost(w->hwnd, false))
                SetStatus(hDlg, L"Removed TOPMOST: \"" + w->title + L"\"");
            else
                SetStatus(hDlg, L"Failed to remove TOPMOST (error "
                    + std::to_wstring(GetLastError()) + L")");
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
                + w->title + L"\" …");
            if (InjectWDAExcludeFromCapture(w->hwnd))
                SetStatus(hDlg, L"WDA_EXCLUDEFROMCAPTURE applied to \""
                    + w->title + L"\"");
            else
                SetStatus(hDlg,
                    L"Injection failed (error "
                    + std::to_wstring(GetLastError())
                    + L"). Run as Administrator and ensure "
                      L"wda_inject.dll is beside the exe.");
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

        case IDC_BTN_REMOVE_HIDDEN:
        {
            HWND hList = GetDlgItem(hDlg, IDC_HIDDEN_LIST);
            int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= static_cast<int>(g_hiddenWindows.size())) {
                SetStatus(hDlg, L"No hidden window selected.");
                break;
            }
            std::wstring title = g_hiddenWindows[sel].title;
            g_hiddenWindows.erase(g_hiddenWindows.begin() + sel);
            PopulateHiddenList(hDlg);
            SetStatus(hDlg, L"Removed from hidden list (not restored): \""
                + title + L"\"");
            break;
        }

        case IDCANCEL:
            if (g_picking) {
                StopPicking(hDlg, true);
            } else {
                EndDialog(hDlg, 0);
            }
            return TRUE;
        }
        break;
    }

    // --------------------------------------------------------------------
    case WM_CLOSE:
        if (g_picking)
            StopPicking(hDlg, true);
        EndDialog(hDlg, 0);
        return TRUE;
    }

    return FALSE;
}
