/* Headless test: verify every toolbar button (esp. "Scheduler") gets a
 * non-zero width. Nuklear's nk_layout_row_push silently drops a DYNAMIC push
 * once the cumulative ratio exceeds 1.0, which previously collapsed the last
 * button ("Scheduler") to zero width -> hidden.
 *
 * Build:
 *   cc tests/test_gui_toolbar.c src/ui/manager.c \
 *      -I third_party/nuklear -I include -I src/engine -I src/ui \
 *      -L build-cmake -lcdmcore -lglfw -lGLEW -lGL -lpthread -lcurl -lm \
 *      -o build-cmake/test_gui_toolbar && ./build-cmake/test_gui_toolbar
 */
#define CDM_UNIT_TEST
#include "../src/ui/gui.c"

#include <stdio.h>
#include <string.h>

static struct nk_context g_ctx;
static cdm_manager g_fake;
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
    if (g_fake.mtx) { cdm_mutex_destroy(g_fake.mtx); g_fake.mtx = NULL; }
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.mtx = cdm_mutex_create(); /* draw_body locks the manager */
    g_fake.selected_id = -1; /* no selection -> button actions are no-ops */
    g_mgr = &g_fake;
    apply_theme(&g_ctx);
}

int main(void) {
    nk_font_atlas_init_default(&g_atlas);
    nk_font_atlas_begin(&g_atlas);
    g_font = nk_font_atlas_add_default(&g_atlas, 14, 0);
    int aw = 0, ah = 0;
    nk_font_atlas_bake(&g_atlas, &aw, &ah, NK_FONT_ATLAS_RGBA32);
    nk_font_atlas_end(&g_atlas, nk_handle_id(0), &g_nulltex);

    reset_ctx();
    nk_clear(&g_ctx);
    nk_input_begin(&g_ctx);
    nk_input_end(&g_ctx);
    if (nk_begin(&g_ctx, "win", nk_rect(0, 0, 1000, 640),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER))
        cdm_gui_test_draw_top(&g_ctx, 1000, 640);
    /* scan BEFORE nk_end (text commands are torn down at end-of-frame) */

    const char *btns[] = {"Add URL","Start/Resume","Stop","Stop All",
                           "Delete","Delete Compl.","Options","Scheduler",
                           "Add Batch","Queues","Edit", NULL};
    int fail = 0;
    printf("Toolbar button widths:\n");
    for (int i = 0; btns[i]; i++) {
        struct nk_rect ib;
        int found = scan_text(btns[i], &ib);
        printf("  %-14s found=%d width=%d\n", btns[i], found, found ? (int)ib.w : 0);
        if (!found || ib.w <= 0) {
            printf("  FAIL: '%s' hidden (width=%d)\n", btns[i], found ? (int)ib.w : 0);
            fail = 1;
        }
    }
    nk_end(&g_ctx);

    if (fail) { printf("FAIL: toolbar has hidden buttons.\n"); return 1; }
    printf("PASS: all toolbar buttons visible (incl. Scheduler).\n");
    return 0;
}
