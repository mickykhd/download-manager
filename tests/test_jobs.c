/*
 * Job persistence tests: duplicate strategy, save/load round-trip, and
 * the "no stray file-named directories" regression. Network-free
 * (jobs stay queued; the engine never runs).
 */
#include "manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define getpid _getpid
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static char g_tmp[256];

static void isolate_config(void) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (!base || !*base) base = ".";
    snprintf(g_tmp, sizeof(g_tmp), "%s\\cdm-jobs-test-%d", base, (int)getpid());
    MKDIR(g_tmp);
    { char buf[512]; snprintf(buf, sizeof(buf), "XDG_CONFIG_HOME=%s", g_tmp); _putenv(buf); }
#else
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/cdm-jobs-test-%d", (int)getpid());
    MKDIR(g_tmp);
    setenv("XDG_CONFIG_HOME", g_tmp, 1);
#endif
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int dir_exists(const char *p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static cdm_config test_cfg(void) {
    cdm_config c;
    cdm_config_default(&c);
    c.url = "http://127.0.0.1:9/file.bin";
    return c;
}

int main(void) {
    isolate_config();

    /* duplicate strategy + no stray dirs */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_config cfg = test_cfg();
        char p1[512], p2[512];
        snprintf(p1, sizeof(p1), "%s/existing.bin", g_tmp);
        snprintf(p2, sizeof(p2), "%s/plain.bin", g_tmp);
        /* seed an existing file */
        {
            FILE *f = fopen(p1, "wb");
            fputs("x", f);
            fclose(f);
        }
        int id1 = cdm_manager_add_ex(m, "http://127.0.0.1:9/a.bin", p1,
                                     &cfg, 1, 0);
        CHECK(id1 >= 0, "add over existing file accepted");
        {
            cdm_job *j = cdm_manager_find(m, id1);
            CHECK(j && strstr(j->outpath, "(1)") != NULL,
                  "rename mode picks sibling name");
            CHECK(!dir_exists(j->outpath), "no directory named like the file");
        }
        int id2 = cdm_manager_add_ex(m, "http://127.0.0.1:9/b.bin", p2,
                                     &cfg, 1, 0);
        CHECK(id2 >= 0, "add to fresh path accepted");
        {
            cdm_job *j = cdm_manager_find(m, id2);
            CHECK(j && strcmp(j->outpath, p2) == 0, "fresh path kept");
            CHECK(!dir_exists(p2), "no stray dir for fresh path");
        }
        /* overwrite mode keeps the name */
        m->dup_mode = 1;
        int id3 = cdm_manager_add_ex(m, "http://127.0.0.1:9/c.bin", p1,
                                     &cfg, 1, 0);
        {
            cdm_job *j = cdm_manager_find(m, id3);
            CHECK(j && strcmp(j->outpath, p1) == 0, "overwrite keeps name");
        }
        m->dup_mode = 0;
        cdm_manager_destroy(m);
    }

    /* save/load round-trip */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_config cfg = test_cfg();
        int q = cdm_manager_queue_add(m, "Q2");
        char p1[512];
        snprintf(p1, sizeof(p1), "%s/keep.bin", g_tmp);
        int a = cdm_manager_add_to_queue_idx(m, "http://127.0.0.1:9/k.bin",
                                             p1, &cfg, q, 0);
        int b = cdm_manager_add_ex(m, "http://127.0.0.1:9/l.bin", NULL,
                                   &cfg, 1, 0);
        CHECK(a >= 0 && b >= 0, "two jobs queued");
        cdm_manager_pause(m, b); /* QUEUED -> PAUSED */
        CHECK(cdm_manager_save_jobs(m) == 0, "save succeeds");

        cdm_manager *m2 = cdm_manager_create();
        /* queues must exist before jobs referencing them load */
        cdm_manager_queue_add(m2, "Q2");
        int n = cdm_manager_load_jobs(m2);
        CHECK(n == 2, "two jobs restored");
        if (n == 2 && m2->count == 2) {
            int qcount = 0, paused = 0;
            for (int i = 0; i < m2->count; i++) {
                if (m2->jobs[i]->queue_idx == 1) qcount++;
                if (m2->jobs[i]->state == JOB_PAUSED) paused++;
            }
            CHECK(qcount == 1, "queue index restored");
            CHECK(paused == 1, "paused state restored");
            CHECK(m2->jobs[0]->running == 0 && m2->jobs[1]->running == 0,
                  "nothing auto-started on load");
        }
        cdm_manager_destroy(m);
        cdm_manager_destroy(m2);
    }

    /* categories: CRUD + persist + job reassignment on remove */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_manager_init_categories(m, g_tmp);
        cdm_settings s;
        CHECK(m->n_cats == 7, "seven built-in categories");
        int c = cdm_manager_category_add(m, "ISOs");
        CHECK(c == 7 && m->n_cats == 8, "custom category added");
        cdm_manager_category_set(m, c, "Images", NULL);
        CHECK(strcmp(m->cats[c].name, "Images") == 0, "category renamed");
        cdm_config cfg = test_cfg();
        int id = cdm_manager_add_ex(m, "http://127.0.0.1:9/x.iso", NULL,
                                    &cfg, 1, 0);
        {
            cdm_job *j = cdm_manager_find(m, id);
            if (j) { cdm_mutex_lock(j->mtx); j->cat_idx = c; cdm_mutex_unlock(j->mtx); }
        }
        cdm_settings_default(&s);
        CHECK(cdm_settings_save(m, &s) == 0, "save with categories succeeds");
        CHECK(cdm_manager_category_remove(m, 0) != 0, "built-in locked");
        CHECK(cdm_manager_category_remove(m, c) == 0 && m->n_cats == 7,
              "custom category removed");
        {
            cdm_job *j = cdm_manager_find(m, id);
            CHECK(j && j->cat_idx == 0, "job reassigned to General");
        }
        cdm_manager *m2 = cdm_manager_create();
        cdm_settings s2;
        cdm_settings_load(m2, &s2);
        CHECK(m2->n_cats == 8, "categories restored");
        CHECK(strcmp(m2->cats[7].name, "Images") == 0, "custom name restored");
        cdm_manager_destroy(m);
        cdm_manager_destroy(m2);
    }

    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all jobs tests passed\n");
    return 0;
}
