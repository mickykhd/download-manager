/*
 * Network-policy tests: URL host extraction, header parsing, host rules,
 * proxy settings (+ persistence) and per-job application. Network-free.
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

static void isolate_config(void) {
    char tmp[256];
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (!base || !*base) base = ".";
    snprintf(tmp, sizeof(tmp), "%s\\cdm-net-test-%d", base, (int)getpid());
    MKDIR(tmp);
    { char buf[512]; snprintf(buf, sizeof(buf), "XDG_CONFIG_HOME=%s", tmp); _putenv(buf); }
#else
    snprintf(tmp, sizeof(tmp), "/tmp/cdm-net-test-%d", (int)getpid());
    MKDIR(tmp);
    setenv("XDG_CONFIG_HOME", tmp, 1);
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

    /* host extraction */
    {
        char h[256];
        cdm_host_of_url("https://Example.COM:8080/a/b?x=1", h, sizeof(h));
        CHECK(strcmp(h, "example.com") == 0, "host lowercased, port stripped");
        cdm_host_of_url("http://127.0.0.1:9/f.bin", h, sizeof(h));
        CHECK(strcmp(h, "127.0.0.1") == 0, "IPv4 host kept");
        cdm_host_of_url("not a url at all", h, sizeof(h));
        CHECK(h[0] != 0, "garbage yields *something* (no crash)");
    }

    /* header parsing */
    {
        char blob[2048];
        const char *ptrs[32];
        int n = cdm_parse_headers("X-A: 1\nBadLine\n  Y-B: x y \n\nX-A: 2\n",
                                  blob, sizeof(blob), ptrs, 32);
        CHECK(n == 3, "3 valid headers (bad line skipped)");
        CHECK(strcmp(ptrs[0], "X-A: 1") == 0, "first header intact");
        CHECK(strcmp(ptrs[1], "Y-B: x y") == 0, "header trimmed");
    }

    /* host rules + application */
    {
        cdm_manager *m = cdm_manager_create();
        int i = cdm_manager_host_add(m, "HTTP://Example.com:80/x");
        CHECK(i == 0, "host normalized on add");
        CHECK(cdm_manager_host_add(m, "example.com") == 0 &&
              m->n_hosts == 1, "duplicate host deduped");
        cdm_manager_host_set(m, 0, 4, 1024, "TestAgent/1.0");
        CHECK(m->hosts[0].max_connections == 4, "host maxconn set");
        CHECK(cdm_manager_host_for_url(m, "https://example.com/f") != NULL,
              "host lookup hits");
        CHECK(cdm_manager_host_for_url(m, "https://other.org/f") == NULL,
              "host lookup misses");
        cdm_config cfg = test_cfg();
        cfg.url = "https://example.com/f.bin";
        /* queued: policy applies at add time but no thread spawns */
        int id = cdm_manager_add_ex(m, "https://example.com/f.bin", NULL,
                                    &cfg, 1, 0);
        cdm_job *j = cdm_manager_find(m, id);
        CHECK(j && j->cfg.max_connections == 4, "host maxconn applied");
        CHECK(j && j->cfg.max_speed_bps == 1024, "host speed applied");
        CHECK(j && j->cfg.user_agent &&
              strcmp(j->cfg.user_agent, "TestAgent/1.0") == 0,
              "host UA applied");
        /* edit_net on the (non-running... it started!) job: immediate adds
         * start running, so use a queued one instead */
        cdm_config cfg2 = test_cfg();
        int id2 = cdm_manager_add_ex(m, "https://example.com/g.bin", NULL,
                                     &cfg2, 1, 0);
        int rc = cdm_manager_edit_net(m, id2, "EditAgent/2.0", NULL, NULL,
                                      "X-T: v\n");
        cdm_job *j2 = cdm_manager_find(m, id2);
        CHECK(rc == 0 && j2 && j2->cfg.user_agent &&
              strcmp(j2->cfg.user_agent, "EditAgent/2.0") == 0,
              "edit_net rewrites UA");
        CHECK(j2 && j2->n_hdrs == 1, "edit_net parses headers");
        CHECK(cdm_manager_host_remove(m, 0) == 0 && m->n_hosts == 0,
              "host removed");
        cdm_manager_destroy(m);
    }

    /* proxy + host persistence */
    {
        cdm_manager *m = cdm_manager_create();
        cdm_settings s;
        cdm_settings_default(&s);
        cdm_manager_set_proxy(m, CDM_PROXY_MANUAL, "http://127.0.0.1:8080");
        cdm_manager_host_add(m, "example.com");
        cdm_manager_host_set(m, 0, 2, 512, "PersistAgent");
        CHECK(cdm_settings_save(m, &s) == 0, "save with net policy succeeds");
        cdm_manager *m2 = cdm_manager_create();
        cdm_settings s2;
        cdm_settings_load(m2, &s2);
        CHECK(m2->proxy_mode == CDM_PROXY_MANUAL, "proxy mode restored");
        CHECK(strcmp(m2->proxy_url, "http://127.0.0.1:8080") == 0,
              "proxy URL restored");
        CHECK(m2->n_hosts == 1 &&
              strcmp(m2->hosts[0].host, "example.com") == 0,
              "host rule restored");
        CHECK(m2->hosts[0].max_speed_bps == 512, "host speed restored");
        cdm_manager_destroy(m);
        cdm_manager_destroy(m2);
    }

    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all net tests passed\n");
    return 0;
}
