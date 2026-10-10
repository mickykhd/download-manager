#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* localtime_r under strict -std=c11 */
#endif
#include "manager.h"
#include "ipc.h"
#include "tray.h"
#include "updater.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_IMPLEMENTATION
#include "nuklear.h"
#define NK_GLFW_GL3_IMPLEMENTATION
#include "nuklear_glfw_gl3.h"

/* local IPC so browser-native-messaging hosts can feed URLs to the running app */
#ifndef CDM_IPC_PORT
#define CDM_IPC_PORT 15151
#endif
#if defined(__linux__) || defined(__unix__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

/* Fatal pre-window error: console builds print to stderr; GUI-subsystem
 * builds (no console) show a message box instead. */
static void fatal_box(const char *msg) {
    fprintf(stderr, "cdm: %s\n", msg);
#ifdef _WIN32
    MessageBoxA(NULL, msg, "cdm - Download Manager",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

/* Single-instance guard: returns 1 when another copy is already running
 * (and brings its window forward on Windows), 0 when we own the lock. */
static int cdm_already_running(void) {
#ifdef _WIN32
    HANDLE m = CreateMutexA(NULL, TRUE, "cdm-download-manager-single-instance");
    if (!m) return 0; /* cannot tell: allow start */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowA(NULL, "cdm - Download Manager");
        if (w) {
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        CloseHandle(m);
        return 1;
    }
    return 0; /* keep handle open for process lifetime (intentional leak) */
#else
    /* POSIX: advisory lock on a file; released automatically on exit. */
    static int lockfd = -1;
    char path[1024];
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.cdm-gui.lock", home);
    lockfd = open(path, O_RDWR | O_CREAT, 0600);
    if (lockfd < 0) return 0;
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(lockfd, F_SETLK, &fl) != 0) return 1; /* locked elsewhere */
    (void)lockfd;
    return 0;
#endif
}

/* ---------------- globals ---------------- */
static cdm_manager *g_mgr;
static cdm_config g_cfg;
static int g_quit = 0;

static char g_base_dir[512] = "downloads";

/* dialog state */
static int g_show_add = 0;
static char g_url[2048] = {0}; static int g_url_len = 0;
static char g_out[2048] = {0}; static int g_out_len = 0;
static int  g_cat_idx = 0;
static char g_speed[32] = {0}; static int g_speed_len = 0;
static char g_sched[32] = {0}; static int g_sched_len = 0;
static int  g_add_queue = -1; /* -1 = download now, else queue index */
static char g_add_ua[256] = {0}; static int g_add_ua_len = 0;
static char g_add_hdrs[2048] = {0}; static int g_add_hdrs_len = 0;

static int g_show_props = 0;
static char g_props_speed[32] = {0}; static int g_props_speed_len = 0;

static int g_show_edit = 0;
static int g_edit_id = -1;
static char g_edit_url[2048] = {0}; static int g_edit_url_len = 0;
static char g_edit_out[2048] = {0}; static int g_edit_out_len = 0;
static char g_edit_speed[32] = {0}; static int g_edit_speed_len = 0;
static int  g_edit_queue = -1;
static char g_edit_ua[256] = {0}; static int g_edit_ua_len = 0;
static char g_edit_referer[512] = {0}; static int g_edit_referer_len = 0;
static char g_edit_cookie[1024] = {0}; static int g_edit_cookie_len = 0;
static char g_edit_hdrs[2048] = {0}; static int g_edit_hdrs_len = 0;

static int g_show_batch = 0;
static char g_batch[8192] = {0}; static int g_batch_len = 0;
static int  g_batch_queue = 0;

static int g_show_queues = 0;
static char g_newq_name[64] = {0}; static int g_newq_name_len = 0;
static int g_show_cats = 0;
static char g_newcat_name[64] = {0}; static int g_newcat_name_len = 0;
static int g_show_net = 0;
static int g_api_enabled = 0;
static char g_api_port[16] = {0}; static int g_api_port_len = 0;
static char g_api_key[128] = {0}; static int g_api_key_len = 0;
static int g_net_proxy = CDM_PROXY_SYSTEM;
static char g_net_proxy_url[512] = {0}; static int g_net_proxy_url_len = 0;
static char g_net_proxy_user[256] = {0}; static int g_net_proxy_user_len = 0;
static char g_net_proxy_pass[256] = {0}; static int g_net_proxy_pass_len = 0;
static char g_newhost[256] = {0}; static int g_newhost_len = 0;
static int  g_qedit_idx = 0;
static char g_qedit_start[16] = {0}; static int g_qedit_start_len = 0;
static char g_qedit_end[16] = {0}; static int g_qedit_end_len = 0;

/* "HH:MM" -> minutes since midnight; empty/invalid -> -1 (no bound). */
static int parse_hhmm_min(const char *s) {
    int hh, mm;
    if (!s || !*s) return -1;
    if (sscanf(s, "%d:%d", &hh, &mm) != 2) return -1;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
    return hh * 60 + mm;
}

static int g_show_opts = 0;
static int g_show_sched = 0;
static int g_show_about = 0;

static int g_tray_close = 0;
static int g_autostart = 0;
static int g_inpath = 0;
static int g_update_check = 1;
static int g_tray_icon = 1;
static int g_tray_minimize = 0;
static int g_tray_up = 0;
static int g_prog_popup = 1;
static int g_done_popup = 1;
static int g_confirm_exit = 1;
static int g_sounds = 1;

/* progress / completion / error / confirm / power dialog state */
static int g_show_progress = 0;
static int g_progress_id = -1;
static int g_show_done = 0;
static int g_done_id = -1;
static int g_show_err = 0;
static int g_err_id = -1;
static int g_show_confirm = 0;
static int g_power_action = 0; /* 0 off, 1 shutdown, 2 sleep, 3 hibernate */
static time_t g_power_deadline = 0; /* countdown target, 0 = idle */

/* transition tracking so popups/balloons/sounds fire exactly once */
#define CDM_SEEN_CAP 256
static int g_run_seen[1024]; static int g_n_run = 0;
static int g_done_seen[CDM_SEEN_CAP]; static int g_n_done = 0;
static int g_err_seen[CDM_SEEN_CAP]; static int g_n_err = 0;
static int seen_has(int *a, int n, int id) {
    for (int k = 0; k < n; k++) if (a[k] == id) return 1;
    return 0;
}
static void seen_add(int *a, int *n, int cap, int id) {
    if (seen_has(a, *n, id)) return;
    if (*n >= cap) {
        memmove(a, a + 1, (size_t)(cap - 1) * sizeof(int));
        (*n)--;
    }
    a[(*n)++] = id;
}

static void play_sound(int kind) {
    if (!g_sounds) return;
#ifdef _WIN32
    MessageBeep(kind ? MB_ICONHAND : MB_ICONASTERISK);
#else
    (void)kind;
#endif
}

static void run_power_action(void) {
    int a = g_power_action;
    g_power_action = 0;
    g_power_deadline = 0;
    if (a == 1) {
#ifdef _WIN32
        ShellExecuteA(NULL, "open", "shutdown.exe", "/s /t 30",
                      NULL, SW_HIDE);
#else
        system("shutdown -h +1 \"cdm: downloads finished\" &");
#endif
    } else if (a == 2 || a == 3) {
#ifdef _WIN32
        typedef BOOL (WINAPI *suspend_fn)(BOOL, BOOL, BOOL);
        HMODULE pm = LoadLibraryA("powrprof.dll");
        if (pm) {
            suspend_fn fn = (suspend_fn)GetProcAddress(pm, "SetSuspendState");
            if (fn) fn(a == 3, FALSE, FALSE);
            FreeLibrary(pm);
        }
#else
        system(a == 3 ? "systemctl hibernate &" : "systemctl suspend &");
#endif
    }
}
static int g_update_state = 0; /* cdm_update_poll result cache */
static char g_update_tag[64] = {0};

/* Close button: confirm while active, hide to tray when enabled. */
static void on_close(GLFWwindow *w) {
    if (g_confirm_exit && cdm_manager_any_running(g_mgr)) {
        g_show_confirm = 1;
        glfwSetWindowShouldClose(w, GLFW_FALSE);
        return;
    }
    if (g_tray_close && g_tray_up) {
        glfwHideWindow(w);
        glfwSetWindowShouldClose(w, GLFW_FALSE);
    }
}

/* Minimize button: hide to tray when enabled (restored via tray menu). */
static void on_iconify(GLFWwindow *w, int iconified) {
    if (iconified && g_tray_minimize && g_tray_up)
        glfwHideWindow(w);
}

static void show_main_window(GLFWwindow *w) {
    glfwShowWindow(w);
    glfwRestoreWindow(w);
    glfwFocusWindow(w);
}

static void open_url(const char *u) {
    if (!u || !*u) return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", u, NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    { char cmd[2200]; snprintf(cmd, sizeof(cmd), "open '%s' &", u);
      system(cmd); }
#else
    { char cmd[2200]; snprintf(cmd, sizeof(cmd), "xdg-open '%s' >/dev/null 2>&1 &", u);
      system(cmd); }
#endif
}

/* theme */
static int g_theme = 0;   /* 0 dark, 1 light */
static int g_skin = 0;    /* 0 blue,1 teal,2 red,3 purple */

static int g_cat_filter = -1; /* -1 = all */
static char g_search[256] = {0}; static int g_search_len = 0;
static int g_sort_col = -1;  /* -1 = insertion order */
static int g_sort_dir = 1;   /* 1 asc, -1 desc */
static int g_multi[256]; static int g_n_multi = 0;

static void open_edit(cdm_job *j); /* defined after draw_toolbar */
static void multi_clear(void); /* defined with the list helpers */
/* selection for bulk actions: the multi set, else the primary selection */
static int bulk_ids(int *out, int cap) {
    int n = 0;
    if (g_n_multi > 0) {
        for (int i = 0; i < g_n_multi && n < cap; i++) {
            if (cdm_manager_find(g_mgr, g_multi[i]))
                out[n++] = g_multi[i];
        }
    } else if (g_mgr->selected_id >= 0 &&
               cdm_manager_find(g_mgr, g_mgr->selected_id)) {
        out[n++] = g_mgr->selected_id;
    }
    return n;
}
static void bulk_stop(void) {
    int ids[256];
    int n = bulk_ids(ids, 256);
    for (int i = 0; i < n; i++) cdm_manager_stop(g_mgr, ids[i]);
}
static void bulk_resume(void) {
    int ids[256];
    int n = bulk_ids(ids, 256);
    for (int i = 0; i < n; i++) {
        cdm_job *j = cdm_manager_find(g_mgr, ids[i]);
        if (j && (j->state == JOB_PAUSED || j->state == JOB_QUEUED))
            cdm_manager_resume(g_mgr, ids[i]);
    }
}
static void bulk_remove(void) {
    int ids[256];
    int n = bulk_ids(ids, 256);
    for (int i = 0; i < n; i++) cdm_manager_remove(g_mgr, ids[i]);
    multi_clear();
}
static void persist_settings(void); /* defined with the modals */
static void restart_ipc_server(void); /* defined with the modals */

