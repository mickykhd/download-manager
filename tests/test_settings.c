/*
 * Round-trip test for persistent Options settings: save to an isolated
 * XDG_CONFIG_HOME, mutate, reload, and verify the values (and the applied
 * max_active) come back. Also covers missing-file defaults, corrupt files,
 * and out-of-range clamping.
 *
 * Build (Makefile target `test` and CMake `test_settings` do this):
 *   cc tests/test_settings.c src/ui/manager.c <engine objects>
 *      -Iinclude -Isrc/engine -Isrc/ui $(pkg-config --cflags libcurl)
 */
#include "manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define MKDIR(d) _mkdir(d)
static void set_cfg(const char *v) {
    if (v) { char buf[2048]; snprintf(buf, sizeof buf, "XDG_CONFIG_HOME=%s", v);
             _putenv(buf); }
    else { _putenv("XDG_CONFIG_HOME="); }
}
static void make_tmp(char *out, size_t cap) {
    const char *base = getenv("TEMP");
    if (!base || !*base) base = getenv("TMP");
    if (!base || !*base) base = ".";
    snprintf(out, cap, "%s\\cdm-settings-test-%d", base, (int)getpid());
    MKDIR(out);
}
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(d) mkdir((d), 0755)
static void set_cfg(const char *v) {
    if (v) setenv("XDG_CONFIG_HOME", v, 1);
    else unsetenv("XDG_CONFIG_HOME");
}
static void make_tmp(char *out, size_t cap) {
    snprintf(out, cap, "/tmp/cdm-settings-test-%d", (int)getpid());
    MKDIR(out);
}
#endif

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

int main(void) {
    char tmp[256];
    make_tmp(tmp, sizeof(tmp));
    set_cfg(tmp);

    /* 1. Missing file -> defaults, and max_active applied to the manager. */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        s.max_active = -99; s.theme = -99; s.skin = -99;
        cdm_settings_load(m, &s);
        CHECK(s.max_active == 3 && s.theme == 0 && s.skin == 0,
              "missing file yields defaults");
        CHECK(m->max_active == 3, "defaults applied to manager");
        cdm_manager_destroy(m);
    }

    /* 2. Save -> file appears; mutate -> load restores saved values. */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        cdm_settings_default(&s);
        s.max_active = 7; s.theme = 1; s.skin = 2;
        CHECK(cdm_settings_save(&s) == 0, "save succeeds");

        char path[300];
        snprintf(path, sizeof(path), "%s/cdm/settings.conf", tmp);
        CHECK(file_exists(path), "settings file created on disk");

        s.max_active = 0; s.theme = 0; s.skin = 0;
        cdm_settings_load(m, &s);
        CHECK(s.max_active == 7 && s.theme == 1 && s.skin == 2,
              "reload restores saved values");
        CHECK(m->max_active == 7, "reload applies max_active to manager");
        cdm_manager_destroy(m);
    }

    /* 3. Out-of-range values are clamped, unknown keys ignored. */
    {
        char path[300];
        snprintf(path, sizeof(path), "%s/cdm/settings.conf", tmp);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "clamp fixture writable");
        if (!f) return 1;
        fputs("CDM-SETTINGS1\nmax_active 99\ntheme -5\nskin 7\n"
              "future_knob 123\n", f);
        fclose(f);
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        cdm_settings_load(m, &s);
        CHECK(s.max_active == 16 && s.theme == 0 && s.skin == 3,
              "out-of-range values clamped, unknown keys ignored");
        cdm_manager_destroy(m);
    }

    /* 4. Corrupt file (bad magic) -> defaults, no crash. */
    {
        char path[300];
        snprintf(path, sizeof(path), "%s/cdm/settings.conf", tmp);
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL, "corrupt fixture writable");
        if (!f) return 1;
        fputs("garbage{{\nmax_active 9\n", f);
        fclose(f);
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        cdm_settings_load(m, &s);
        CHECK(s.max_active == 3 && s.theme == 0 && s.skin == 0,
              "corrupt file falls back to defaults");
        cdm_manager_destroy(m);
    }

    set_cfg(NULL);
    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all settings tests passed\n");
    return 0;
}
