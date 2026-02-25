#pragma once

// Application icon
#define IDI_APP_ICON            100

// Dialog IDs
#define IDD_MAIN_DIALOG         101

// Control IDs
#define IDC_WINDOW_LIST         1001
#define IDC_HIDDEN_LIST         1002
#define IDC_BTN_HIDE            1022
#define IDC_BTN_SHOW            1030
#define IDC_STATUS_TEXT         1040
#define IDC_SELECTED_INFO       1041
#define IDC_GRP_OPS             1050
#define IDC_GRP_HIDDEN          1051

// New controls for UI redesign
#define IDC_PREVIEW_LABEL       1060
#define IDC_PREVIEW_SUBTEXT     1061
#define IDC_PREVIEW_STATIC      1062
#define IDC_TAB_SCREENS         1063
#define IDC_HIDE_APPS_LABEL     1064
#define IDC_HIDE_APPS_SUB       1065
#define IDC_BTN_TOPMOST         1066
#define IDC_CHK_SHOW_CURSOR     1067
#define IDC_PLACEHOLDER_LABEL   1068
#define IDC_CHK_SHOW_PREVIEW    1069

// DLL management controls
#define IDC_BTN_UNLOAD_DLL      1070
#define IDC_CHK_AUTO_UNLOAD     1071

// Process watch controls
#define IDC_GRP_WATCH           1080
#define IDC_WATCH_EDIT          1081
#define IDC_BTN_WATCH_ADD       1082
#define IDC_BTN_WATCH_REMOVE    1083
#define IDC_WATCH_LIST          1084

// Timers
#define IDT_REFRESH             1
#define IDT_PREVIEW             2
#define IDT_WATCH               3

// Tray icon
#define WM_TRAYICON             (WM_USER + 1)
#define IDM_TRAY_SHOW           2001
#define IDM_TRAY_EXIT           2002