/* case-insensitive substring search */
static int contains_i(const char *hay, const char *needle) {
    size_t nl;
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    nl = strlen(needle);
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] &&
               tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int multi_has(int id) {
    for (int i = 0; i < g_n_multi; i++)
        if (g_multi[i] == id) return 1;
    return 0;
}

static void multi_toggle(int id) {
    for (int i = 0; i < g_n_multi; i++) {
        if (g_multi[i] == id) {
            memmove(&g_multi[i], &g_multi[i+1],
                    (size_t)(g_n_multi - i - 1) * sizeof(int));
            g_n_multi--;
            return;
        }
    }
    if (g_n_multi < 256) g_multi[g_n_multi++] = id;
}

static void multi_clear(void) { g_n_multi = 0; }

/* local IPC (browser native-messaging -> running app) */
static int g_ipc_sock = -1;

/* ---------------- helpers ---------------- */
static void human_bytes(double b, char *out, size_t cap) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    snprintf(out, cap, "%.1f %s", b, u[i]);
}
static void fmt_eta(double s, char *out, size_t cap) {
    if (s < 0) { snprintf(out, cap, "--:--"); return; }
    int t = (int)(s + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, sec);
    else       snprintf(out, cap, "%02d:%02d", m, sec);
}
static const char *state_name(cdm_job_state s) {
    switch (s) {
        case JOB_QUEUED:  return "Queued";
        case JOB_RUNNING: return "Downloading";
        case JOB_PAUSED:  return "Paused";
        case JOB_DONE:    return "Complete";
        case JOB_ERROR:   return "Error";
        case JOB_CANCELED:return "Canceled";
    }
    return "?";
}
static int64_t parse_speed(const char *s) {
    if (!s || !*s) return 0;
    char *end;
    double v = strtod(s, &end);
    if (end == s) return 0;
    if (*end == 'k' || *end == 'K') v *= 1024.0;
    else if (*end == 'm' || *end == 'M') v *= 1024.0 * 1024.0;
    else if (*end == 'g' || *end == 'G') v *= 1024.0 * 1024.0 * 1024.0;
    return (int64_t)v;
}
static time_t parse_hhmm(const char *s) {
    if (!s || sscanf(s, "%d:%d", &((int){0}), &((int){0})) != 2) return 0;
    int hh, mm;
    if (sscanf(s, "%d:%d", &hh, &mm) != 2) return 0;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    t->tm_hour = hh; t->tm_min = mm; t->tm_sec = 0;
    time_t e = mktime(t);
    if (e < now) e += 86400;
    return e;
}
/* Thread-safe localtime that also checks for failure (MSVC-safe). */
static struct tm *cdm_localtime(const time_t *t, struct tm *out) {
#ifdef _WIN32
    return localtime_s(out, t) == 0 ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

/* ---------------- theming ---------------- */
static void apply_theme(struct nk_context *ctx) {
    struct nk_color text, bg, panel, btn, border, track;
    if (g_theme == 0) {
        bg = nk_rgb(0x21,0x23,0x28); panel = nk_rgb(0x2b,0x2e,0x35);
        text = nk_rgb(0xe6,0xe6,0xe6); btn = nk_rgb(0x3a,0x3f,0x47);
        border = nk_rgb(0x12,0x12,0x14); track = nk_rgb(0x18,0x1a,0x1f);
    } else {
        bg = nk_rgb(0xf2,0xf2,0xf2); panel = nk_rgb(0xe6,0xe6,0xe6);
        text = nk_rgb(0x20,0x20,0x20); btn = nk_rgb(0xd8,0xd8,0xd8);
        border = nk_rgb(0xb0,0xb0,0xb0); track = nk_rgb(0xcf,0xcf,0xcf);
    }
    struct nk_color accent = g_skin==0 ? nk_rgb(0x4f,0x8c,0xff)
                         : g_skin==1 ? nk_rgb(0x2e,0xc4,0xb6)
                         : g_skin==2 ? nk_rgb(0xff,0x6b,0x6b)
                                     : nk_rgb(0x9b,0x59,0xb6);
    /* Selectable rows (category tree, job list) need themeable backgrounds
     * too: Nuklear's defaults are dark, which makes text unreadable in the
     * light theme (black on black). Active rows use the accent color. */
    struct nk_color sel_norm, sel_hover;
    if (g_theme == 0) {
        sel_norm = panel; sel_hover = btn;
    } else {
        sel_norm = nk_rgb(0xff,0xff,0xff); sel_hover = nk_rgb(0xcf,0xcf,0xcf);
    }
    ctx->style.text.color = text;
    ctx->style.window.background = bg;
    ctx->style.window.fixed_background = nk_style_item_color(panel);
    ctx->style.button.normal = nk_style_item_color(btn);
    ctx->style.button.hover  = nk_style_item_color(accent);
    ctx->style.button.active = nk_style_item_color(btn);
    ctx->style.button.text_normal = text;
    ctx->style.button.text_hover  = nk_rgb(255,255,255);
    ctx->style.button.border_color = border;
    ctx->style.button.rounding = 3;
    ctx->style.progress.normal = nk_style_item_color(track);
    ctx->style.progress.hover  = nk_style_item_color(track);
    ctx->style.progress.active = nk_style_item_color(track);
    ctx->style.progress.cursor_normal  = nk_style_item_color(accent);
    ctx->style.progress.cursor_hover  = nk_style_item_color(accent);
    ctx->style.progress.cursor_active = nk_style_item_color(accent);
    ctx->style.progress.border_color = border;

    /* popup/menu item buttons (menubar dropdowns + right-click menu) */
    ctx->style.menu_button.normal      = nk_style_item_hide();
    ctx->style.menu_button.hover       = nk_style_item_color(accent);
    ctx->style.menu_button.active      = nk_style_item_color(accent);
    ctx->style.menu_button.text_normal = text;
    ctx->style.menu_button.text_hover  = nk_rgb(255,255,255);
    ctx->style.menu_button.text_active = nk_rgb(255,255,255);
    ctx->style.menu_button.border_color = border;
    ctx->style.menu_button.rounding    = 2;

    ctx->style.contextual_button.normal      = nk_style_item_hide();
    ctx->style.contextual_button.hover       = nk_style_item_color(accent);
    ctx->style.contextual_button.active      = nk_style_item_color(accent);
    ctx->style.contextual_button.text_normal = text;
    ctx->style.contextual_button.text_hover  = nk_rgb(255,255,255);
    ctx->style.contextual_button.text_active = nk_rgb(255,255,255);
    ctx->style.contextual_button.border_color = border;
    ctx->style.contextual_button.rounding    = 2;

    ctx->style.window.menu_border_color     = border;
    ctx->style.window.contextual_border_color = border;
    ctx->style.window.popup_border_color    = border;

    /* text widgets (Add dialog edit field, category tree, list rows) */
    ctx->style.edit.text_normal = text;
    ctx->style.edit.text_hover  = text;
    ctx->style.edit.text_active = text;
    ctx->style.edit.cursor_text_normal = text;
    ctx->style.edit.cursor_text_hover  = text;
    ctx->style.edit.selected_text_normal = text;
    ctx->style.edit.selected_text_hover  = text;

    ctx->style.selectable.normal         = nk_style_item_color(sel_norm);
    ctx->style.selectable.hover          = nk_style_item_color(sel_hover);
    ctx->style.selectable.pressed        = nk_style_item_color(accent);
    ctx->style.selectable.normal_active  = nk_style_item_color(accent);
    ctx->style.selectable.hover_active   = nk_style_item_color(accent);
    ctx->style.selectable.pressed_active = nk_style_item_color(accent);
    ctx->style.selectable.text_normal = text;
    ctx->style.selectable.text_hover  = text;
    ctx->style.selectable.text_pressed = nk_rgb(255,255,255);
    ctx->style.selectable.text_normal_active = nk_rgb(255,255,255);
    ctx->style.selectable.text_hover_active  = nk_rgb(255,255,255);
    ctx->style.selectable.text_pressed_active = nk_rgb(255,255,255);
    ctx->style.selectable.text_background = sel_norm;
}

/* ---------------- menu/toolbar ---------------- */
static void draw_menubar(struct nk_context *ctx) {
    cdm_job *j = (g_mgr->selected_id >= 0)
        ? cdm_manager_find(g_mgr, g_mgr->selected_id) : NULL;

    nk_menubar_begin(ctx);
    nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 4);
    nk_layout_row_push(ctx, 0.12f);
    if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(160, 180))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "Add URL", NK_TEXT_LEFT)) g_show_add = 1;
        nk_menu_end(ctx);
    }
    nk_layout_row_push(ctx, 0.18f);
    if (nk_menu_begin_label(ctx, "Downloads", NK_TEXT_LEFT, nk_vec2(200, 260))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "Start / Resume", NK_TEXT_LEFT) && j
            && (j->state==JOB_PAUSED||j->state==JOB_QUEUED)) cdm_manager_resume(g_mgr, j->id);
        if (nk_menu_item_label(ctx, "Stop", NK_TEXT_LEFT) && j) cdm_manager_stop(g_mgr, j->id);
        if (nk_menu_item_label(ctx, "Stop All", NK_TEXT_LEFT)) cdm_manager_stop_all(g_mgr);
        if (nk_menu_item_label(ctx, "Delete", NK_TEXT_LEFT) && j) cdm_manager_remove(g_mgr, j->id);
        if (nk_menu_item_label(ctx, "Properties", NK_TEXT_LEFT) && j) { g_show_props=1;
            snprintf(g_props_speed,sizeof g_props_speed,"%lld",(long long)j->cfg.max_speed_bps); }
        if (nk_menu_item_label(ctx, "Add to Queue", NK_TEXT_LEFT) && j) cdm_manager_add_to_queue(g_mgr, j->id);
        if (nk_menu_item_label(ctx, "Delete from Queue", NK_TEXT_LEFT) && j) cdm_manager_remove_from_queue(g_mgr, j->id);
        if (nk_menu_item_label(ctx, "Delete All Completed", NK_TEXT_LEFT)) cdm_manager_delete_all_completed(g_mgr);
        if (nk_menu_item_label(ctx, "Categories...", NK_TEXT_LEFT)) g_show_cats = 1;
        if (nk_menu_item_label(ctx, "Stop selected", NK_TEXT_LEFT)) bulk_stop();
        if (nk_menu_item_label(ctx, "Resume selected", NK_TEXT_LEFT)) bulk_resume();
        if (nk_menu_item_label(ctx, "Delete selected", NK_TEXT_LEFT)) bulk_remove();
        nk_menu_end(ctx);
    }
    nk_layout_row_push(ctx, 0.18f);
    if (nk_menu_begin_label(ctx, "Options", NK_TEXT_LEFT, nk_vec2(180, 160))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "Settings...", NK_TEXT_LEFT)) g_show_opts = 1;
        nk_menu_end(ctx);
    }
    nk_layout_row_push(ctx, 0.12f);
    if (nk_menu_begin_label(ctx, "Help", NK_TEXT_LEFT, nk_vec2(160, 80))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_menu_item_label(ctx, "About", NK_TEXT_LEFT)) g_show_about = 1;
        nk_menu_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_menubar_end(ctx);
}

