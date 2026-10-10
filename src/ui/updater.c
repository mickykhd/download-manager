#include "updater.h"
#include "cdm/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#ifndef CDM_VERSION_STR
#define CDM_VERSION_STR "0.0.0"
#endif

static cdm_mutex *g_mu = NULL;
static int g_running = 0;
static int g_state = 0; /* 0 pending/none, 1 current, 2 available, -1 failed */
static char g_tag[64] = {0};
static char g_repo[128] = {0};

static void ensure_mu(void) {
    if (!g_mu) g_mu = cdm_mutex_create();
}

int cdm_vercmp(const char *a, const char *b) {
    /* skip leading v/V, stop at '-' (pre-release) or end */
    if (a && (*a == 'v' || *a == 'V')) a++;
    if (b && (*b == 'v' || *b == 'V')) b++;
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x < y) return -1;
        if (x > y) return 1;
        {
            int asep = (*a == '.');
            int bsep = (*b == '.');
            if (*a == '-') a = "";
            else if (asep) a++;
            else if (*a) return 1; /* a has trailing junk: newer-ish */
            if (*b == '-') b = "";
            else if (bsep) b++;
            else if (*b) return -1;
            if (!*a && !*b) return 0;
            if (!*a) return -1;
            if (!*b) return 1;
        }
    }
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} respbuf;

static size_t write_mem(char *ptr, size_t size, size_t nmemb, void *userp) {
    respbuf *r = (respbuf *)userp;
    size_t n = size * nmemb;
    if (r->len + n + 1 > r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 8192;
        char *nd;
        while (nc < r->len + n + 1) nc *= 2;
        if (nc > 256 * 1024) return 0; /* sanity cap */
        nd = (char *)realloc(r->data, nc);
        if (!nd) return 0;
        r->data = nd;
        r->cap = nc;
    }
    memcpy(r->data + r->len, ptr, n);
    r->len += n;
    r->data[r->len] = 0;
    return n;
}

static void *check_thread(void *arg) {
    char url[256];
    CURL *c;
    respbuf r = {0, 0, 0};
    int ok = 0;
    (void)arg;
    snprintf(url, sizeof(url),
             "https://api.github.com/repos/%s/releases/latest", g_repo);
    /* NOTE: no curl_global_init here (not thread-safe); the app must have
     * called cdm_global_init() on the main thread first. */
    c = curl_easy_init();
    if (c) {
        struct curl_slist *hs = NULL;
        hs = curl_slist_append(hs, "Accept: application/vnd.github+json");
        hs = curl_slist_append(hs, "User-Agent: cdm-updater");
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_mem);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        if (curl_easy_perform(c) == CURLE_OK && r.data) {
            const char *p = strstr(r.data, "\"tag_name\"");
            if (p) {
                p = strchr(p, ':');
                if (p) {
                    p++;
                    while (*p == ' ' || *p == '"') p++;
                    {
                        size_t k = 0;
                        char tag[64];
                        while (*p && *p != '"' && *p != ',' && k + 1 < sizeof(tag))
                            tag[k++] = *p++;
                        tag[k] = 0;
                        if (tag[0]) {
                            cdm_mutex_lock(g_mu);
                            snprintf(g_tag, sizeof(g_tag), "%s", tag);
                            g_state = (cdm_vercmp(tag, CDM_VERSION_STR) > 0) ? 2 : 1;
                            cdm_mutex_unlock(g_mu);
                            ok = 1;
                        }
                    }
                }
            }
        }
        curl_slist_free_all(hs);
        curl_easy_cleanup(c);
    }
    free(r.data);
    if (!ok) {
        cdm_mutex_lock(g_mu);
        if (g_state == 0) g_state = -1;
        cdm_mutex_unlock(g_mu);
    }
    cdm_mutex_lock(g_mu);
    g_running = 0;
    cdm_mutex_unlock(g_mu);
    return NULL;
}

void cdm_update_check_async(const char *repo) {
    ensure_mu();
    if (!g_mu || !repo || !*repo) return;
    cdm_mutex_lock(g_mu);
    if (g_running) {
        cdm_mutex_unlock(g_mu);
        return;
    }
    g_running = 1;
    g_state = 0;
    g_tag[0] = 0;
    snprintf(g_repo, sizeof(g_repo), "%s", repo);
    cdm_mutex_unlock(g_mu);
    if (cdm_thread_spawn_detached(check_thread, NULL) != 0) {
        cdm_mutex_lock(g_mu);
        g_running = 0;
        g_state = -1;
        cdm_mutex_unlock(g_mu);
    }
}

int cdm_update_poll(char *out_tag, unsigned cap) {
    int st;
    ensure_mu();
    if (!g_mu) return 0;
    cdm_mutex_lock(g_mu);
    st = g_state;
    if (out_tag && cap > 0)
        snprintf(out_tag, cap, "%s", g_tag);
    cdm_mutex_unlock(g_mu);
    return st;
}
