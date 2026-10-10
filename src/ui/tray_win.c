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
#define CDM_MENU_ADDURL 1003
#define CDM_MENU_CLIPBOARD 1004
#define CDM_MENU_PAUSEALL 1005
#define CDM_MENU_SETTINGS 1006

static HWND g_msgwin = NULL;
static HANDLE g_thread = NULL;
static volatile int g_run = 0;
static volatile int g_exit_req = 0;
static volatile int g_show_req = 0;
static volatile int g_addurl_req = 0;
static volatile int g_clip_req = 0;
static volatile int g_pause_req = 0;
static volatile int g_settings_req = 0;
static volatile int g_ready = 0;
static char g_tip[128] = "cdm Download Manager";
static char g_clip[2048] = {0};
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
            AppendMenuA(menu, MF_STRING, CDM_MENU_SHOW, "Show download list");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, CDM_MENU_ADDURL, "New download...");
            AppendMenuA(menu, MF_STRING, CDM_MENU_CLIPBOARD, "New download from clipboard");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, CDM_MENU_PAUSEALL, "Pause / resume all");
            AppendMenuA(menu, MF_STRING, CDM_MENU_SETTINGS, "Settings...");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
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
        else if (id == CDM_MENU_ADDURL) g_addurl_req = 1;
        else if (id == CDM_MENU_SETTINGS) g_settings_req = 1;
        else if (id == CDM_MENU_PAUSEALL) g_pause_req = 1;
        else if (id == CDM_MENU_CLIPBOARD) {
            /* snapshot clipboard text now (tray thread owns the timing) */
            if (OpenClipboard(hw)) {
                HANDLE h = GetClipboardData(CF_TEXT);
                if (h) {
                    const char *t = (const char *)GlobalLock(h);
                    if (t) {
                        EnterCriticalSection(&g_tip_cs);
                        snprintf(g_clip, sizeof(g_clip), "%s", t);
                        LeaveCriticalSection(&g_tip_cs);
                        g_clip_req = 1;
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
            }
        }
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

int cdm_tray_add_url_requested(void) {
    int r = g_addurl_req;
    g_addurl_req = 0;
    return r;
}

int cdm_tray_clipboard_requested(char *out, size_t cap) {
    int r = g_clip_req;
    g_clip_req = 0;
    if (r && out && cap > 0) {
        EnterCriticalSection(&g_tip_cs);
        snprintf(out, cap, "%s", g_clip);
        LeaveCriticalSection(&g_tip_cs);
    }
    return r;
}

int cdm_tray_pause_all_requested(void) {
    int r = g_pause_req;
    g_pause_req = 0;
    return r;
}

int cdm_tray_settings_requested(void) {
    int r = g_settings_req;
    g_settings_req = 0;
    return r;
}

static int bindir(char *out, size_t cap) {
    char exe[2048], *s;
    if (GetModuleFileNameA(NULL, exe, sizeof(exe)) == 0) return -1;
    s = strrchr(exe, '\\');
    if (!s) return -1;
    *s = 0;
    snprintf(out, cap, "%s", exe);
    return 0;
}

/* Split HKCU Environment Path handling: read full value (any length),
 * test for our bin dir (case-insensitive), append/remove as needed. */
static int read_user_path(char **out, DWORD *type) {
    HKEY hk;
    DWORD sz = 0, t = REG_EXPAND_SZ;
    char *buf;
    LONG rc;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return -1;
    rc = RegQueryValueExA(hk, "Path", NULL, &t, NULL, &sz);
    if (rc != ERROR_SUCCESS) {
        /* no user PATH yet: treat as empty */
        buf = (char *)calloc(1, 1);
        if (!buf) {
            RegCloseKey(hk);
            return -1;
        }
        *out = buf;
        if (type) *type = REG_EXPAND_SZ;
        RegCloseKey(hk);
        return 0;
    }
    buf = (char *)malloc(sz + 2);
    if (!buf) {
        RegCloseKey(hk);
        return -1;
    }
    if (RegQueryValueExA(hk, "Path", NULL, &t, (LPBYTE)buf, &sz) != ERROR_SUCCESS) {
        free(buf);
        RegCloseKey(hk);
        return -1;
    }
    buf[sz] = 0;
    *out = buf;
    if (type) *type = t;
    RegCloseKey(hk);
    return 0;
}

static void broadcast_env_change(void) {
    SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
}

/* case-insensitive whole-entry match on ';'-separated list */
static int path_has(const char *path, const char *entry) {
    size_t elen = strlen(entry);
    const char *p = path;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        /* trim trailing slashes for comparison */
        while (n > 0 && (p[n-1] == '\\' || p[n-1] == '/')) n--;
        if (n == elen && _strnicmp(p, entry, n) == 0) return 1;
        if (!semi) break;
        p = semi + 1;
    }
    return 0;
}

int cdm_path_get(void) {
    char bin[2048];
    char *path = NULL;
    int r = 0;
    if (bindir(bin, sizeof(bin)) != 0) return 0;
    if (read_user_path(&path, NULL) != 0) return 0;
    r = path_has(path, bin);
    free(path);
    return r;
}

int cdm_path_set(int on) {
    char bin[2048];
    char *path = NULL;
    DWORD type = REG_EXPAND_SZ;
    HKEY hk;
    int rc = -1;
    if (bindir(bin, sizeof(bin)) != 0) return -1;
    if (read_user_path(&path, &type) != 0) return -1;
    if (on) {
        if (!path_has(path, bin)) {
            size_t need = strlen(path) + 1 + strlen(bin) + 1;
            char *np = (char *)malloc(need);
            if (!np) {
                free(path);
                return -1;
            }
            if (path[0])
                snprintf(np, need, "%s;%s", path, bin);
            else
                snprintf(np, need, "%s", bin);
            free(path);
            path = np;
        } else {
            rc = 0; /* already there */
        }
    } else {
        /* rebuild without our entry */
        size_t len = strlen(path);
        char *np = (char *)malloc(len + 1);
        char *w;
        const char *p;
        size_t elen = strlen(bin);
        if (!np) {
            free(path);
            return -1;
        }
        w = np;
        p = path;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t n = semi ? (size_t)(semi - p) : strlen(p);
            size_t tn = n;
            while (tn > 0 && (p[tn-1] == '\\' || p[tn-1] == '/')) tn--;
            if (!(tn == elen && _strnicmp(p, bin, tn) == 0)) {
                if (w != np) *w++ = ';';
                memcpy(w, p, n);
                w += n;
            }
            if (!semi) break;
            p = semi + 1;
        }
        *w = 0;
        free(path);
        path = np;
    }
    if (rc != 0) {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0,
                          KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
            if (RegSetValueExA(hk, "Path", 0, type,
                               (const BYTE *)path,
                               (DWORD)(strlen(path) + 1)) == ERROR_SUCCESS)
                rc = 0;
            RegCloseKey(hk);
        }
        if (rc == 0) broadcast_env_change();
    }
    free(path);
    return rc;
}

/* Auto-start via HKCU\\...\\Run */
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