static void draw_toolbar(struct nk_context *ctx) {
    cdm_job *j = (g_mgr->selected_id >= 0)
        ? cdm_manager_find(g_mgr, g_mgr->selected_id) : NULL;

    nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 8);
    nk_layout_row_push(ctx, 0.14f);
    if (nk_button_label(ctx, "Add URL")) g_show_add = 1;
    nk_layout_row_push(ctx, 0.14f);
    if (nk_button_label(ctx, "Start/Resume") && j &&
        (j->state==JOB_PAUSED||j->state==JOB_QUEUED)) cdm_manager_resume(g_mgr, j->id);
    nk_layout_row_push(ctx, 0.09f);
    if (nk_button_label(ctx, "Stop") && j) cdm_manager_stop(g_mgr, j->id);
    nk_layout_row_push(ctx, 0.11f);
    if (nk_button_label(ctx, "Stop All")) cdm_manager_stop_all(g_mgr);
    nk_layout_row_push(ctx, 0.10f);
    if (nk_button_label(ctx, "Delete") && j) cdm_manager_remove(g_mgr, j->id);
    nk_layout_row_push(ctx, 0.16f);
    if (nk_button_label(ctx, "Delete Compl.")) cdm_manager_delete_all_completed(g_mgr);
    nk_layout_row_push(ctx, 0.11f);
    if (nk_button_label(ctx, "Options")) g_show_opts = 1;
    nk_layout_row_push(ctx, 0.12f);
    if (nk_button_label(ctx, "Scheduler")) g_show_sched = 1;
    nk_layout_row_end(ctx);

    nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 4);
    nk_layout_row_push(ctx, 0.24f);
    if (nk_button_label(ctx, "Add Batch")) g_show_batch = 1;
    nk_layout_row_push(ctx, 0.24f);
    if (nk_button_label(ctx, "Queues")) g_show_queues = 1;
    nk_layout_row_push(ctx, 0.26f);
    if (nk_button_label(ctx, "Network")) {
        g_net_proxy = g_mgr->proxy_mode;
        snprintf(g_net_proxy_url, sizeof(g_net_proxy_url), "%s", g_mgr->proxy_url);
        g_net_proxy_url_len = (int)strlen(g_net_proxy_url);
        snprintf(g_net_proxy_user, sizeof(g_net_proxy_user), "%s", g_mgr->proxy_user);
        g_net_proxy_user_len = (int)strlen(g_net_proxy_user);
        snprintf(g_net_proxy_pass, sizeof(g_net_proxy_pass), "%s", g_mgr->proxy_pass);
        g_net_proxy_pass_len = (int)strlen(g_net_proxy_pass);
        g_show_net = 1;
    }
    nk_layout_row_push(ctx, 0.26f);
    if (nk_button_label(ctx, "Edit") && j && !j->running) open_edit(j);
    nk_layout_row_end(ctx);
}

static int g_host_edit_idx = 0;
static int g_host_hmax = 0;
static char g_host_speed[32] = {0}; static int g_host_speed_len = 0;
static char g_host_ua[256] = {0}; static int g_host_ua_len = 0;

static void draw_net_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 300, win_h/2 - 260, 600, 520);
    if (nk_begin(ctx, "Network", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 2);
        nk_layout_row_push(ctx, 0.7f);
        nk_checkbox_label(ctx, "Browser integration server", &g_api_enabled);
        nk_layout_row_push(ctx, 0.3f);
        { char st[32];
          int p = cdm_ipc_port();
          snprintf(st, sizeof st, "%s", p > 0 ? "running" : "stopped");
          nk_label(ctx, st, NK_TEXT_LEFT); }
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 3);
        nk_layout_row_push(ctx, 0.3f); nk_label(ctx, "Port (0=auto):", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.3f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_api_port, &g_api_port_len,
                       sizeof(g_api_port)-1, nk_filter_decimal);
        nk_layout_row_push(ctx, 0.4f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_api_key, &g_api_key_len,
                       sizeof(g_api_key)-1, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        nk_label(ctx, "Key (optional, X-Api-Key):", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply server")) {
            persist_settings();
            restart_ipc_server();
        }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Proxy:", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.4f);
        { const char *modes[3] = {"Direct", "System", "Manual"};
          if (g_net_proxy < 0 || g_net_proxy > 2) g_net_proxy = CDM_PROXY_SYSTEM;
          g_net_proxy = nk_combo(ctx, modes, 3, g_net_proxy, 18, nk_vec2(160, 90)); }
        nk_layout_row_push(ctx, 0.6f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_net_proxy_url, &g_net_proxy_url_len,
                       sizeof(g_net_proxy_url)-1, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_net_proxy_user, &g_net_proxy_user_len,
                       sizeof(g_net_proxy_user)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_net_proxy_pass, &g_net_proxy_pass_len,
                       sizeof(g_net_proxy_pass)-1, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        nk_label(ctx, "Manual: host:port or URL; user/pass optional", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply proxy")) {
            cdm_manager_set_proxy(g_mgr, g_net_proxy,
                                  g_net_proxy_url_len ? g_net_proxy_url : "");
            cdm_manager_set_proxy_auth(g_mgr, g_net_proxy_user, g_net_proxy_pass);
            persist_settings();
        }
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 3);
        nk_layout_row_push(ctx, 0.34f);
        { int sp = g_mgr->sparse; nk_checkbox_label(ctx, "Sparse files", &sp);
          if (!!sp != !!g_mgr->sparse) { g_mgr->sparse = sp; persist_settings(); } }
        nk_layout_row_push(ctx, 0.33f);
        { int pt = g_mgr->preserve_time; nk_checkbox_label(ctx, "Keep file time", &pt);
          if (!!pt != !!g_mgr->preserve_time) { g_mgr->preserve_time = pt; persist_settings(); } }
        nk_layout_row_push(ctx, 0.33f);
        { int is = g_mgr->ignore_ssl; nk_checkbox_label(ctx, "Ignore TLS errors", &is);
          if (!!is != !!g_mgr->ignore_ssl) { g_mgr->ignore_ssl = is; persist_settings(); } }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Per-host rules (host | max conn | speed B/s | del):", NK_TEXT_LEFT);
        for (int i = 0; i < g_mgr->n_hosts; i++) {
            cdm_host_rule *h = &g_mgr->hosts[i];
            nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 4);
            nk_layout_row_push(ctx, 0.40f); nk_label(ctx, h->host, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 0.20f);
            { char ms[24]; snprintf(ms, sizeof ms, "%d", h->max_connections);
              nk_label(ctx, ms, NK_TEXT_LEFT); }
            nk_layout_row_push(ctx, 0.24f);
            { char sp[32]; snprintf(sp, sizeof sp, "%lld", (long long)h->max_speed_bps);
              nk_label(ctx, sp, NK_TEXT_LEFT); }
            nk_layout_row_push(ctx, 0.16f);
            if (nk_button_label(ctx, "Del")) {
                cdm_manager_host_remove(g_mgr, i);
                persist_settings();
            }
            nk_layout_row_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 3);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_newhost, &g_newhost_len,
                       sizeof(g_newhost)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Add")) {
            if (g_newhost_len > 0) {
                cdm_manager_host_add(g_mgr, g_newhost);
                g_newhost[0]=0; g_newhost_len=0;
                persist_settings();
            }
        }
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Close")) {
            g_newhost[0]=0; g_newhost_len=0; g_show_net=0;
        }
        nk_layout_row_end(ctx);

        /* edit the selected host's limits */
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Edit host limits (max conn, speed B/s, UA):", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 3);
        nk_layout_row_push(ctx, 0.40f);
        { const char *names[CDM_MAX_HOSTS];
          for (int i = 0; i < g_mgr->n_hosts; i++) names[i] = g_mgr->hosts[i].host;
          if (g_host_edit_idx >= g_mgr->n_hosts) g_host_edit_idx = 0;
          if (g_mgr->n_hosts > 0)
              g_host_edit_idx = nk_combo(ctx, names, g_mgr->n_hosts, g_host_edit_idx,
                                         18, nk_vec2(200, 200)); }
        nk_layout_row_push(ctx, 0.24f);
        nk_property_int(ctx, "#", 0, &g_host_hmax, 64, 1, 1);
        nk_layout_row_push(ctx, 0.36f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_host_speed, &g_host_speed_len,
                       sizeof(g_host_speed)-1, nk_filter_decimal);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.15f); nk_label(ctx, "UA:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.85f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_host_ua, &g_host_ua_len,
                       sizeof(g_host_ua)-1, nk_filter_default);
        nk_layout_row_end(ctx);
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, "Apply host limits") && g_mgr->n_hosts > 0) {
            cdm_manager_host_set(g_mgr, g_host_edit_idx, g_host_hmax,
                                 parse_speed(g_host_speed), g_host_ua);
            g_host_speed[0]=0; g_host_speed_len=0;
            g_host_ua[0]=0; g_host_ua_len=0;
            persist_settings();
        }
    }
    nk_end(ctx);
}

static void open_edit(cdm_job *j) {
    static int edit_id = -1;
    if (!j) return;
    cdm_mutex_lock(j->mtx);
    snprintf(g_edit_url, sizeof(g_edit_url), "%s", j->url);
    snprintf(g_edit_out, sizeof(g_edit_out), "%s", j->outpath);
    snprintf(g_edit_speed, sizeof(g_edit_speed), "%lld",
             (long long)j->cfg.max_speed_bps);
    snprintf(g_edit_ua, sizeof(g_edit_ua), "%s", j->net_ua);
    snprintf(g_edit_referer, sizeof(g_edit_referer), "%s", j->net_referer);
    snprintf(g_edit_cookie, sizeof(g_edit_cookie), "%s", j->net_cookie);
    g_edit_hdrs[0] = 0;
    /* rebuild the textarea from the parsed headers */
    {
        size_t off = 0;
        for (int i = 0; i < j->n_hdrs && off + 1 < sizeof(g_edit_hdrs); i++) {
            int w = snprintf(g_edit_hdrs + off, sizeof(g_edit_hdrs) - off,
                             "%s%s", i ? "\n" : "", j->hdr_ptrs[i]);
            if (w < 0) break;
            off += (size_t)w;
            if (off >= sizeof(g_edit_hdrs)) { off = sizeof(g_edit_hdrs) - 1; break; }
        }
    }
    g_edit_queue = j->queue_idx;
    edit_id = j->id;
    cdm_mutex_unlock(j->mtx);
    g_edit_url_len = (int)strlen(g_edit_url);
    g_edit_out_len = (int)strlen(g_edit_out);
    g_edit_speed_len = (int)strlen(g_edit_speed);
    g_edit_ua_len = (int)strlen(g_edit_ua);
    g_edit_referer_len = (int)strlen(g_edit_referer);
    g_edit_cookie_len = (int)strlen(g_edit_cookie);
    g_edit_hdrs_len = (int)strlen(g_edit_hdrs);
    g_edit_id = edit_id;
    g_show_edit = 1;
}

/* ---------------- category tree ---------------- */
/* NOTE: this group IS a pushed item of the outer 2-column body row in
 * draw_body(); do not open another layout row here (nested rows collapse
 * the outer columns and hide the download list). */
