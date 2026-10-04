/* Headless test: verify the download list (headers + job rows) actually
 * renders. Previously draw_list()/draw_categories() opened a nested layout
 * row inside the body row's pushed columns, which collapsed the outer
 * columns and left the whole list area blank on screen.
 *
 * Build:
 *   cc tests/test_gui_list.c src/ui/manager.c \
 *      -I third_party/nuklear -I include -I src/engine -I src/ui \
 *      -L build-cmake -lcdmcore -lglfw -lGLEW -lGL -lpthread -lcurl -lm \
 *      -o build-cmake/test_gui_list && ./build-cmake/test_gui_list
  */
#define CDM_UNIT_TEST
#include "../src/ui/gui.c"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static struct nk_context g_ctx;
static cdm_manager g_fake;
static cdm_job g_job;
static cdm_job *g_jobs[4];
static struct nk_font_atlas g_atlas;
static struct nk_font *g_font;
static struct nk_draw_null_texture g_nulltex;

static int scan_text(const char *text, struct nk_rect *out) {
    struct nk_buffer *b = &g_ctx.memory;
    if (!b->memory.ptr) return 0;
    char *base = (char *)b->memory.ptr;
    nk_size total = b->allocated;
    struct nk_command *cmd = (struct nk_command *)base;
    while (cmd && (char *)cmd < base + total) {
        if (cmd->type == NK_COMMAND_TEXT) {
            struct nk_command_text *t = (struct nk_command_text *)cmd;
            if (strcmp(t->string, text) == 0) {
                if (out) { out->x = t->x; out->y = t->y; out->w = t->w; out->h = t->h; }
                return 1;
            }
        }
        if (cmd->next == 0 || cmd->next >= total) break;
        cmd = (struct nk_command *)(base + cmd->next);
    }
    return 0;
}

static void reset_ctx(void) {
    memset(&g_ctx, 0, sizeof g_ctx);
    nk_init_default(&g_ctx, &g_font->handle);
    nk_style_set_font(&g_ctx, &g_font->handle);
    memset(&g_fake, 0, sizeof g_fake);
    memset(&g_job, 0, sizeof g_job);
    pthread_mutex_init(&g_fake.mtx, NULL);
    pthread_mutex_init(&g_job.mtx, NULL);
    g_fake.selected_id = -1;
    g_fake.n_cats = 1;
    strcpy(g_fake.cats[0].name, "General");
    g_mgr = &g_fake;
    g_cat_filter = -1;
    apply_theme(&g_ctx);
}

static void draw_frame(void) {
    nk_clear(&g_ctx);
    nk_input_begin(&g_ctx);
    nk_input_end(&g_ctx);
    if (nk_begin(&g_ctx, "win", nk_rect(0, 0, 1000, 640),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER))
        cdm_gui_test_draw_top(&g_ctx, 1000, 640);
    /* scan BEFORE nk_end (commands are torn down at end-of-frame) */
}

int main(void) {
    nk_font_atlas_init_default(&g_atlas);
    nk_font_atlas_begin(&g_atlas);
    g_font = nk_font_atlas_add_default(&g_atlas, 14, 0);
    int aw = 0, ah = 0;
    nk_font_atlas_bake(&g_atlas, &aw, &ah, NK_FONT_ATLAS_RGBA32);
    nk_font_atlas_end(&g_atlas, nk_handle_id(0), &g_nulltex);

    int fail = 0;

    /* Case 1: empty manager -> column headers must still be drawn. */
    reset_ctx();
    draw_frame();
    printf("Empty list headers:\n");
    const char *hdrs[] = {"File", "Size", "Status", "Progress", "Speed",
                          "Time", "Downloaded", "Xfer", "Q", NULL};
    for (int i = 0; hdrs[i]; i++) {
        struct nk_rect r;
        int found = scan_text(hdrs[i], &r);
        printf("  %-10s found=%d width=%d\n", hdrs[i], found, found ? (int)r.w : 0);
        if (!found || r.w <= 0) {
            printf("  FAIL: header '%s' not drawn\n", hdrs[i]);
            fail = 1;
        }
    }
    nk_end(&g_ctx);

    /* Case 2: one running job -> its row must be drawn with nonzero size. */
    reset_ctx();
    /* NOTE: keep the name short; Nuklear clips overlong item text in the
     * command stream, so exact-match scans need a name under that cap. */
    snprintf(g_job.outpath, sizeof g_job.outpath, "demo.iso");
    snprintf(g_job.url, sizeof g_job.url, "http://127.0.0.1:8773/demo.iso");
    g_job.id = 1;
    g_job.state = JOB_RUNNING;
    g_job.cat_idx = 0;
    g_job.prog.total_bytes = 1000;
    g_job.prog.downloaded_bytes = 400;
    g_job.prog.speed_bps = 12345;
    g_jobs[0] = &g_job;
    g_fake.jobs = g_jobs;
    g_fake.count = 1;
    draw_frame();
    printf("Job row:\n");
    {
        struct nk_rect r;
        int found = scan_text("demo.iso", &r);
        printf("  job name found=%d width=%d\n", found, found ? (int)r.w : 0);
        if (!found || r.w <= 0) {
            printf("  FAIL: job row not drawn (list area blank?)\n");
            fail = 1;
        }
        found = scan_text("Downloading", &r);
        printf("  status   found=%d width=%d\n", found, found ? (int)r.w : 0);
        if (!found || r.w <= 0) {
            printf("  FAIL: job status not drawn\n");
            fail = 1;
        }
    }
    nk_end(&g_ctx);

    pthread_mutex_destroy(&g_fake.mtx);
    pthread_mutex_destroy(&g_job.mtx);

    /* Case 3: selectable contrast per theme (regression: light theme had
     * black text on Nuklear's default dark selectable background). */
    printf("Selectable contrast:\n");
    g_theme = 1;
    reset_ctx(); /* re-applies the light theme */
    {
        struct nk_color bg = g_ctx.style.selectable.normal.data.color;
        struct nk_color fg = g_ctx.style.selectable.text_normal;
        int bright_bg = bg.r >= 0xf0 && bg.g >= 0xf0 && bg.b >= 0xf0;
        int dark_fg = fg.r <= 0x40 && fg.g <= 0x40 && fg.b <= 0x40;
        printf("  light: bg=(%d,%d,%d) fg=(%d,%d,%d)\n",
               bg.r, bg.g, bg.b, fg.r, fg.g, fg.b);
        if (!bright_bg || !dark_fg) {
            printf("  FAIL: light selectables lack dark-on-light contrast\n");
            fail = 1;
        }
    }
    g_theme = 0;
    reset_ctx(); /* re-applies the dark theme */
    {
        struct nk_color bg = g_ctx.style.selectable.normal.data.color;
        struct nk_color fg = g_ctx.style.selectable.text_normal;
        int dark_bg = bg.r <= 0x40 && bg.g <= 0x40 && bg.b <= 0x40;
        int bright_fg = fg.r >= 0xc0 && fg.g >= 0xc0 && fg.b >= 0xc0;
        printf("  dark:  bg=(%d,%d,%d) fg=(%d,%d,%d)\n",
               bg.r, bg.g, bg.b, fg.r, fg.g, fg.b);
        if (!dark_bg || !bright_fg) {
            printf("  FAIL: dark selectables lack light-on-dark contrast\n");
            fail = 1;
        }
    }

    if (fail) { printf("FAIL: download list has rendering gaps.\n"); return 1; }
    printf("PASS: download list headers and job rows are drawn.\n");
    return 0;
}
