#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* localtime_r under strict -std=c11 */
#endif
#include "manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

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
#endif

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

static int g_show_props = 0;
static char g_props_speed[32] = {0}; static int g_props_speed_len = 0;

static int g_show_edit = 0;
static int g_edit_id = -1;
static char g_edit_url[2048] = {0}; static int g_edit_url_len = 0;
static char g_edit_out[2048] = {0}; static int g_edit_out_len = 0;
static char g_edit_speed[32] = {0}; static int g_edit_speed_len = 0;
static int  g_edit_queue = -1;

static int g_show_batch = 0;
static char g_batch[8192] = {0}; static int g_batch_len = 0;
static int  g_batch_queue = 0;

static int g_show_queues = 0;
static char g_newq_name[64] = {0}; static int g_newq_name_len = 0;
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

/* theme */
static int g_theme = 0;   /* 0 dark, 1 light */
static int g_skin = 0;    /* 0 blue,1 teal,2 red,3 purple */

static int g_cat_filter = -1; /* -1 = all */

static void open_edit(cdm_job *j); /* defined after draw_toolbar */

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

    nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 3);
    nk_layout_row_push(ctx, 0.33f);
    if (nk_button_label(ctx, "Add Batch")) g_show_batch = 1;
    nk_layout_row_push(ctx, 0.33f);
    if (nk_button_label(ctx, "Queues")) g_show_queues = 1;
    nk_layout_row_push(ctx, 0.34f);
    if (nk_button_label(ctx, "Edit") && j && !j->running) open_edit(j);
    nk_layout_row_end(ctx);
}