static void draw_categories(struct nk_context *ctx) {
    if (nk_group_begin(ctx, "cats", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_selectable_label(ctx, "All Downloads", NK_TEXT_LEFT,
                                &(int){g_cat_filter == -1})) {
            g_cat_filter = -1;
        }
        for (int i = 0; i < g_mgr->n_cats; i++) {
            int sel = (g_cat_filter == i);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_selectable_label(ctx, g_mgr->cats[i].name, NK_TEXT_LEFT, &sel))
                g_cat_filter = sel ? i : -1;
        }
        nk_group_end(ctx);
    }
}

/* ---------------- download list ---------------- */
/* Same nesting rule as draw_categories: the "jobs" group is the pushed row
 * item itself. Stats are returned via out-params; the status bar is drawn
 * by draw_body() after the row ends. */
/* Flat display snapshot so the list can filter + sort without holding locks. */
#define CDM_LIST_CAP 4096
typedef struct {
    int id;
    char name[256];
    int64_t size, dl;
    double speed, eta;
    int xfer;
    int state, qidx, sched;
    double pct;
} list_row_t;

static int list_row_cmp(const void *a, const void *b) {
    const list_row_t *x = (const list_row_t *)a;
    const list_row_t *y = (const list_row_t *)b;
    int r = 0;
    switch (g_sort_col) {
    case 0: {
        const char *p = x->name, *q = y->name;
        while (*p && *q &&
               tolower((unsigned char)*p) == tolower((unsigned char)*q)) {
            p++;
            q++;
        }
        r = (int)tolower((unsigned char)*p) - (int)tolower((unsigned char)*q);
        break;
    }
    case 1: r = (x->size > y->size) - (x->size < y->size); break;
    case 2: r = x->state - y->state; break;
    case 3: r = (x->pct > y->pct) - (x->pct < y->pct); break;
    case 4: r = (x->speed > y->speed) - (x->speed < y->speed); break;
    default: r = 0; break;
    }
    if (r == 0) r = x->id - y->id;
    return r * g_sort_dir;
}

static void draw_list(struct nk_context *ctx, int *active, double *total_speed) {
    *active = 0;
    *total_speed = 0;
    char s_size[32], s_spd[32], s_eta[16], s_dl[32];
    static list_row_t rows[CDM_LIST_CAP];
    int nrows = 0;

    if (nk_group_begin(ctx, "jobs", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        /* search */
        nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 2);
        nk_layout_row_push(ctx, 0.15f); nk_label(ctx, "Search:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.85f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_search, &g_search_len,
                       sizeof(g_search)-1, nk_filter_default);
        nk_layout_row_end(ctx);

        /* snapshot visible jobs */
        cdm_mutex_lock(g_mgr->mtx);
        for (int i = 0; i < g_mgr->count && nrows < CDM_LIST_CAP; i++) {
            cdm_job *job = g_mgr->jobs[i];
            list_row_t *r;
            cdm_progress p;
            int st, qidx, sched, id, cat;
            char url[2048], out[2048];
            if (g_cat_filter >= 0 && job->cat_idx != g_cat_filter) continue;
            cdm_mutex_lock(job->mtx);
            p = job->prog;
            st = job->state;
            qidx = job->queue_idx;
            sched = (job->sched_epoch > 0);
            id = job->id;
            cat = job->cat_idx;
            snprintf(url, sizeof(url), "%s", job->url);
            snprintf(out, sizeof(out), "%s", job->outpath);
            cdm_mutex_unlock(job->mtx);
            (void)cat;
            {
                const char *bn = out[0] ? strrchr(out, '/') : NULL;
                const char *bu = strrchr(url, '/');
#ifdef _WIN32
                if (out[0]) {
                    const char *w = strrchr(out, '\\');
                    if (w && (!bn || w > bn)) bn = w;
                }
#endif
                if (!bn) bn = bu ? bu + 1 : url;
                else bn = bn + 1;
                if (!bn || !*bn) bn = url;
                r = &rows[nrows];
                r->id = id;
                snprintf(r->name, sizeof(r->name), "%s", bn);
                if (!contains_i(r->name, g_search) && !contains_i(url, g_search))
                    continue;
                r->size = p.total_bytes;
                r->dl = p.downloaded_bytes;
                r->speed = p.speed_bps;
                r->eta = p.eta_seconds;
                r->xfer = p.active_connections;
                r->state = st;
                r->qidx = qidx;
                r->sched = sched;
                r->pct = (p.total_bytes > 0 && p.downloaded_bytes > 0)
                         ? 100.0 * (double)p.downloaded_bytes / (double)p.total_bytes : 0.0;
                nrows++;
            }
            if (st == JOB_RUNNING) { (*active)++; *total_speed += p.speed_bps; }
        }
        cdm_mutex_unlock(g_mgr->mtx);
        if (g_sort_col >= 0)
            qsort(rows, (size_t)nrows, sizeof(rows[0]), list_row_cmp);

        /* headers (click to sort) */
        nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 9);
        float w[9] = {0.24f,0.09f,0.10f,0.18f,0.08f,0.08f,0.09f,0.06f,0.04f};
        const char *hdr[9] = {"File","Size","Status","Progress","Speed",
                              "Time","Downloaded","Xfer","Q"};
        const int sortcol[9] = {0, 1, 2, 3, 4, -1, -1, -1, -1};
        for (int c = 0; c < 9; c++) {
            char lab[32];
            nk_layout_row_push(ctx, w[c]);
            if (sortcol[c] >= 0) {
                if (g_sort_col == sortcol[c])
                    snprintf(lab, sizeof lab, "%s %s", hdr[c],
                             g_sort_dir > 0 ? "▲" : "▼");
                else
                    snprintf(lab, sizeof lab, "%s", hdr[c]);
                if (nk_button_label(ctx, lab)) {
                    if (g_sort_col == sortcol[c]) g_sort_dir = -g_sort_dir;
                    else { g_sort_col = sortcol[c]; g_sort_dir = 1; }
                }
            } else {
                nk_label(ctx, hdr[c], NK_TEXT_LEFT);
            }
        }
        nk_layout_row_end(ctx);

        {
            int ctrl = nk_input_is_key_down(&ctx->input, NK_KEY_CTRL);
            for (int i = 0; i < nrows; i++) {
                list_row_t *r = &rows[i];
                human_bytes(r->size > 0 ? (double)r->size : 0, s_size, sizeof s_size);
                human_bytes(r->speed, s_spd, sizeof s_spd);
                human_bytes((double)r->dl, s_dl, sizeof s_dl);
                fmt_eta(r->eta, s_eta, sizeof s_eta);
                {
                    char stxt[32];
                    const char *base = (r->state == JOB_QUEUED && r->sched) ? "Scheduled"
                                       : state_name(r->state);
                    snprintf(stxt, sizeof stxt, "%s", base);
                    nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 9);
                    nk_layout_row_push(ctx, w[0]);
                    { int sel = (r->id == g_mgr->selected_id) || multi_has(r->id);
                      if (nk_selectable_label(ctx, r->name, NK_TEXT_LEFT, &sel)) {
                          if (ctrl) {
                              multi_toggle(r->id);
                              cdm_manager_select(g_mgr, r->id);
                          } else {
                              multi_clear();
                              cdm_manager_select(g_mgr, sel ? r->id : -1);
                          }
                      } }
                    nk_layout_row_push(ctx, w[1]); nk_label(ctx, s_size, NK_TEXT_LEFT);
                    nk_layout_row_push(ctx, w[2]); nk_label(ctx, stxt, NK_TEXT_LEFT);
                    nk_layout_row_push(ctx, w[3]);
                    { nk_size cur = (nk_size)r->pct, mx = 100;
                      nk_progress(ctx, &cur, mx, NK_FIXED); }
                    nk_layout_row_push(ctx, w[4]); nk_label(ctx, r->state==JOB_RUNNING?s_spd:"-", NK_TEXT_LEFT);
                    nk_layout_row_push(ctx, w[5]); nk_label(ctx, r->state==JOB_RUNNING?s_eta:"-", NK_TEXT_LEFT);
                    nk_layout_row_push(ctx, w[6]); nk_label(ctx, s_dl, NK_TEXT_LEFT);
                    nk_layout_row_push(ctx, w[7]);
                    { char x[8]; snprintf(x,sizeof x,"%d",r->xfer); nk_label(ctx,x,NK_TEXT_LEFT); }
                    nk_layout_row_push(ctx, w[8]);
                    { char q[8];
                      if (r->qidx >= 0) snprintf(q, sizeof q, "Q%d", r->qidx);
                      else snprintf(q, sizeof q, "%s", r->sched ? "S" : "-");
                      nk_label(ctx, q, NK_TEXT_LEFT); }
                    nk_layout_row_end(ctx);
                }
            }
        }
        nk_group_end(ctx);
    }
}

/* Body row shared by the app and the headless test hook: categories on the
 * left, download list on the right, status bar underneath. */
static void draw_body(struct nk_context *ctx, float h) {
    int active = 0;
    double total_speed = 0;

    nk_layout_row_begin(ctx, NK_DYNAMIC, h - 172.0f, 2);
    nk_layout_row_push(ctx, 0.18f);
    draw_categories(ctx);
    nk_layout_row_push(ctx, 0.82f);
    draw_list(ctx, &active, &total_speed);
    nk_layout_row_end(ctx);

    char status[192];
    snprintf(status, sizeof status,
             "Active: %d   Total speed: %.1f %s/s   Max: %d   Sel: %d   Theme: %s",
             active, total_speed >= 1024 ? total_speed/1024 : total_speed,
             total_speed >= 1024 ? "KB" : "B", g_mgr->max_active,
             g_n_multi > 0 ? g_n_multi : (g_mgr->selected_id >= 0 ? 1 : 0),
             g_theme ? "Light" : "Dark");
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label(ctx, status, NK_TEXT_LEFT);
}

/* ---------------- dialogs ---------------- */
static void submit_add(void) {
    if (g_url_len <= 0) return;
    int64_t sp = parse_speed(g_speed);
    time_t ep = parse_hhmm(g_sched);
    char outpath[2200];
    if (g_out[0]) snprintf(outpath,sizeof outpath,"%s",g_out);
    else {
        const char *bn = strrchr(g_url,'/'); bn = bn?bn+1:g_url;
        if (!*bn) bn = "download.bin";
        /* dir is at most 1023 chars; cap the basename so dir + '/' +
         * name + NUL always fits outpath (dir[1024] + 1 + 1174 + 1 <= 2200). */
        snprintf(outpath,sizeof outpath,"%s/%.1174s", g_mgr->cats[g_cat_idx].dir, bn);
    }
    cdm_config cfg = g_cfg; cfg.max_speed_bps = sp;
    cfg.user_agent = g_add_ua_len ? g_add_ua : NULL;
    cfg.headers_text = g_add_hdrs_len ? g_add_hdrs : NULL;
    int id;
    if (g_add_queue >= 0)
        id = cdm_manager_add_to_queue_idx(g_mgr, g_url, outpath, &cfg, g_add_queue, ep);
    else
        id = cdm_manager_add_ex(g_mgr, g_url, outpath, &cfg, 0, ep);
    if (id >= 0) {
        cdm_job *jj = cdm_manager_find(g_mgr, id);
        if (jj) jj->cat_idx = g_cat_idx;
    }
    g_url[0]=0; g_url_len=0; g_out[0]=0; g_out_len=0;
    g_speed[0]=0; g_speed_len=0; g_sched[0]=0; g_sched_len=0;
    g_add_ua[0]=0; g_add_ua_len=0; g_add_hdrs[0]=0; g_add_hdrs_len=0;
    g_add_queue=-1; g_show_add=0;
}

