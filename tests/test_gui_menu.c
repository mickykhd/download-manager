/* Headless test for the IDM menubar.
 *
 * Compiles gui.c (with CDM_UNIT_TEST so main() is excluded) and drives the
 * real draw_menubar() with synthetic mouse input, then inspects Nuklear's
 * immediate-mode draw command pool to prove every menu actually lays out and
 * draws its items (the bug was missing nk_layout_row_dynamic -> blank popups).
 *
 * Each menu is exercised on a FRESH context so no previous popup lingers and
 * blocks a new one from opening.
 *
 * Build:
 *   cc tests/test_gui_menu.c src/ui/manager.c \
 *      -I third_party/nuklear -I include -I src/engine -I src/ui \
 *      -L build-cmake -lcdmcore -lglfw -lGLEW -lGL -lpthread -lcurl -lm \
 *      -o build-cmake/test_gui_menu
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

/* Scan the whole frame's immediate command pool for a text command equal to
 * `text`. If found and `out` is non-NULL, the item's drawn rect is returned. */
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

/* Fresh context + themed, ready for a clean menu test. */
static void reset_ctx(void) {
    memset(&g_ctx, 0, sizeof g_ctx);
    nk_init_default(&g_ctx, &g_font->handle);
    nk_style_set_font(&g_ctx, &g_font->handle);
    if (g_fake.mtx) { cdm_mutex_destroy(g_fake.mtx); g_fake.mtx = NULL; }
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.mtx = cdm_mutex_create(); /* draw_body locks the manager */
    g_fake.selected_id = -1; /* no selection -> menu actions are no-ops */
    g_mgr = &g_fake;
    apply_theme(&g_ctx);
}

static void idle_frame(void) {
    nk_clear(&g_ctx);
    nk_input_begin(&g_ctx);
    nk_input_end(&g_ctx);
    if (nk_begin(&g_ctx, "win", nk_rect(0, 0, 1000, 640),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER))
        draw_menubar(&g_ctx);
    nk_end(&g_ctx);
}

/* Click the header `label`, then verify every `items[]` entry is drawn with a
 * non-zero size. The pool is scanned BEFORE nk_end, because Nuklear tears the
 * popup's commands down at end-of-frame once the menu closes. */
static int test_menu(const char *label, const char *items[]) {
    reset_ctx();
    idle_frame(); /* draw menubar so the header exists */

    struct nk_rect r;
    if (!scan_text(label, &r)) {
        printf("  FAIL: menubar header '%s' not drawn\n", label);
        return 0;
    }
    int cx = (int)(r.x + r.w / 2), cy = (int)(r.y + r.h / 2);

    /* press the header -> popup opens and its items are drawn this frame */
    nk_clear(&g_ctx);
    nk_input_begin(&g_ctx);
    nk_input_motion(&g_ctx, cx, cy);
    nk_input_button(&g_ctx, NK_BUTTON_LEFT, cx, cy, 1);
    nk_input_end(&g_ctx);
    if (nk_begin(&g_ctx, "win", nk_rect(0, 0, 1000, 640),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER))
        draw_menubar(&g_ctx);

    /* menu is open: items exist in the command pool right now */
    int ok = 1;
    for (int i = 0; items[i]; i++) {
        struct nk_rect ib;
        if (!scan_text(items[i], &ib)) {
            printf("  FAIL: '%s' menu missing item '%s'\n", label, items[i]);
            ok = 0;
            continue;
        }
        if (ib.w <= 0 || ib.h <= 0) {
            printf("  FAIL: '%s' item '%s' has zero size (not laid out)\n", label, items[i]);
            ok = 0;
        }
    }
    nk_end(&g_ctx);
    return ok;
}

int main(void) {
    nk_font_atlas_init_default(&g_atlas);
    nk_font_atlas_begin(&g_atlas);
    g_font = nk_font_atlas_add_default(&g_atlas, 14, 0);
    int aw = 0, ah = 0;
    nk_font_atlas_bake(&g_atlas, &aw, &ah, NK_FONT_ATLAS_RGBA32);
    nk_font_atlas_end(&g_atlas, nk_handle_id(0), &g_nulltex);

    int pass = 1;
    const char *file_items[] = {"Add URL", NULL};
    const char *dl_items[] = {"Start / Resume", "Stop", "Stop All", "Delete",
                              "Properties", "Add to Queue", "Delete from Queue",
                              "Delete All Completed", NULL};
    const char *opt_items[] = {"Settings...", NULL};
    const char *help_items[] = {"About", NULL};

    printf("Testing menubar dropdowns (real draw_menubar via Nuklear):\n");
    if (!test_menu("File", file_items)) pass = 0;
    if (!test_menu("Downloads", dl_items)) pass = 0;
    if (!test_menu("Options", opt_items)) pass = 0;
    if (!test_menu("Help", help_items)) pass = 0;

    if (pass) {
        printf("PASS: all menubar dropdowns open and list their items.\n");
        return 0;
    }
    printf("FAIL: one or more menus are broken.\n");
    return 1;
}
