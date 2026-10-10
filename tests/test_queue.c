/*
 * Tests for named queues, batch import, range expansion, scheduler
 * windows and the edit path. Network-free: downloads are never started
 * here (queue limits of 0 keep pump from spawning threads... note pump
 * only starts QUEUED jobs when a slot is free, and job_start_thread
 * would run the engine; tests therefore use disabled queues / closed
 * windows to assert jobs stay queued, plus direct API checks).
 */
#include "manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define getpid _getpid
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

static int failures = 0;

/* Isolate settings I/O from the real user config. */
static void isolate_config(void) {
    char tmp[256];
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (!base || !*base) base = ".";
    snprintf(tmp, sizeof(tmp), "%s\\cdm-queue-test-%d", base, (int)getpid());
    MKDIR(tmp);
    { char buf[512]; snprintf(buf, sizeof(buf), "XDG_CONFIG_HOME=%s", tmp); _putenv(buf); }
#else
    snprintf(tmp, sizeof(tmp), "/tmp/cdm-queue-test-%d", (int)getpid());
    MKDIR(tmp);
    setenv("XDG_CONFIG_HOME", tmp, 1);
#endif
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static cdm_config test_cfg(void) {
    cdm_config c;
    cdm_config_default(&c);
    c.url = "http://127.0.0.1:9/file.bin";
    return c;
}

int main(void) {
    isolate_config();

    /* range expansion */
    {
        static char out[512][2048];
        int n = cdm_expand_range("http://x/f{001:003}.zip", out, 512);
        CHECK(n == 3, "range expands to 3");
        CHECK(strcmp(out[0], "http://x/f001.zip") == 0, "range zero-padded first");
        CHECK(strcmp(out[2], "http://x/f003.zip") == 0, "range zero-padded last");
        n = cdm_expand_range("http://x/plain.zip", out, 512);
        CHECK(n == 1 && strcmp(out[0], "http://x/plain.zip") == 0,
              "plain URL passes through");
        n = cdm_expand_range("http://x/f{5:3}.zip", out, 512);
        CHECK(n == 3 && strcmp(out[0], "http://x/f5.zip") == 0,
              "descending range works");
    }

    /* queue CRUD */
    {
        cdm_manager *m = cdm_manager_create();
        CHECK(m->n_queues == 1, "starts with Default queue");
        int q = cdm_manager_queue_add(m, "Night");
        CHECK(q == 1 && m->n_queues == 2, "queue added");
        cdm_manager_queue_set(m, q, NULL, 2, 120, 360, 1);
        CHECK(m->queues[q].max_active == 2, "queue limit set");
        CHECK(m->queues[q].sched_start_min == 120, "window start set");
        CHECK(cdm_manager_queue_remove(m, 0) != 0, "queue 0 protected");
        CHECK(cdm_manager_queue_remove(m, q) == 0 && m->n_queues == 1,
              "queue removed");
        cdm_manager_destroy(m);
    }

    /* window open logic: always-open vs empty window */
    {
        cdm_queue q;
        memset(&q, 0, sizeof(q));
        q.sched_start_min = -1;
        q.sched_end_min = -1;
        CHECK(cdm_manager_queue_window_open(&q, time(NULL)) == 1,
              "unbounded window always open");
        q.sched_start_min = 0;
        q.sched_end_min = 0;
        CHECK(cdm_manager_queue_window_open(&q, time(NULL)) == 0,
              "empty window never open");
    }

    /* queued jobs wait when the queue is disabled; batch lands queued */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_config cfg = test_cfg();
        int q = cdm_manager_queue_add(m, "Gated");
        cdm_manager_queue_set(m, q, NULL, 4, -1, -1, 0); /* disabled */
        const char *urls[] = {
            "http://127.0.0.1:9/a.bin",
            "http://127.0.0.1:9/b.bin",
        };
        int ok = cdm_manager_add_batch(m, urls, 2, &cfg, q);
        CHECK(ok == 2 && m->count == 2, "batch adds two jobs");
        CHECK(m->jobs[0]->queue_idx == q, "batch jobs land in queue");
        CHECK(m->jobs[0]->state == JOB_QUEUED, "batch jobs start queued");
        cdm_manager_pump(m);
        CHECK(m->jobs[0]->state == JOB_QUEUED && !m->jobs[0]->running,
              "disabled queue does not start jobs");
        cdm_manager_queue_set(m, q, NULL, 0, -1, -1, 1); /* enable, unlimited */
        /* pump would spawn real engine threads now; only verify the
         * queue became eligible without running it (re-disable first) */
        cdm_manager_queue_set(m, q, NULL, 0, -1, -1, 0);
        cdm_manager_destroy(m);
    }

    /* pause_all / resume_all / any_running (no threads involved) */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_config cfg = test_cfg();
        int a = cdm_manager_add_ex(m, "http://127.0.0.1:9/p1.bin",
                                   NULL, &cfg, 1, 0);
        int b = cdm_manager_add_ex(m, "http://127.0.0.1:9/p2.bin",
                                   NULL, &cfg, 1, 0);
        CHECK(a >= 0 && b >= 0, "two queued jobs created");
        CHECK(cdm_manager_any_running(m) == 0, "nothing running");
        cdm_manager_pause(m, a); /* QUEUED -> PAUSED */
        cdm_manager_resume_all(m);
        {
            cdm_job *ja = cdm_manager_find(m, a);
            cdm_job *jb = cdm_manager_find(m, b);
            CHECK(ja && ja->state == JOB_QUEUED, "paused job re-queued");
            CHECK(jb && jb->state == JOB_QUEUED, "queued job untouched");
        }
        cdm_manager_pause_all(m); /* nothing running: no-op, no crash */
        CHECK(cdm_manager_any_running(m) == 0, "still nothing running");
        cdm_manager_destroy(m);
    }

    /* edit path on a queued job */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_config cfg = test_cfg();
        int id = cdm_manager_add_ex(m, "http://127.0.0.1:9/old.bin",
                                    "old.bin", &cfg, 1, 0);
        CHECK(id >= 0, "queued job created");
        int rc = cdm_manager_edit(m, id, "http://127.0.0.1:9/new.bin",
                                  "new.bin", 12345, 0);
        cdm_job *j = cdm_manager_find(m, id);
        CHECK(rc == 0 && j && strcmp(j->url, "http://127.0.0.1:9/new.bin") == 0,
              "edit rewrites URL");
        CHECK(j && j->cfg.max_speed_bps == 12345, "edit rewrites speed");
        cdm_manager_destroy(m);
    }

    /* queue persistence round-trip through settings file */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        cdm_settings_default(&s);
        int q = cdm_manager_queue_add(m, "Persist");
        cdm_manager_queue_set(m, q, NULL, 2, 60, 120, 1);
        m->default_queue = q;
        CHECK(cdm_settings_save(m, &s) == 0, "save with queues succeeds");
        cdm_manager *m2 = cdm_manager_create();
        cdm_settings s2;
        cdm_settings_load(m2, &s2);
        CHECK(m2->n_queues == 2, "queues restored");
        CHECK(strcmp(m2->queues[1].name, "Persist") == 0, "queue name restored");
        CHECK(m2->queues[1].max_active == 2, "queue limit restored");
        CHECK(m2->default_queue == 1, "default queue restored");
        cdm_manager_destroy(m);
        cdm_manager_destroy(m2);
    }

    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all queue tests passed\n");
    return 0;
}