/* Queue combo items: [0] = action label, [1..n] = queue names. */
static int queue_combo(struct nk_context *ctx, int *sel, const char *now_label) {
    char items[CDM_MAX_QUEUES + 1][80];
    const char *ptrs[CDM_MAX_QUEUES + 1];
    int n = 0;
    snprintf(items[0], sizeof(items[0]), "%s", now_label);
    ptrs[0] = items[0];
    n = 1;
    for (int i = 0; i < g_mgr->n_queues && n < CDM_MAX_QUEUES + 1; i++) {
        snprintf(items[n], sizeof(items[n]), "Queue: %.70s", g_mgr->queues[i].name);
        ptrs[n] = items[n];
        n++;
    }
    /* sel: -1 = now/action, else queue index = sel - 1 */
    int cur = *sel + 1;
    if (cur < 0) cur = 0;
    if (cur >= n) cur = n - 1;
    int s = nk_combo(ctx, ptrs, n, cur, 20, nk_vec2(220, 200));
    *sel = s - 1;
    return *sel;
}

static void draw_add_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 260, win_h/2 - 160, 520, 320);
    if (nk_begin(ctx, "Add Download", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "URL:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_url, &g_url_len, sizeof(g_url)-1, nk_filter_default);

        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Save as (optional):", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_out, &g_out_len, sizeof(g_out)-1, nk_filter_default);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Category:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        { const char *names[16]; for (int i=0;i<g_mgr->n_cats;i++) names[i]=g_mgr->cats[i].name;
          int s = nk_combo(ctx, names, g_mgr->n_cats, g_cat_idx, 20, nk_vec2(200,200));
          g_cat_idx = s; }
        nk_layout_row_end(ctx);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Speed limit (e.g. 500K, 0=off):", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_speed, &g_speed_len, sizeof(g_speed)-1, nk_filter_decimal);
        nk_layout_row_end(ctx);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Start at (HH:MM, today):", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_sched, &g_sched_len, sizeof(g_sched)-1, nk_filter_default);
        nk_layout_row_end(ctx);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Start:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        queue_combo(ctx, &g_add_queue, "Download now");
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "User-Agent (optional):", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_add_ua, &g_add_ua_len,
                       sizeof(g_add_ua)-1, nk_filter_default);

        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Extra headers, one Name: value per line:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 64, 1);
        nk_edit_string(ctx, NK_EDIT_BOX, g_add_hdrs, &g_add_hdrs_len,
                       sizeof(g_add_hdrs)-1, nk_filter_default);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Download")) submit_add();
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Cancel")) {
            g_url[0]=0; g_url_len=0; g_out[0]=0; g_out_len=0;
            g_speed[0]=0; g_speed_len=0; g_sched[0]=0; g_sched_len=0;
            g_add_ua[0]=0; g_add_ua_len=0; g_add_hdrs[0]=0; g_add_hdrs_len=0;
            g_add_queue=-1; g_show_add=0;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void submit_batch(void) {
    /* split textarea into lines, expand {a:b} ranges, enqueue */
    char *lines[512];
    int n = 0;
    char copy[8192];
    snprintf(copy, sizeof(copy), "%s", g_batch);
    for (char *p = copy; *p && n < 512;) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        lines[n++] = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = 0;
        /* rtrim */
        char *e = lines[n-1] + strlen(lines[n-1]);
        while (e > lines[n-1] && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!lines[n-1][0]) n--;
    }
    {
        static char expanded[512][2048];
        const char *urls[512];
        int total = 0;
        cdm_config cfg = g_cfg;
        cfg.quiet = 1;
        for (int i = 0; i < n && total < 512; i++) {
            int got = cdm_expand_range(lines[i], &expanded[total], 512 - total);
            for (int k = 0; k < got && total < 512; k++) {
                urls[total] = expanded[total];
                total++;
            }
        }
        if (total > 0)
            cdm_manager_add_batch(g_mgr, urls, total, &cfg, g_batch_queue);
    }
    g_batch[0]=0; g_batch_len=0; g_show_batch=0;
}