static void open_edit(cdm_job *j) {
    static int edit_id = -1;
    if (!j) return;
    cdm_mutex_lock(j->mtx);
    snprintf(g_edit_url, sizeof(g_edit_url), "%s", j->url);
    snprintf(g_edit_out, sizeof(g_edit_out), "%s", j->outpath);
    snprintf(g_edit_speed, sizeof(g_edit_speed), "%lld",
             (long long)j->cfg.max_speed_bps);
    g_edit_queue = j->queue_idx;
    edit_id = j->id;
    cdm_mutex_unlock(j->mtx);
    g_edit_url_len = (int)strlen(g_edit_url);
    g_edit_out_len = (int)strlen(g_edit_out);
    g_edit_speed_len = (int)strlen(g_edit_speed);
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
static void draw_list(struct nk_context *ctx, int *active, double *total_speed) {
    *active = 0;
    *total_speed = 0;
    char s_size[32], s_spd[32], s_eta[16], s_dl[32];

    if (nk_group_begin(ctx, "jobs", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        /* headers */
        nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 9);
        float w[9] = {0.24f,0.09f,0.10f,0.18f,0.08f,0.08f,0.09f,0.06f,0.04f};
        const char *hdr[9] = {"File","Size","Status","Progress","Speed",
                              "Time","Downloaded","Xfer","Q"};
        for (int c = 0; c < 9; c++) { nk_layout_row_push(ctx, w[c]);
            nk_label(ctx, hdr[c], NK_TEXT_LEFT); }
        nk_layout_row_end(ctx);

        cdm_mutex_lock(g_mgr->mtx);
        for (int i = 0; i < g_mgr->count; i++) {
            cdm_job *job = g_mgr->jobs[i];
            if (g_cat_filter >= 0 && job->cat_idx != g_cat_filter) continue;

            cdm_progress p;
            cdm_mutex_lock(job->mtx);
            p = job->prog;
            int st = job->state;
            int qidx = job->queue_idx;
            int sched = (job->sched_epoch > 0);
            cdm_mutex_unlock(job->mtx);

            if (st == JOB_RUNNING) { (*active)++; *total_speed += p.speed_bps; }

            const char *name = job->outpath[0] ? job->outpath
                              : (strrchr(job->url, '/') ? strrchr(job->url, '/') + 1
                                                        : job->url);
            human_bytes(p.total_bytes > 0 ? (double)p.total_bytes : 0, s_size, sizeof s_size);
            human_bytes(p.speed_bps, s_spd, sizeof s_spd);
            human_bytes(p.downloaded_bytes, s_dl, sizeof s_dl);
            fmt_eta(p.eta_seconds, s_eta, sizeof s_eta);
            const char *stxt = (st == JOB_QUEUED && sched) ? "Scheduled"
                             : state_name(st);

            nk_layout_row_begin(ctx, NK_DYNAMIC, 24, 9);
            nk_layout_row_push(ctx, w[0]);
            { int sel = (job->id == g_mgr->selected_id);
              if (nk_selectable_label(ctx, name, NK_TEXT_LEFT, &sel))
                  cdm_manager_select(g_mgr, sel ? job->id : -1); }
            nk_layout_row_push(ctx, w[1]); nk_label(ctx, s_size, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, w[2]); nk_label(ctx, stxt, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, w[3]);
            { nk_size cur = 0, mx = 100;
              if (p.total_bytes > 0 && p.downloaded_bytes > 0)
                  cur = (nk_size)(100.0*(double)p.downloaded_bytes/(double)p.total_bytes);
              nk_progress(ctx, &cur, mx, NK_FIXED); }
            nk_layout_row_push(ctx, w[4]); nk_label(ctx, st==JOB_RUNNING?s_spd:"-", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, w[5]); nk_label(ctx, st==JOB_RUNNING?s_eta:"-", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, w[6]); nk_label(ctx, s_dl, NK_TEXT_LEFT);
            nk_layout_row_push(ctx, w[7]);
            { char x[8]; snprintf(x,sizeof x,"%d",p.active_connections); nk_label(ctx,x,NK_TEXT_LEFT); }
            nk_layout_row_push(ctx, w[8]);
            { char q[8];
              if (qidx >= 0) snprintf(q, sizeof q, "Q%d", qidx);
              else snprintf(q, sizeof q, "%s", sched ? "S" : "-");
              nk_label(ctx, q, NK_TEXT_LEFT); }
            nk_layout_row_end(ctx);
        }
        cdm_mutex_unlock(g_mgr->mtx);
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

    char status[160];
    snprintf(status, sizeof status,
             "Active: %d   Total speed: %.1f %s/s   Max concurrent: %d   Theme: %s",
             active, total_speed >= 1024 ? total_speed/1024 : total_speed,
             total_speed >= 1024 ? "KB" : "B", g_mgr->max_active,
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

        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Download")) submit_add();
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Cancel")) {
            g_url[0]=0; g_url_len=0; g_out[0]=0; g_out_len=0;
            g_speed[0]=0; g_speed_len=0; g_sched[0]=0; g_sched_len=0;
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
    cdm_settings_save(g_mgr, &st);
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
        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply")) {
            cdm_manager_edit(g_mgr, g_edit_id,
                             g_edit_url_len ? g_edit_url : NULL,
                             g_edit_out_len ? g_edit_out : NULL,
                             parse_speed(g_edit_speed), g_edit_queue);
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
    struct nk_rect r = nk_rect(360, 240, 360, 220);
    if (nk_begin(ctx, "Options", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        /* Staged copies: combos/property edit these, globals change only on
         * Apply (Close discards). Seeded from live values on each opening. */
        static int dlg_maxact = -1, dlg_theme = -1, dlg_skin = -1;
        if (dlg_maxact < 0) {
            dlg_maxact = g_mgr->max_active;
            dlg_theme = g_theme;
            dlg_skin = g_skin;
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

        nk_layout_row_begin(ctx, NK_DYNAMIC, 28, 2);
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Apply")) {
            g_theme = dlg_theme;
            g_skin = dlg_skin;
            cdm_manager_set_max_active(g_mgr, dlg_maxact);
            /* Persist immediately so the setting survives a restart. */
            {
                cdm_settings st;
                st.max_active = dlg_maxact;
                st.theme = dlg_theme;
                st.skin = dlg_skin;
                cdm_settings_save(g_mgr, &st);
            }
            dlg_maxact = dlg_theme = dlg_skin = -1; /* re-seed next open */
            g_show_opts=0;
        }
        nk_layout_row_push(ctx, 0.5f);
        if (nk_button_label(ctx, "Close")) {
            dlg_maxact = dlg_theme = dlg_skin = -1; /* discard staged edits */
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
    }
    nk_end(ctx);
}

static void draw_about_modal(struct nk_context *ctx) {
    struct nk_rect r = nk_rect(380, 280, 360, 150);
    if (nk_begin(ctx, "About", r, NK_WINDOW_TITLE|NK_WINDOW_BORDER|NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "cdm - Download Manager", NK_TEXT_LEFT);
        nk_label(ctx, "IDM-style segmented download manager.", NK_TEXT_LEFT);
        nk_label(ctx, "Engine: libcurl + HTTP Range + resume.", NK_TEXT_LEFT);
        if (nk_button_label(ctx, "OK")) g_show_about = 0;
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
    if (cdm_global_init() != CDM_OK) { fprintf(stderr, "cdm: engine init failed\n"); return 1; }
    g_mgr = cdm_manager_create();
    cdm_manager_init_categories(g_mgr, g_base_dir);
    cdm_config_default(&g_cfg);

    /* Restore persisted Options (max concurrent, theme, skin). */
    {
        cdm_settings st;
        cdm_settings_load(g_mgr, &st);
        g_theme = st.theme;
        g_skin = st.skin;
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

    if (!glfwInit()) { fprintf(stderr, "cdm: GLFW init failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    int win_w = 1000, win_h = 640;
    GLFWwindow *win = glfwCreateWindow(win_w, win_h, "cdm - Download Manager", NULL, NULL);
    if (!win) { fprintf(stderr, "cdm: window create failed\n"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (glewInit() != GLEW_OK) { fprintf(stderr, "cdm: GLEW init failed\n"); return 1; }

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
        if (g_show_queues) draw_queues_modal(ctx, w, h);
        if (g_show_edit)   draw_edit_modal(ctx, w, h);
        if (g_show_props)  draw_props_modal(ctx);
        if (g_show_opts)   draw_opts_modal(ctx);
        if (g_show_sched)  draw_sched_modal(ctx);
        if (g_show_about)  draw_about_modal(ctx);

        cdm_manager_pump(g_mgr);
        cdm_manager_reap(g_mgr);

        glClearColor(g_theme?0.92f:0.13f, g_theme?0.92f:0.14f, g_theme?0.92f:0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        nk_glfw3_render(&nk, NK_ANTI_ALIASING_ON, 512*1024, 128*1024);
        glfwSwapBuffers(win);
    }

    nk_glfw3_shutdown(&nk);
    /* Persist Options so they survive a restart (covers theme/skin picked
     * via combo without pressing Apply). */
    persist_settings();
    cdm_manager_destroy(g_mgr);
    cdm_global_cleanup();
    glfwTerminate();
    return 0;
}
#endif /* !CDM_UNIT_TEST */
