#if defined(_WIN32)

#include "tray.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CDM_TRAY_MSG (WM_APP + 77)
#define CDM_TRAY_ID 1
#define CDM_MENU_SHOW 1001
#define CDM_MENU_EXIT 1002

static HWND g_msgwin = NULL;
static HANDLE g_thread = NULL;
static volatile int g_run = 0;
static volatile int g_exit_req = 0;
static volatile int g_show_req = 0;
static volatile int g_ready = 0;
static char g_tip[128] = "cdm Download Manager";
static CRITICAL_SECTION g_tip_cs;

static void utf8_to_wide(const char *s, wchar_t *out, int cap) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    out[cap - 1] = 0;
}

static void tray_modify(HWND hw) {
    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hw;
    nid.uID = CDM_TRAY_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = CDM_TRAY_MSG;
    nid.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    if (!nid.hIcon)
        nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    EnterCriticalSection(&g_tip_cs);
    snprintf(nid.szTip, sizeof(nid.szTip), "%s", g_tip);
    LeaveCriticalSection(&g_tip_cs);
    Shell_NotifyIconA(NIM_MODIFY, &nid);
    if (nid.hIcon && nid.hIcon != LoadIconA(NULL, IDI_APPLICATION))
        DestroyIcon(nid.hIcon);
}

static void tray_add(HWND hw) {
    NOTIFYICONDATAA nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hw;
    nid.uID = CDM_TRAY_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = CDM_TRAY_MSG;
    nid.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    if (!nid.hIcon)
        nid.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    EnterCriticalSection(&g_tip_cs);
    snprintf(nid.szTip, sizeof(nid.szTip), "%s", g_tip);
    LeaveCriticalSection(&g_tip_cs);
    Shell_NotifyIconA(NIM_ADD, &nid);
    Shell_NotifyIconA(NIM_SETVERSION, &nid);
    if (nid.hIcon && nid.hIcon != LoadIconA(NULL, IDI_APPLICATION))
        DestroyIcon(nid.hIcon);
}

static LRESULT CALLBACK tray_wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == CDM_TRAY_MSG) {
        UINT ev = (UINT)LOWORD(lp);
        if (ev == WM_RBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            POINT pt;
            HMENU menu = CreatePopupMenu();
            GetCursorPos(&pt);
            AppendMenuA(menu, MF_STRING, CDM_MENU_SHOW, "Show cdm");
            AppendMenuA(menu, MF_STRING, CDM_MENU_EXIT, "Exit");
            SetForegroundWindow(hw);
            TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y,
                           0, hw, NULL);
            DestroyMenu(menu);
        } else if (ev == WM_LBUTTONUP) {
            g_show_req = 1;
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = (int)LOWORD(wp);
        if (id == CDM_MENU_SHOW) g_show_req = 1;
        else if (id == CDM_MENU_EXIT) g_exit_req = 1;
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static DWORD WINAPI tray_thread(LPVOID arg) {
    WNDCLASSA wc;
    MSG m;
    (void)arg;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "cdmTrayMsgWindow";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 1;
    g_msgwin = CreateWindowExA(0, wc.lpszClassName, "cdm tray", 0,
                               0, 0, 0, 0, HWND_MESSAGE, NULL,
                               wc.hInstance, NULL);
    if (!g_msgwin) return 1;
    tray_add(g_msgwin);
    g_ready = 1;
    while (g_run && GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    {
        NOTIFYICONDATAA nid;
        memset(&nid, 0, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = g_msgwin;
        nid.uID = CDM_TRAY_ID;
        Shell_NotifyIconA(NIM_DELETE, &nid);
    }
    DestroyWindow(g_msgwin);
    g_msgwin = NULL;
    return 0;
}

int cdm_tray_init(void) {
    static int cs_init = 0;
    if (g_run) return 0;
    if (!cs_init) {
        InitializeCriticalSection(&g_tip_cs);
        cs_init = 1;
    }
    g_run = 1;
    g_ready = 0;
    g_thread = CreateThread(NULL, 0, tray_thread, NULL, 0, NULL);
    if (!g_thread) {
        g_run = 0;
        return -1;
    }
    /* wait briefly for the icon to appear */
    for (int i = 0; i < 50 && !g_ready; i++) Sleep(20);
    return g_ready ? 0 : -1;
}

void cdm_tray_shutdown(void) {
    if (!g_run) return;
    g_run = 0;
    if (g_msgwin) PostMessageA(g_msgwin, WM_CLOSE, 0, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    DeleteCriticalSection(&g_tip_cs);
}

void cdm_tray_tooltip(const char *text) {
    EnterCriticalSection(&g_tip_cs);
    if (text)
        snprintf(g_tip, sizeof(g_tip), "%s", text);
    else
        snprintf(g_tip, sizeof(g_tip), "cdm Download Manager");
    LeaveCriticalSection(&g_tip_cs);
    if (g_msgwin) tray_modify(g_msgwin);
}

void cdm_tray_notify(const char *title, const char *msg) {
    HWND hw;
    wchar_t wt[64], wm[256];
    if (!g_run) return;
    hw = g_msgwin;
    if (!hw) return;
    utf8_to_wide(title ? title : "cdm", wt, 64);
    utf8_to_wide(msg ? msg : "", wm, 256);
    {
        NOTIFYICONDATAW nidw;
        memset(&nidw, 0, sizeof(nidw));
        nidw.cbSize = sizeof(nidw);
        nidw.hWnd = hw;
        nidw.uID = CDM_TRAY_ID;
        nidw.uFlags = NIF_INFO;
        nidw.dwInfoFlags = NIIF_INFO;
        wcsncpy(nidw.szInfoTitle, wt, 63);
        wcsncpy(nidw.szInfo, wm, 255);
        Shell_NotifyIconW(NIM_MODIFY, &nidw);
    }
}

int cdm_tray_exit_requested(void) {
    int r = g_exit_req;
    g_exit_req = 0;
    return r;
}

int cdm_tray_show_requested(void) {
    int r = g_show_req;
    g_show_req = 0;
    return r;
}

/* Auto-start via HKCU\\...\Run */
int cdm_autostart_get(void) {
    HKEY hk;
    char val[2048];
    DWORD sz = sizeof(val);
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return 0;
    if (RegQueryValueExA(hk, "cdm", NULL, NULL, (LPBYTE)val, &sz) != ERROR_SUCCESS)
        val[0] = 0;
    RegCloseKey(hk);
    return val[0] ? 1 : 0;
}

int cdm_autostart_set(int on) {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return -1;
    if (on) {
        char exe[2048], cmd[2200];
        if (GetModuleFileNameA(NULL, exe, sizeof(exe)) == 0) {
            RegCloseKey(hk);
            return -1;
        }
        /* start hidden to tray */
        snprintf(cmd, sizeof(cmd), "\"%s\" --minimized", exe);
        if (RegSetValueExA(hk, "cdm", 0, REG_SZ,
                           (const BYTE *)cmd, (DWORD)(strlen(cmd) + 1)) != ERROR_SUCCESS) {
            RegCloseKey(hk);
            return -1;
        }
    } else {
        RegDeleteValueA(hk, "cdm");
    }
    RegCloseKey(hk);
    return 0;
}

#endif /* _WIN32 */