static void draw_batch_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 280, win_h/2 - 200, 560, 400);
    if (nk_begin(ctx, "Add Batch", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "One URL per line. Use {start:end} for ranges:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 240, 1);
        nk_edit_string(ctx, NK_EDIT_BOX, g_batch, &g_batch_len,
                       sizeof(g_batch)-1, nk_filter_default);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Start:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        queue_combo(ctx, &g_batch_queue, "Download now");
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Add all")) submit_batch();
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Cancel")) {
            g_batch[0]=0; g_batch_len=0; g_show_batch=0;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void persist_settings(void) {
    cdm_settings st;
    st.max_active = g_mgr->max_active;
    st.theme = g_theme;
    st.skin = g_skin;
    st.api_enabled = g_api_enabled;
    st.api_port = atoi(g_api_port);
    if (st.api_port < 0 || st.api_port > 65535) st.api_port = 0;
    snprintf(st.api_key, sizeof(st.api_key), "%s", g_api_key);
    st.tray_close = g_tray_close;
    st.autostart = g_autostart;
    st.update_check = g_update_check;
    st.tray_icon = g_tray_icon;
    st.tray_minimize = g_tray_minimize;
    st.prog_popup = g_prog_popup;
    st.done_popup = g_done_popup;
    st.confirm_exit = g_confirm_exit;
    st.sounds = g_sounds;
    st.dup_mode = g_mgr->dup_mode;
    st.ignore_ssl = g_mgr->ignore_ssl;
    st.sparse = g_mgr->sparse;
    st.preserve_time = g_mgr->preserve_time;
    snprintf(st.proxy_user, sizeof(st.proxy_user), "%s", g_mgr->proxy_user);
    snprintf(st.proxy_pass, sizeof(st.proxy_pass), "%s", g_mgr->proxy_pass);
    cdm_settings_save(g_mgr, &st);
}

static void restart_ipc_server(void) {
    cdm_ipc_stop();
    if (g_api_enabled) {
        int port = atoi(g_api_port);
        if (port <= 0 || port > 65535) port = 0; /* default */
        cdm_ipc_start(g_mgr, port, g_api_key);
    }
}

static void draw_queues_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 300, win_h/2 - 220, 600, 440);
    if (nk_begin(ctx, "Queues", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Name | Max (global=-1) | Window HH:MM-HH:MM | On | Del", NK_TEXT_LEFT);
        for (int i = 0; i < g_mgr->n_queues; i++) {
            cdm_queue *q = &g_mgr->queues[i];
            char ws[16], we[16];
            if (q->sched_start_min < 0) snprintf(ws, sizeof ws, "--:--");
            else snprintf(ws, sizeof ws, "%02d:%02d", q->sched_start_min / 60, q->sched_start_min % 60);
            if (q->sched_end_min < 0) snprintf(we, sizeof we, "--:--");
            else snprintf(we, sizeof we, "%02d:%02d", q->sched_end_min / 60, q->sched_end_min % 60);
            nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 5);
            nk_layout_row_push(ctx, 0.30f); nk_label(ctx, q->name, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 0.22f);
            { int mx = q->max_active;
              nk_property_int(ctx, "#", -1, &mx, 16, 1, 1);
              if (mx != q->max_active) {
                  cdm_manager_queue_set(g_mgr, i, NULL, mx, q->sched_start_min,
                                        q->sched_end_min, q->enabled);
                  persist_settings();
              } }
            nk_layout_row_push(ctx, 0.26f);
            { char win[16]; snprintf(win, sizeof win, "%s-%s", ws, we);
              nk_label(ctx, win, NK_TEXT_LEFT); }
            nk_layout_row_push(ctx, 0.10f);
            { int en = q->enabled;
              if (nk_checkbox_label(ctx, "", &en)) {
                  cdm_manager_queue_set(g_mgr, i, NULL, q->max_active,
                                        q->sched_start_min, q->sched_end_min, en);
                  persist_settings();
              } }
            nk_layout_row_push(ctx, 0.12f);
            if (i > 0 && nk_button_label(ctx, "Del")) {
                cdm_manager_queue_remove(g_mgr, i);
                persist_settings();
            } else nk_label(ctx, "", NK_TEXT_LEFT);
            nk_layout_row_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 3);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_newq_name, &g_newq_name_len,
                       sizeof(g_newq_name)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Add")) {
            if (g_newq_name_len > 0) {
                cdm_manager_queue_add(g_mgr, g_newq_name);
                g_newq_name[0]=0; g_newq_name_len=0;
                persist_settings();
            }
        }
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Close")) {
            g_newq_name[0]=0; g_newq_name_len=0; g_show_queues=0;
        }
        nk_layout_row_end(ctx);

        /* time-window editor for the selected queue */
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Daily window for queue (empty = no bound):", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 4);
        nk_layout_row_push(ctx, 0.34f);
        { const char *names[CDM_MAX_QUEUES];
          for (int i = 0; i < g_mgr->n_queues; i++) names[i] = g_mgr->queues[i].name;
          if (g_qedit_idx >= g_mgr->n_queues) g_qedit_idx = 0;
          g_qedit_idx = nk_combo(ctx, names, g_mgr->n_queues, g_qedit_idx,
                                 20, nk_vec2(200, 200)); }
        nk_layout_row_push(ctx, 0.22f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_qedit_start, &g_qedit_start_len,
                       sizeof(g_qedit_start)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.22f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_qedit_end, &g_qedit_end_len,
                       sizeof(g_qedit_end)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.22f);
        if (nk_button_label(ctx, "Set")) {
            cdm_queue *q = &g_mgr->queues[g_qedit_idx];
            cdm_manager_queue_set(g_mgr, g_qedit_idx, NULL, q->max_active,
                                  parse_hhmm_min(g_qedit_start),
                                  parse_hhmm_min(g_qedit_end), q->enabled);
            g_qedit_start[0]=0; g_qedit_start_len=0;
            g_qedit_end[0]=0; g_qedit_end_len=0;
            persist_settings();
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_cats_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 300, win_h/2 - 220, 600, 440);
    if (nk_begin(ctx, "Categories", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Name | Folder (slots 0-6 map extensions, locked)", NK_TEXT_LEFT);
        for (int i = 0; i < g_mgr->n_cats; i++) {
            cdm_category *c = &g_mgr->cats[i];
            nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 3);
            nk_layout_row_push(ctx, 0.28f);
            { char nm[64]; int nl;
              snprintf(nm, sizeof nm, "%s", c->name); nl = (int)strlen(nm);
              nk_edit_string(ctx, NK_EDIT_FIELD, nm, &nl, sizeof(nm)-1,
                             nk_filter_default);
              if (nl > 0 && strcmp(nm, c->name) != 0)
                  cdm_manager_category_set(g_mgr, i, nm, NULL);
            }
            nk_layout_row_push(ctx, 0.56f);
            { char dr[1024]; int dl;
              snprintf(dr, sizeof dr, "%s", c->dir); dl = (int)strlen(dr);
              nk_edit_string(ctx, NK_EDIT_FIELD, dr, &dl, sizeof(dr)-1,
                             nk_filter_default);
              if (dl > 0 && strcmp(dr, c->dir) != 0)
                  cdm_manager_category_set(g_mgr, i, NULL, dr);
            }
            nk_layout_row_push(ctx, 0.16f);
            if (i >= 7 && nk_button_label(ctx, "Del")) {
                cdm_manager_category_remove(g_mgr, i);
                persist_settings();
            } else nk_label(ctx, i >= 7 ? "" : "*", NK_TEXT_LEFT);
            nk_layout_row_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 3);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_newcat_name, &g_newcat_name_len,
                       sizeof(g_newcat_name)-1, nk_filter_default);
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Add")) {
            if (g_newcat_name_len > 0) {
                cdm_manager_category_add(g_mgr, g_newcat_name);
                g_newcat_name[0]=0; g_newcat_name_len=0;
                persist_settings();
            }
        }
        nk_layout_row_push(ctx, 0.25f);
        if (nk_button_label(ctx, "Close")) {
            g_newcat_name[0]=0; g_newcat_name_len=0;
            persist_settings();
            g_show_cats=0;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_edit_modal(struct nk_context *ctx, int win_w, int win_h) {
    cdm_job *j = (g_edit_id >= 0) ? cdm_manager_find(g_mgr, g_edit_id) : NULL;
    if (!j) { g_show_edit = 0; return; }
    struct nk_rect r = nk_rect(win_w/2 - 260, win_h/2 - 180, 520, 360);
    if (nk_begin(ctx, "Edit Download", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "URL:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_url, &g_edit_url_len,
                       sizeof(g_edit_url)-1, nk_filter_default);
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Save as:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_out, &g_edit_out_len,
                       sizeof(g_edit_out)-1, nk_filter_default);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Speed (e.g. 500K, 0=off):", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_speed, &g_edit_speed_len,
                       sizeof(g_edit_speed)-1, nk_filter_decimal);
        nk_layout_row_end(ctx);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Queue:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        queue_combo(ctx, &g_edit_queue, "Run immediately");
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "User-Agent:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_ua, &g_edit_ua_len,
                       sizeof(g_edit_ua)-1, nk_filter_default);
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Referer:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_referer, &g_edit_referer_len,
                       sizeof(g_edit_referer)-1, nk_filter_default);
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Cookie:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_edit_cookie, &g_edit_cookie_len,
                       sizeof(g_edit_cookie)-1, nk_filter_default);
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Extra headers, one Name: value per line:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 64, 1);
        nk_edit_string(ctx, NK_EDIT_BOX, g_edit_hdrs, &g_edit_hdrs_len,
                       sizeof(g_edit_hdrs)-1, nk_filter_default);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply")) {
            cdm_manager_edit(g_mgr, g_edit_id,
                             g_edit_url_len ? g_edit_url : NULL,
                             g_edit_out_len ? g_edit_out : NULL,
                             parse_speed(g_edit_speed), g_edit_queue);
            cdm_manager_edit_net(g_mgr, g_edit_id,
                                 g_edit_ua, g_edit_referer, g_edit_cookie,
                                 g_edit_hdrs);
            g_show_edit = 0;
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Close")) g_show_edit = 0;
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_props_modal(struct nk_context *ctx) {
    cdm_job *j = (g_mgr->selected_id>=0)?cdm_manager_find(g_mgr,g_mgr->selected_id):NULL;
    if (!j) { g_show_props = 0; return; }
    struct nk_rect r = nk_rect(360, 260, 360, 160);
    if (nk_begin(ctx, "Properties", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, j->url, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 24, 1); nk_label(ctx, "Speed limit (bytes/s, 0=off):", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_edit_string(ctx, NK_EDIT_FIELD, g_props_speed, &g_props_speed_len,
                       sizeof(g_props_speed)-1, nk_filter_decimal);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply")) {
            j->cfg.max_speed_bps = parse_speed(g_props_speed);
            g_show_props = 0;
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Close")) g_show_props = 0;
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_opts_modal(struct nk_context *ctx) {
    struct nk_rect r = nk_rect(360, 160, 380, 420);
    if (nk_begin(ctx, "Options", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        /* Staged copies: combos/property edit these, globals change only on
         * Apply (Close discards). Seeded from live values on each opening. */
        static int dlg_maxact = -1, dlg_theme = -1, dlg_skin = -1;
        static int dlg_tray = -1, dlg_auto = -1, dlg_upd = -1, dlg_path = -1;
        static int dlg_ticon = -1, dlg_tmin = -1, dlg_dup = -1;
        static int dlg_prog = -1, dlg_done = -1, dlg_confirm = -1, dlg_snd = -1;
        if (dlg_maxact < 0) {
            dlg_maxact = g_mgr->max_active;
            dlg_theme = g_theme;
            dlg_skin = g_skin;
            dlg_tray = g_tray_close;
            dlg_auto = g_autostart;
            dlg_upd = g_update_check;
            dlg_path = g_inpath = cdm_path_get();
            dlg_ticon = g_tray_icon;
            dlg_tmin = g_tray_minimize;
            dlg_dup = g_mgr->dup_mode;
            dlg_prog = g_prog_popup;
            dlg_done = g_done_popup;
            dlg_confirm = g_confirm_exit;
            dlg_snd = g_sounds;
        }
        nk_layout_row_dynamic(ctx, 24, 1); nk_label(ctx, "Max concurrent downloads:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_property_int(ctx, "#", 0, &dlg_maxact, 16, 1, 1);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Theme:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        { const char *th[2]={"Dark","Light"};
          dlg_theme=nk_combo(ctx,th,2,dlg_theme,18,nk_vec2(120,80)); }
        nk_layout_row_end(ctx);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Skin:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        { const char *sk[4]={"Blue","Teal","Red","Purple"};
          dlg_skin=nk_combo(ctx,sk,4,dlg_skin,18,nk_vec2(120,100)); }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Show tray icon", &dlg_ticon);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Close to tray instead of quitting", &dlg_tray);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Minimize to tray", &dlg_tmin);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Start with Windows", &dlg_auto);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Add install folder to user PATH", &dlg_path);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Check for updates at startup", &dlg_upd);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
        nk_layout_row_push(ctx, 0.5f); nk_label(ctx, "Existing files:", NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.5f);
        { const char *dm[2]={"Auto-rename","Overwrite"};
          if (dlg_dup < 0 || dlg_dup > 1) dlg_dup = 0;
          dlg_dup=nk_combo(ctx,dm,2,dlg_dup,18,nk_vec2(140,60)); }
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Progress dialog on start", &dlg_prog);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Completion dialog on finish", &dlg_done);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Confirm exit while active", &dlg_confirm);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_checkbox_label(ctx, "Sounds", &dlg_snd);

        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply")) {
            g_theme = dlg_theme;
            g_skin = dlg_skin;
            g_tray_close = dlg_tray ? 1 : 0;
            g_tray_minimize = dlg_tmin ? 1 : 0;
            g_update_check = dlg_upd ? 1 : 0;
            g_prog_popup = dlg_prog ? 1 : 0;
            g_done_popup = dlg_done ? 1 : 0;
            g_confirm_exit = dlg_confirm ? 1 : 0;
            g_sounds = dlg_snd ? 1 : 0;
            g_mgr->dup_mode = dlg_dup ? 1 : 0;
            if (!!g_tray_icon != !!dlg_ticon) {
                g_tray_icon = dlg_ticon ? 1 : 0;
                if (g_tray_icon) g_tray_up = (cdm_tray_init() == 0);
                else { cdm_tray_shutdown(); g_tray_up = 0; }
            }
            if (!!g_autostart != !!dlg_auto) {
                if (cdm_autostart_set(dlg_auto ? 1 : 0) == 0)
                    g_autostart = dlg_auto ? 1 : 0;
            }
            if (!!g_inpath != !!dlg_path) {
                if (cdm_path_set(dlg_path ? 1 : 0) == 0)
                    g_inpath = dlg_path ? 1 : 0;
            }
            cdm_manager_set_max_active(g_mgr, dlg_maxact);
            persist_settings();
            dlg_maxact = dlg_theme = dlg_skin = -1;
            dlg_tray = dlg_auto = dlg_upd = dlg_path = -1; /* re-seed next open */
            dlg_ticon = dlg_tmin = dlg_dup = -1;
            dlg_prog = dlg_done = dlg_confirm = dlg_snd = -1;
            g_show_opts=0;
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Close")) {
            dlg_maxact = dlg_theme = dlg_skin = -1; /* discard staged edits */
            dlg_tray = dlg_auto = dlg_upd = dlg_path = -1;
            dlg_ticon = dlg_tmin = dlg_dup = -1;
            dlg_prog = dlg_done = dlg_confirm = dlg_snd = -1;
            g_show_opts=0;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_sched_modal(struct nk_context *ctx) {
    struct nk_rect r = nk_rect(340, 200, 420, 320);
    if (nk_begin(ctx, "Scheduler", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, "Scheduled / queued downloads:", NK_TEXT_LEFT);
        char line[256];
        cdm_mutex_lock(g_mgr->mtx);
        for (int i=0;i<g_mgr->count;i++) {
            cdm_job *job = g_mgr->jobs[i];
            if (job->state != JOB_QUEUED) continue;
            const char *bn = strrchr(job->url,'/'); bn = bn?bn+1:job->url;
            const char *qn = (job->queue_idx >= 0 && job->queue_idx < g_mgr->n_queues)
                             ? g_mgr->queues[job->queue_idx].name : "-";
            if (job->sched_epoch>0) {
                struct tm tmv; struct tm *t = cdm_localtime(&job->sched_epoch, &tmv);
                if (t) snprintf(line,sizeof line,"[%s|%.8s] %02d:%02d %.220s",
                                 job->queue_idx>=0?"Q":"S", qn, t->tm_hour, t->tm_min, bn);
                else   snprintf(line,sizeof line,"[Q|%.8s] %.240s", qn, bn);
            } else {
                snprintf(line,sizeof line,"[Q|%.8s] %.240s", qn, bn);
            }
            nk_layout_row_dynamic(ctx, 20, 1); nk_label(ctx, line, NK_TEXT_LEFT);
        }
        cdm_mutex_unlock(g_mgr->mtx);
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_label(ctx, "Start all queued now:", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Start now")) {
            cdm_mutex_lock(g_mgr->mtx);
            for (int i=0;i<g_mgr->count;i++) {
                cdm_job *job=g_mgr->jobs[i];
                if (job->state==JOB_QUEUED) { cdm_mutex_lock(job->mtx);
                    job->sched_epoch=0; cdm_mutex_unlock(job->mtx); }
            }
            cdm_mutex_unlock(g_mgr->mtx);
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Close")) g_show_sched=0;
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "When everything finishes:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26, 1);
        { const char *pa[4]={"Nothing","Shutdown","Sleep","Hibernate"};
          if (g_power_action < 0 || g_power_action > 3) g_power_action = 0;
          g_power_action=nk_combo(ctx,pa,4,g_power_action,18,nk_vec2(160,110)); }
    }
    nk_end(ctx);
}

static const char *job_display_name(cdm_job *j, char *buf, size_t cap) {
    const char *bn;
    if (j->outpath[0]) bn = strrchr(j->outpath, '/');
    else bn = strrchr(j->url, '/');
#ifdef _WIN32
    {
        const char *b2 = strrchr(j->outpath[0] ? j->outpath : j->url, '\\');
        if (b2 && (!bn || b2 > bn)) bn = b2;
    }
#endif
    if (bn) bn++;
    else bn = j->outpath[0] ? j->outpath : j->url;
    snprintf(buf, cap, "%s", bn);
    return buf;
}

static void draw_progress_modal(struct nk_context *ctx, int win_w, int win_h) {
    cdm_job *j = (g_progress_id >= 0) ? cdm_manager_find(g_mgr, g_progress_id) : NULL;
    if (!j || (j->state != JOB_RUNNING && j->state != JOB_PAUSED)) {
        g_show_progress = 0;
        return;
    }
    {
        struct nk_rect r = nk_rect(win_w/2 - 220, win_h/2 - 90, 440, 180);
        char nm[256], dl[32], tot[32], spd[32], eta[16];
        cdm_progress p;
        cdm_mutex_lock(j->mtx);
        p = j->prog;
        job_display_name(j, nm, sizeof(nm));
        cdm_mutex_unlock(j->mtx);
        human_bytes(p.total_bytes > 0 ? (double)p.total_bytes : 0, tot, sizeof(tot));
        human_bytes((double)p.downloaded_bytes, dl, sizeof(dl));
        human_bytes(p.speed_bps, spd, sizeof(spd));
        fmt_eta(p.eta_seconds, eta, sizeof(eta));
        if (nk_begin(ctx, "Downloading", r,
                     NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, nm, NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 24, 1);
            { nk_size cur = 0, mx = 100;
              if (p.total_bytes > 0 && p.downloaded_bytes > 0)
                  cur = (nk_size)(100.0*(double)p.downloaded_bytes/(double)p.total_bytes);
              nk_progress(ctx, &cur, mx, NK_FIXED); }
            { char st[128];
              snprintf(st, sizeof st, "%s / %s  %s/s  ETA %s", dl, tot, spd, eta);
              nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, st, NK_TEXT_LEFT); }
            nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
            nk_layout_row_push(ctx, 0.5f);
            if (nk_button_label(ctx, "Stop")) cdm_manager_stop(g_mgr, j->id);
            nk_layout_row_push(ctx, 0.5f);
            if (nk_button_label(ctx, "Hide")) g_show_progress = 0;
            nk_layout_row_end(ctx);
        }
        nk_end(ctx);
    }
}

static void draw_done_modal(struct nk_context *ctx, int win_w, int win_h) {
    cdm_job *j = (g_done_id >= 0) ? cdm_manager_find(g_mgr, g_done_id) : NULL;
    if (!j || j->state != JOB_DONE) { g_show_done = 0; return; }
    {
        struct nk_rect r = nk_rect(win_w/2 - 220, win_h/2 - 80, 440, 160);
        char nm[256];
        job_display_name(j, nm, sizeof(nm));
        if (nk_begin(ctx, "Download complete", r,
                     NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, nm, NK_TEXT_LEFT);
            nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 3);
            nk_layout_row_push(ctx, 0.34f);
            if (nk_button_label(ctx, "Open folder")) {
                char dir[2048];
                snprintf(dir, sizeof(dir), "%s", j->outpath);
                { char *s = strrchr(dir, '/');
#ifdef _WIN32
                  char *b = strrchr(dir, '\\');
                  if (b && (!s || b > s)) s = b;
#endif
                  if (s) *s = 0; }
#ifdef _WIN32
                ShellExecuteA(NULL, "open", dir[0] ? dir : ".",
                              NULL, NULL, SW_SHOWNORMAL);
#else
                open_url(dir[0] ? dir : ".");
#endif
            }
            nk_layout_row_push(ctx, 0.33f);
            if (nk_button_label(ctx, "Delete entry")) {
                cdm_manager_remove(g_mgr, j->id);
                g_show_done = 0;
            }
            nk_layout_row_push(ctx, 0.33f);
            if (nk_button_label(ctx, "OK")) g_show_done = 0;
            nk_layout_row_end(ctx);
        }
        nk_end(ctx);
    }
}

static void draw_err_modal(struct nk_context *ctx, int win_w, int win_h) {
    cdm_job *j = (g_err_id >= 0) ? cdm_manager_find(g_mgr, g_err_id) : NULL;
    if (!j || j->state != JOB_ERROR) { g_show_err = 0; return; }
    {
        struct nk_rect r = nk_rect(win_w/2 - 220, win_h/2 - 90, 440, 180);
        char nm[256], msg[256];
        job_display_name(j, nm, sizeof(nm));
        cdm_mutex_lock(j->mtx);
        snprintf(msg, sizeof(msg), "%s", j->errmsg);
        cdm_mutex_unlock(j->mtx);
        if (nk_begin(ctx, "Download failed", r,
                     NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
            nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, nm, NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, msg, NK_TEXT_LEFT);
            nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 3);
            nk_layout_row_push(ctx, 0.34f);
            if (nk_button_label(ctx, "Retry")) {
                cdm_manager_resume(g_mgr, j->id);
                g_show_err = 0;
            }
            nk_layout_row_push(ctx, 0.33f);
            if (nk_button_label(ctx, "Delete")) {
                cdm_manager_remove(g_mgr, j->id);
                g_show_err = 0;
            }
            nk_layout_row_push(ctx, 0.33f);
            if (nk_button_label(ctx, "Close")) g_show_err = 0;
            nk_layout_row_end(ctx);
        }
        nk_end(ctx);
    }
}

static void draw_confirm_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 200, win_h/2 - 80, 400, 160);
    if (nk_begin(ctx, "Quit?", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Downloads are still active.", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "Quit anyway?", NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Quit")) { g_quit = 1; g_show_confirm = 0; }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Cancel")) g_show_confirm = 0;
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_power_modal(struct nk_context *ctx, int win_w, int win_h) {
    struct nk_rect r = nk_rect(win_w/2 - 200, win_h/2 - 80, 400, 160);
    long left = (long)(g_power_deadline - time(NULL));
    const char *what = g_power_action == 1 ? "shutdown" :
                       g_power_action == 2 ? "sleep" : "hibernate";
    if (left < 0) left = 0;
    if (nk_begin(ctx, "Power", r,
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        char line[128];
        snprintf(line, sizeof(line), "All done: %s in %ld s", what, left);
        nk_layout_row_dynamic(ctx, 22, 1); nk_label(ctx, line, NK_TEXT_LEFT);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Do it now")) run_power_action();
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Cancel")) {
            g_power_action = 0;
            g_power_deadline = 0;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_about_modal(struct nk_context *ctx) {
    struct nk_rect r = nk_rect(380, 260, 380, 200);
    if (nk_begin(ctx, "About", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "cdm - Download Manager", NK_TEXT_LEFT);
        nk_label(ctx, "IDM-style segmented download manager.", NK_TEXT_LEFT);
        nk_label(ctx, "Engine: libcurl + HTTP Range + resume.", NK_TEXT_LEFT);
        {
            int st = cdm_update_poll(g_update_tag, sizeof(g_update_tag));
            g_update_state = st;
            if (st == 2) {
                char line[128];
                snprintf(line, sizeof line, "Update available: %s", g_update_tag);
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, line, NK_TEXT_LEFT);
            } else if (st == 0) {
                nk_layout_row_dynamic(ctx, 22, 1);
                nk_label(ctx, "Checking for updates...", NK_TEXT_LEFT);
            }
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Get update") && g_update_state == 2) {
            open_url("https://github.com/mickykhd/download-manager/releases/latest");
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "OK")) g_show_about = 0;
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

static void draw_context_menu(struct nk_context *ctx) {
    int id = g_mgr->selected_id;
    cdm_job *j = id>=0 ? cdm_manager_find(g_mgr, id) : NULL;
    if (!j) return;
    if (nk_contextual_begin(ctx, 0, nk_vec2(160, 200), nk_window_get_bounds(ctx))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_contextual_item_label(ctx, "Start / Resume", 0) &&
            (j->state==JOB_PAUSED||j->state==JOB_QUEUED)) cdm_manager_resume(g_mgr, id);
        if (nk_contextual_item_label(ctx, "Stop", 0)) cdm_manager_stop(g_mgr, id);
        if (nk_contextual_item_label(ctx, "Delete", 0)) cdm_manager_remove(g_mgr, id);
        if (nk_contextual_item_label(ctx, "Properties", 0)) { g_show_props=1;
            snprintf(g_props_speed,sizeof g_props_speed,"%lld",(long long)j->cfg.max_speed_bps); }
        if (nk_contextual_item_label(ctx, "Add to Queue", 0)) cdm_manager_add_to_queue(g_mgr, id);
        if (nk_contextual_item_label(ctx, "Delete from Queue", 0)) cdm_manager_remove_from_queue(g_mgr, id);
        if (nk_contextual_item_label(ctx, "Edit...", 0) && !j->running) open_edit(j);
        nk_contextual_end(ctx);
    }
}

/* ---------------- main ---------------- */
#if defined(CDM_UNIT_TEST)
void cdm_gui_test_draw_top(struct nk_context *ctx, int w, int h) {
    draw_menubar(ctx);
    draw_toolbar(ctx);
    draw_body(ctx, (float)h);
    (void)w;
}
#endif

#if !defined(CDM_UNIT_TEST)
int main(int argc, char **argv) {
    if (cdm_already_running()) return 0;
    if (cdm_global_init() != CDM_OK) { fatal_box("engine init failed"); return 1; }
    g_mgr = cdm_manager_create();
    cdm_manager_init_categories(g_mgr, g_base_dir);
    cdm_config_default(&g_cfg);

    /* Restore persisted Options (max concurrent, theme, skin). */
    {
        cdm_settings st;
        cdm_settings_load(g_mgr, &st);
        g_theme = st.theme;
        g_skin = st.skin;
        g_api_enabled = st.api_enabled;
        if (st.api_port > 0) {
            snprintf(g_api_port, sizeof(g_api_port), "%d", st.api_port);
            g_api_port_len = (int)strlen(g_api_port);
        }
        snprintf(g_api_key, sizeof(g_api_key), "%s", st.api_key);
        g_api_key_len = (int)strlen(g_api_key);
        g_net_proxy = g_mgr->proxy_mode;
        snprintf(g_net_proxy_url, sizeof(g_net_proxy_url), "%s", g_mgr->proxy_url);
        g_net_proxy_url_len = (int)strlen(g_net_proxy_url);
        g_tray_close = st.tray_close;
        g_prog_popup = st.prog_popup;
        g_done_popup = st.done_popup;
        g_confirm_exit = st.confirm_exit;
        g_sounds = st.sounds;
        g_autostart = cdm_autostart_get();
        g_update_check = st.update_check;
        g_tray_icon = st.tray_icon;
        g_tray_minimize = st.tray_minimize;
        restart_ipc_server();
        /* restore last session's job list (history + queued) */
        cdm_manager_load_jobs(g_mgr);
        /* don't re-announce restored history */
        for (int i = 0; i < g_mgr->count; i++) {
            cdm_job *jj = g_mgr->jobs[i];
            if (jj->state == JOB_DONE) seen_add(g_done_seen, &g_n_done, CDM_SEEN_CAP, jj->id);
            else if (jj->state == JOB_ERROR) seen_add(g_err_seen, &g_n_err, CDM_SEEN_CAP, jj->id);
        }
        if (g_update_check)
            cdm_update_check_async("mickykhd/download-manager");
    }

    if (argc > 1) {
        const char *url = argv[1];
        const cdm_category *c = cdm_manager_category_for_ext(g_mgr, url);
        const char *bn = strrchr(url, '/'); bn = bn ? bn + 1 : url;
        if (!*bn) bn = "download.bin";
        char outp[2200];
        snprintf(outp, sizeof outp, "%s/%.1174s", c->dir, bn);
        cdm_manager_add_ex(g_mgr, url, outp, &g_cfg, 0, 0);
    }

    if (!glfwInit()) { fatal_box("GLFW init failed"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    int win_w = 1000, win_h = 640;
    int start_hidden = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--minimized") == 0) start_hidden = 1;
    GLFWwindow *win = glfwCreateWindow(win_w, win_h, "cdm - Download Manager", NULL, NULL);
    if (!win) { fatal_box("window create failed"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (glewInit() != GLEW_OK) { fatal_box("GLEW init failed"); return 1; }
    glfwSetWindowCloseCallback(win, on_close);
    glfwSetWindowIconifyCallback(win, on_iconify);
    g_tray_up = (g_tray_icon && cdm_tray_init() == 0);
    if (start_hidden && g_tray_up) glfwHideWindow(win);

    struct nk_glfw nk;
    /* The backend leaves input state (text_len, key_events, scroll)
     * untouched, so a non-zeroed struct feeds garbage input events
     * (and a garbage text length) into the first frames. */
    memset(&nk, 0, sizeof(nk));
    nk_glfw3_init(&nk, win, NK_GLFW3_INSTALL_CALLBACKS);
    struct nk_context *ctx = &nk.ctx;
    struct nk_font_atlas *atlas;
    nk_glfw3_font_stash_begin(&nk, &atlas);
    struct nk_font *font = nk_font_atlas_add_default(atlas, 14, 0);
    nk_glfw3_font_stash_end(&nk);
    nk_style_set_font(ctx, &font->handle);

    while (!g_quit && !glfwWindowShouldClose(win)) {
        glfwPollEvents();
        nk_glfw3_new_frame(&nk);
        apply_theme(ctx);

        int w, h;
        glfwGetWindowSize(win, &w, &h);
        if (nk_begin(ctx, "cdm", nk_rect(0,0,(float)w,(float)h), NK_WINDOW_NO_SCROLLBAR)) {
            draw_menubar(ctx);
            draw_toolbar(ctx);
            /* body: categories (left) + list (right) + status bar */
            draw_body(ctx, (float)h);
        }
        nk_end(ctx);

        draw_context_menu(ctx);
        if (g_show_add)    draw_add_modal(ctx, w, h);
        if (g_show_batch)  draw_batch_modal(ctx, w, h);
        if (g_show_cats)   draw_cats_modal(ctx, w, h);
        if (g_show_queues) draw_queues_modal(ctx, w, h);
        if (g_show_net)    draw_net_modal(ctx, w, h);
        if (g_show_edit)   draw_edit_modal(ctx, w, h);
        if (g_show_props)  draw_props_modal(ctx);
        if (g_show_opts)   draw_opts_modal(ctx);
        if (g_show_sched)  draw_sched_modal(ctx);
        if (g_show_about)  draw_about_modal(ctx);
        if (g_show_progress) draw_progress_modal(ctx, w, h);
        if (g_show_done)   draw_done_modal(ctx, w, h);
        if (g_show_err)    draw_err_modal(ctx, w, h);
        if (g_show_confirm) draw_confirm_modal(ctx, w, h);
        if (g_power_deadline) draw_power_modal(ctx, w, h);

        cdm_manager_pump(g_mgr);
        cdm_manager_reap(g_mgr);

        /* tray requests + tooltip + completion notifications */
        {
            static int tick = 0;
            tick++;
            if (cdm_tray_exit_requested()) {
                if (g_confirm_exit && cdm_manager_any_running(g_mgr)) {
                    g_show_confirm = 1;
                    show_main_window(win);
                } else break;
            }
            if (cdm_tray_show_requested()) show_main_window(win);
            if (cdm_tray_add_url_requested()) {
                g_url[0] = 0; g_url_len = 0;
                g_show_add = 1;
                show_main_window(win);
            }
            if (cdm_tray_settings_requested()) {
                g_show_opts = 1;
                show_main_window(win);
            }
            if (cdm_tray_pause_all_requested()) {
                if (cdm_manager_any_running(g_mgr))
                    cdm_manager_pause_all(g_mgr);
                else
                    cdm_manager_resume_all(g_mgr);
            }
            {
                static char clip[2048];
                if (cdm_tray_clipboard_requested(clip, sizeof(clip))) {
                    /* trim + accept when it looks like a URL */
                    char *s = clip;
                    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
                    {
                        char *e = s + strlen(s);
                        while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                                         e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
                    }
                    if (s[0]) {
                        snprintf(g_url, sizeof(g_url), "%s", s);
                        g_url_len = (int)strlen(g_url);
                        g_show_add = 1;
                        show_main_window(win);
                    }
                }
            }
            /* transitions: exactly-once popups, balloons, sounds */
            {
                int cur_run[1024], n_cur = 0;
                char ev_name[256] = {0}, ev_err[256] = {0};
                int ev_done_id = -1, ev_err_id = -1, ev_run_id = -1;
                cdm_mutex_lock(g_mgr->mtx);
                for (int i = 0; i < g_mgr->count; i++) {
                    cdm_job *jj = g_mgr->jobs[i];
                    cdm_mutex_lock(jj->mtx);
                    if (jj->state == JOB_RUNNING) {
                        if (n_cur < 1024) cur_run[n_cur++] = jj->id;
                        if (!seen_has(g_run_seen, g_n_run, jj->id))
                            ev_run_id = jj->id;
                    } else if (jj->state == JOB_DONE &&
                               !seen_has(g_done_seen, g_n_done, jj->id)) {
                        job_display_name(jj, ev_name, sizeof(ev_name));
                        ev_done_id = jj->id;
                    } else if (jj->state == JOB_ERROR &&
                               !seen_has(g_err_seen, g_n_err, jj->id)) {
                        job_display_name(jj, ev_name, sizeof(ev_name));
                        snprintf(ev_err, sizeof(ev_err), "%s", jj->errmsg);
                        ev_err_id = jj->id;
                    }
                    cdm_mutex_unlock(jj->mtx);
                    /* no early break: the running set below must be complete;
                     * extra events surface on following frames */
                }
                cdm_mutex_unlock(g_mgr->mtx);
                /* rebuild running set (re-runs re-trigger) */
                g_n_run = n_cur < 1024 ? n_cur : 1024;
                for (int i = 0; i < g_n_run; i++) g_run_seen[i] = cur_run[i];
                if (ev_run_id >= 0 && g_prog_popup && !g_show_progress) {
                    g_progress_id = ev_run_id;
                    g_show_progress = 1;
                }
                if (ev_done_id >= 0) {
                    seen_add(g_done_seen, &g_n_done, CDM_SEEN_CAP, ev_done_id);
                    cdm_tray_notify("Download complete", ev_name);
                    play_sound(0);
                    if (g_done_popup && !g_show_done) {
                        g_done_id = ev_done_id;
                        g_show_done = 1;
                    }
                }
                if (ev_err_id >= 0) {
                    seen_add(g_err_seen, &g_n_err, CDM_SEEN_CAP, ev_err_id);
                    {
                        char msg[300];
                        snprintf(msg, sizeof(msg), "%.240s: %.40s", ev_name, ev_err);
                        cdm_tray_notify("Download failed", msg);
                    }
                    play_sound(1);
                    if (!g_show_err) {
                        g_err_id = ev_err_id;
                        g_show_err = 1;
                    }
                }
            }
            if ((tick % 120) == 0) {
                int active = 0;
                double spd = 0;
                char tip[128];
                cdm_mutex_lock(g_mgr->mtx);
                for (int i = 0; i < g_mgr->count; i++) {
                    cdm_job *jj = g_mgr->jobs[i];
                    cdm_mutex_lock(jj->mtx);
                    if (jj->state == JOB_RUNNING) {
                        active++;
                        spd += jj->prog.speed_bps;
                    }
                    cdm_mutex_unlock(jj->mtx);
                }
                cdm_mutex_unlock(g_mgr->mtx);
                if (active > 0) {
                    char hb[32];
                    human_bytes(spd, hb, sizeof(hb));
                    snprintf(tip, sizeof(tip), "cdm - %d active (%s/s)", active, hb);
                } else {
                    snprintf(tip, sizeof(tip), "cdm Download Manager");
                }
                cdm_tray_tooltip(tip);
            }
            /* power action countdown */
            if (g_power_action) {
                if (cdm_manager_idle(g_mgr)) {
                    if (!g_power_deadline)
                        g_power_deadline = time(NULL) + 60;
                    else if (time(NULL) >= g_power_deadline)
                        run_power_action();
                } else {
                    g_power_deadline = 0;
                }
            }
        }

        /* URLs fed by the integration server -> open Add / Batch dialogs */
        {
            static char urls[CDM_MAX_PENDING_URLS][2048];
            int n = cdm_manager_poll_urls(g_mgr, urls, CDM_MAX_PENDING_URLS);
            if (n == 1) {
                snprintf(g_url, sizeof(g_url), "%s", urls[0]);
                g_url_len = (int)strlen(g_url);
                g_show_add = 1;
            } else if (n > 1) {
                size_t off = 0;
                g_batch[0] = 0;
                for (int i = 0; i < n && off + 1 < sizeof(g_batch); i++) {
                    int w = snprintf(g_batch + off, sizeof(g_batch) - off,
                                     "%s%s", i ? "\n" : "", urls[i]);
                    if (w < 0) break;
                    off += (size_t)w;
                }
                g_batch_len = (int)strlen(g_batch);
                g_show_batch = 1;
            }
        }

        glClearColor(g_theme?0.92f:0.13f, g_theme?0.92f:0.14f, g_theme?0.92f:0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_glfw3_render(&nk, NK_ANTI_ALIASING_ON, 512*1024, 128*1024);
        glfwSwapBuffers(win);
    }

    nk_glfw3_shutdown(&nk);
    /* Persist Options so they survive a restart (covers theme/skin picked
     * via combo without pressing Apply). */
    persist_settings();
    cdm_manager_save_jobs(g_mgr);
    cdm_ipc_stop();
    cdm_tray_shutdown();
    cdm_manager_destroy(g_mgr);
    cdm_global_cleanup();
    glfwTerminate();
    return 0;
}
#endif /* !CDM_UNIT_TEST */
