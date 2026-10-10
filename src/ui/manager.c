#include "manager.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <strings.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

/* Portable case-insensitive compare for file extensions (MSVC has no
 * strcasecmp; MinGW may lack it depending on headers). */
static int cdm_extcasecmp(const char *a, const char *b) {
#ifdef _WIN32
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (int)(unsigned char)tolower((unsigned char)*a) -
           (int)(unsigned char)tolower((unsigned char)*b);
#else
    return strcasecmp(a, b);
#endif
}

static void job_free(cdm_job *j);
const cdm_category *cdm_manager_category_for_ext(cdm_manager *m,
                                                 const char *name_or_url);

static void ensure_parent_dir(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = 0;
            MKDIR(tmp);
            *p = sep;
        }
    }
    MKDIR(tmp);
}

static void job_progress_cb(const cdm_progress *p, void *user) {
    cdm_job *j = (cdm_job *)user;
    cdm_mutex_lock(j->mtx);
    j->prog = *p;
    cdm_mutex_unlock(j->mtx);
}

static void *job_runner(void *arg) {
    cdm_job *j = (cdm_job *)arg;

    cdm_status st = cdm_download_run(j->dl);

    cdm_mutex_lock(j->mtx);
    j->result = st;
    j->running = 0;
    if (st == CDM_OK) {
        j->state = JOB_DONE;
    } else if (st == CDM_ERR_CANCELED) {
        if (j->requeue_req)      j->state = JOB_QUEUED;
        else if (j->cancel_req)  j->state = JOB_CANCELED;
        else                     j->state = JOB_PAUSED;
    } else {
        snprintf(j->errmsg, sizeof(j->errmsg), "%s", cdm_status_str(st));
        j->state = JOB_ERROR;
    }
    cdm_mutex_unlock(j->mtx);
    return NULL;
}

static void job_start_thread(cdm_job *j) {
    j->running = 1;
    j->state = JOB_RUNNING;
    j->pause_req = 0;
    j->cancel_req = 0;
    j->requeue_req = 0;
    if (cdm_thread_spawn_detached(job_runner, j) != 0) {
        cdm_mutex_lock(j->mtx);
        j->running = 0;
        j->state = JOB_ERROR;
        snprintf(j->errmsg, sizeof(j->errmsg), "thread spawn failed");
        cdm_mutex_unlock(j->mtx);
    }
}

static int count_running(cdm_manager *m) {
    int n = 0;
    for (int i = 0; i < m->count; i++)
        if (m->jobs[i]->running) n++;
    return n;
}

cdm_manager *cdm_manager_create(void) {
    cdm_manager *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->capacity = 16;
    m->jobs = calloc((size_t)m->capacity, sizeof(cdm_job *));
    if (!m->jobs) { free(m); return NULL; }
    m->max_active = 3;
    m->mtx = cdm_mutex_create();
    if (!m->mtx) { free(m->jobs); free(m); return NULL; }
    /* Queue 0 ("Default") always exists; per-queue limit -1 = global. */
    snprintf(m->queues[0].name, sizeof(m->queues[0].name), "Default");
    m->queues[0].max_active = -1;
    m->queues[0].sched_start_min = -1;
    m->queues[0].sched_end_min = -1;
    m->queues[0].enabled = 1;
    m->n_queues = 1;
    m->default_queue = 0;
    m->proxy_mode = CDM_PROXY_SYSTEM;
    m->proxy_url[0] = 0;
    m->n_hosts = 0;
    return m;
}

void cdm_host_of_url(const char *url, char *host, size_t cap) {
    const char *p, *end;
    size_t n, i;
    if (!host || cap == 0) return;
    host[0] = 0;
    if (!url) return;
    p = strstr(url, "://");
    p = p ? p + 3 : url;
    if (*p == '[') { /* IPv6 literal [::1]:port */
        p++;
        end = strchr(p, ']');
    } else {
        end = strpbrk(p, "/?#:");
    }
    if (!end) end = p + strlen(p);
    n = (size_t)(end - p);
    if (n >= cap) n = cap - 1;
    for (i = 0; i < n; i++) {
        char c = p[i];
        host[i] = (char)tolower((unsigned char)c);
    }
    host[n] = 0;
}

const cdm_host_rule *cdm_manager_host_for_url(cdm_manager *m, const char *url) {
    char host[256];
    int i;
    if (!m || !url) return NULL;
    cdm_host_of_url(url, host, sizeof(host));
    if (!host[0]) return NULL;
    for (i = 0; i < m->n_hosts; i++) {
        if (strcmp(m->hosts[i].host, host) == 0)
            return &m->hosts[i];
    }
    return NULL;
}

int cdm_parse_headers(const char *text, char *blob, size_t bcap,
                      const char **ptrs, int pcap) {
    int n = 0;
    size_t used = 0;
    const char *p;
    if (!text || !blob || bcap == 0 || !ptrs || pcap <= 0) return 0;
    blob[0] = 0;
    n = 0;
    used = 0;
    p = text;
    while (*p && n < pcap) {
        char line[512];
        size_t li = 0;
        while (*p && *p != '\r' && *p != '\n' && li + 1 < sizeof(line))
            line[li++] = *p++;
        while (*p == '\r' || *p == '\n') p++;
        line[li] = 0;
        /* trim */
        {
            size_t a = 0, b = li;
            while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
            while (b > a && (line[b-1] == ' ' || line[b-1] == '\t')) b--;
            if (b <= a) continue;
            if (!memchr(line + a, ':', b - a)) continue;
            if (used + (b - a) + 1 > bcap) break;
            memcpy(blob + used, line + a, b - a);
            blob[used + (b - a)] = 0;
            ptrs[n++] = blob + used;
            used += b - a + 1;
        }
    }
    return n;
}

/* Is queue q's daily time window open at `now`? Bounds are minutes since
 * midnight, -1 = unbounded. Overnight windows (start > end) wrap. */
int cdm_manager_queue_window_open(const cdm_queue *q, time_t now) {
    int s, e, cur;
    struct tm tmv;
    struct tm *t;
    if (!q) return 0;
    s = q->sched_start_min;
    e = q->sched_end_min;
    if (s < 0 && e < 0) return 1;
    if (s < 0) s = 0;
    if (e < 0) e = 24 * 60;
#ifdef _WIN32
    if (localtime_s(&tmv, &now) != 0) return 1;
    t = &tmv;
#else
    t = localtime_r(&now, &tmv);
    if (!t) return 1;
#endif
    cur = t->tm_hour * 60 + t->tm_min;
    if (s <= e) return cur >= s && cur < e;
    return cur >= s || cur < e; /* overnight wrap */
}

void cdm_manager_init_categories(cdm_manager *m, const char *base_dir) {
    const char *names[7] = {"General", "Video", "Music", "Programs",
                            "Documents", "Compressed", "Others"};
    m->n_cats = 7;
    for (int i = 0; i < 7; i++) {
        snprintf(m->cats[i].name, sizeof m->cats[i].name, "%s", names[i]);
        snprintf(m->cats[i].dir, sizeof m->cats[i].dir, "%s/%s",
                 base_dir ? base_dir : "downloads", names[i]);
        ensure_parent_dir(m->cats[i].dir);
    }
}

/* ---- persistent settings ---- */

#define CDM_SETTINGS_MAGIC "CDM-SETTINGS1"
#define CDM_SETTINGS_MAX_ACTIVE 16

/* Resolve the settings file path. Returns 1 when a usable location was
 * found, 0 when there is nowhere to persist (load keeps defaults, save
 * becomes a no-op). */
static int settings_path(char *out, size_t cap) {
    const char *base = NULL;
    const char *suffix = NULL;
    /* XDG_CONFIG_HOME is honored on all platforms so tests can isolate
     * the config dir (and users can override it). */
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        suffix = "cdm/settings.conf";
    }
#ifdef _WIN32
    if (!suffix) {
        base = getenv("APPDATA");
        if (base && *base) {
            suffix = "cdm/settings.conf";
        } else {
            base = getenv("USERPROFILE");
            if (base && *base) suffix = "AppData/Roaming/cdm/settings.conf";
        }
    }
#else
    if (!suffix) {
        base = getenv("HOME");
        if (base && *base) suffix = ".config/cdm/settings.conf";
    }
#endif
    if (!base || !*base || !suffix) return 0;
    /* base capped so base + '/' + suffix + NUL always fits cap (>= 64). */
    snprintf(out, cap, "%.2000s/%s", base, suffix);
    return 1;
}

void cdm_settings_default(cdm_settings *s) {
    if (!s) return;
    s->max_active = 3;
    s->theme = 0;
    s->skin = 0;
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void cdm_settings_load(cdm_manager *m, cdm_settings *s) {
    cdm_settings_default(s);
    char path[2048];
    if (!settings_path(path, sizeof(path))) return;
    FILE *f = fopen(path, "rb");
    if (!f) return; /* first run: no file yet */

    char line[256];
    if (!fgets(line, sizeof(line), f) ||
        strncmp(line, CDM_SETTINGS_MAGIC, strlen(CDM_SETTINGS_MAGIC)) != 0) {
        fclose(f);
        return; /* not ours: keep defaults */
    }
    if (m) {
        /* restore user queues (queue 0 Default is always present) */
        m->n_queues = 1;
        m->default_queue = 0;
        m->proxy_mode = CDM_PROXY_SYSTEM;
        m->proxy_url[0] = 0;
        m->n_hosts = 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val;
        if (sscanf(line, "%63s %d", key, &val) != 2) {
            /* string-carrying lines: queue/hostrule/proxy_url */
            if (m && strncmp(line, "queue ", 6) == 0) {
                char name[64];
                int mx, st, en, on;
                if (sscanf(line + 6, "%63s %d %d %d %d",
                            name, &mx, &st, &en, &on) == 5 &&
                    m->n_queues < CDM_MAX_QUEUES) {
                    cdm_queue *q = &m->queues[m->n_queues++];
                    snprintf(q->name, sizeof(q->name), "%s", name);
                    q->max_active = (mx >= -1 && mx <= CDM_SETTINGS_MAX_ACTIVE) ? mx : -1;
                    q->sched_start_min = (st >= -1 && st < 24 * 60) ? st : -1;
                    q->sched_end_min = (en >= -1 && en < 24 * 60) ? en : -1;
                    q->enabled = on ? 1 : 0;
                }
            } else if (m && strncmp(line, "hostrule ", 9) == 0) {
                char host[256], ua[256] = {0};
                int mc;
                long long sp;
                /* hostrule <host> <maxconn> <speed> [ua...] */
                if (sscanf(line + 9, "%255s %d %lld %255[^\n]",
                            host, &mc, &sp, ua) >= 3 &&
                    m->n_hosts < CDM_MAX_HOSTS) {
                    cdm_host_rule *r = &m->hosts[m->n_hosts++];
                    snprintf(r->host, sizeof(r->host), "%s", host);
                    r->max_connections = (mc >= 0 && mc <= 64) ? mc : 0;
                    r->max_speed_bps = sp >= 0 ? (int64_t)sp : 0;
                    snprintf(r->user_agent, sizeof(r->user_agent), "%s", ua);
                }
            } else if (m && strncmp(line, "proxy_url ", 10) == 0) {
                char *v = line + 10;
                while (*v == ' ' || *v == '\t') v++;
                {
                    size_t n = strlen(v);
                    while (n > 0 && (v[n-1] == '\r' || v[n-1] == '\n' ||
                                     v[n-1] == ' ' || v[n-1] == '\t')) n--;
                    if (n >= sizeof(m->proxy_url)) n = sizeof(m->proxy_url) - 1;
                    memcpy(m->proxy_url, v, n);
                    m->proxy_url[n] = 0;
                }
            }
            continue;
        }
        if (strcmp(key, "max_active") == 0)
            s->max_active = clamp_int(val, 0, CDM_SETTINGS_MAX_ACTIVE);
        else if (strcmp(key, "theme") == 0)
            s->theme = clamp_int(val, 0, 1);
        else if (strcmp(key, "skin") == 0)
            s->skin = clamp_int(val, 0, 3);
        else if (strcmp(key, "default_queue") == 0) {
            if (m) m->default_queue = val;
        }
        else if (strcmp(key, "proxy_mode") == 0) {
            if (m && val >= CDM_PROXY_DIRECT && val <= CDM_PROXY_MANUAL)
                m->proxy_mode = val;
        }
        /* unknown keys ignored for forward compatibility */
    }
    fclose(f);

    if (m) {
        if (m->default_queue < 0 || m->default_queue >= m->n_queues)
            m->default_queue = 0;
        cdm_manager_set_max_active(m, s->max_active);
    }
}

int cdm_settings_save(const cdm_manager *m, const cdm_settings *s) {
    if (!s) return -1;
    char path[2048];
    if (!settings_path(path, sizeof(path))) return -1;

    /* Create the config dir (ensure_parent_dir mkdirs every prefix, so
     * strip the basename first to avoid mkdir-ing the file itself). */
    char dir[2048];
    snprintf(dir, sizeof(dir), "%.2047s", path);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        *slash = 0;
        ensure_parent_dir(dir);
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n", CDM_SETTINGS_MAGIC);
    fprintf(f, "max_active %d\n", clamp_int(s->max_active, 0, CDM_SETTINGS_MAX_ACTIVE));
    fprintf(f, "theme %d\n", clamp_int(s->theme, 0, 1));
    fprintf(f, "skin %d\n", clamp_int(s->skin, 0, 3));
    if (m) {
        fprintf(f, "default_queue %d\n", m->default_queue);
        for (int i = 1; i < m->n_queues; i++) {
            const cdm_queue *q = &m->queues[i];
            fprintf(f, "queue %s %d %d %d %d\n", q->name, q->max_active,
                    q->sched_start_min, q->sched_end_min, q->enabled);
        }
        fprintf(f, "proxy_mode %d\n", m->proxy_mode);
        if (m->proxy_url[0])
            fprintf(f, "proxy_url %s\n", m->proxy_url);
        for (int i = 0; i < m->n_hosts; i++) {
            const cdm_host_rule *r = &m->hosts[i];
            fprintf(f, "hostrule %s %d %lld %s\n", r->host,
                    r->max_connections, (long long)r->max_speed_bps,
                    r->user_agent);
        }
    }
    fclose(f);
    return 0;
}

const cdm_category *cdm_manager_category_for_ext(cdm_manager *m,
                                                 const char *name_or_url) {
    const char *dot = strrchr(name_or_url, '.');
    const char *slash = strrchr(name_or_url, '/');
    const char *base = slash ? slash + 1 : name_or_url;
    const char *ext = dot && dot > base ? dot + 1 : "";

    int idx = 0; /* General */
    if      (cdm_extcasecmp(ext,"mp4")==0||cdm_extcasecmp(ext,"mkv")==0||
             cdm_extcasecmp(ext,"avi")==0||cdm_extcasecmp(ext,"mov")==0||
             cdm_extcasecmp(ext,"webm")==0||cdm_extcasecmp(ext,"flv")==0) idx = 1;
    else if (cdm_extcasecmp(ext,"mp3")==0||cdm_extcasecmp(ext,"wav")==0||
             cdm_extcasecmp(ext,"flac")==0||cdm_extcasecmp(ext,"ogg")==0||
             cdm_extcasecmp(ext,"m4a")==0) idx = 2;
    else if (cdm_extcasecmp(ext,"exe")==0||cdm_extcasecmp(ext,"msi")==0||
             cdm_extcasecmp(ext,"deb")==0||cdm_extcasecmp(ext,"dmg")==0||
             cdm_extcasecmp(ext,"app")==0||cdm_extcasecmp(ext,"apk")==0) idx = 3;
    else if (cdm_extcasecmp(ext,"pdf")==0||cdm_extcasecmp(ext,"doc")==0||
             cdm_extcasecmp(ext,"docx")==0||cdm_extcasecmp(ext,"txt")==0||
             cdm_extcasecmp(ext,"xls")==0||cdm_extcasecmp(ext,"ppt")==0) idx = 4;
    else if (cdm_extcasecmp(ext,"zip")==0||cdm_extcasecmp(ext,"rar")==0||
             cdm_extcasecmp(ext,"7z")==0||cdm_extcasecmp(ext,"tar")==0||
             cdm_extcasecmp(ext,"gz")==0||cdm_extcasecmp(ext,"bz2")==0) idx = 5;
    return &m->cats[idx];
}

/* Copy cfg's network strings into job-owned storage and repoint cfg.
 * Must be called before cdm_download_create. */
static void job_apply_net(cdm_job *j, const cdm_config *cfg) {
    if (cfg->user_agent && *cfg->user_agent)
        snprintf(j->net_ua, sizeof(j->net_ua), "%s", cfg->user_agent);
    else
        j->net_ua[0] = 0;
    if (cfg->referer && *cfg->referer)
        snprintf(j->net_referer, sizeof(j->net_referer), "%s", cfg->referer);
    else
        j->net_referer[0] = 0;
    if (cfg->cookie && *cfg->cookie)
        snprintf(j->net_cookie, sizeof(j->net_cookie), "%s", cfg->cookie);
    else
        j->net_cookie[0] = 0;
    if (cfg->headers_text && *cfg->headers_text) {
        j->n_hdrs = cdm_parse_headers(cfg->headers_text, j->hdr_blob,
                                      sizeof(j->hdr_blob), j->hdr_ptrs,
                                      CDM_MAX_JOB_HEADERS);
    } else {
        j->n_hdrs = 0;
    }
    j->cfg.user_agent = j->net_ua[0] ? j->net_ua : NULL;
    j->cfg.referer = j->net_referer[0] ? j->net_referer : NULL;
    j->cfg.cookie = j->net_cookie[0] ? j->net_cookie : NULL;
    if (j->n_hdrs > 0) {
        j->cfg.extra_headers = j->hdr_ptrs;
        j->cfg.n_extra_headers = j->n_hdrs;
    } else {
        j->cfg.extra_headers = NULL;
        j->cfg.n_extra_headers = 0;
    }
    j->cfg.headers_text = NULL; /* materialized; never pass transient text on */
}

/* Manager connection policy: untouched-default downloads inherit the
 * manager proxy; host rules override connections/speed/UA when set. */
static void job_apply_policy(cdm_manager *m, cdm_job *j) {
    const cdm_host_rule *r;
    if (j->cfg.proxy_mode == CDM_PROXY_SYSTEM && !j->cfg.proxy_url) {
        j->cfg.proxy_mode = m->proxy_mode;
        j->cfg.proxy_url = m->proxy_url[0] ? m->proxy_url : NULL;
    }
    r = cdm_manager_host_for_url(m, j->url);
    if (r) {
        if (r->max_connections > 0) {
            j->cfg.max_connections = r->max_connections;
            if (j->cfg.start_connections > j->cfg.max_connections)
                j->cfg.start_connections = j->cfg.max_connections;
        }
        if (r->max_speed_bps > 0)
            j->cfg.max_speed_bps = r->max_speed_bps;
        if (r->user_agent[0]) {
            snprintf(j->net_ua, sizeof(j->net_ua), "%s", r->user_agent);
            j->cfg.user_agent = j->net_ua;
        }
    }
}

static int add_internal(cdm_manager *m, const char *url, const char *outpath,
                         const cdm_config *cfg, int queue_idx,
                         time_t sched_epoch) {
    if (!url || !*url) return -1;
    if (queue_idx >= m->n_queues) queue_idx = m->n_queues - 1;

    cdm_job *j = calloc(1, sizeof(*j));
    if (!j) return -1;
    j->mtx = cdm_mutex_create();
    if (!j->mtx) { free(j); return -1; }
    j->queue_idx = queue_idx;

    j->id = m->next_id++;
    snprintf(j->url, sizeof(j->url), "%s", url);
    if (outpath && *outpath) {
        snprintf(j->outpath, sizeof(j->outpath), "%s", outpath);
        ensure_parent_dir(j->outpath);
    }

    j->cfg = *cfg;
    j->cfg.url = j->url;
    j->cfg.output_path = j->outpath[0] ? j->outpath : NULL;
    j->cfg.quiet = 1;
    job_apply_net(j, cfg);
    job_apply_policy(m, j);

    j->sched_epoch = sched_epoch;

    j->dl = cdm_download_create(&j->cfg);
    if (!j->dl) {
        snprintf(j->errmsg, sizeof j->errmsg, "create failed");
        j->state = JOB_ERROR;
        cdm_mutex_destroy(j->mtx);
        free(j);
        return -1;
    }
    cdm_download_set_progress_cb(j->dl, job_progress_cb, j);

    /* decide whether to start now */
    int running = count_running(m);
    int can_start = (m->max_active <= 0) || (running < m->max_active);
    int sched_ok = (sched_epoch == 0) || ((time_t)time(NULL) >= sched_epoch);

    cdm_mutex_lock(m->mtx);
    if (m->count >= m->capacity) {
        m->capacity *= 2;
        cdm_job **nj = realloc(m->jobs, (size_t)m->capacity * sizeof(cdm_job *));
        if (!nj) { cdm_mutex_unlock(m->mtx); job_free(j); return -1; }
        m->jobs = nj;
    }
    /* default category follows the URL extension; callers may override */
    j->cat_idx = (int)(cdm_manager_category_for_ext(m, url) - m->cats);
    m->jobs[m->count++] = j;
    m->selected_id = j->id;
    cdm_mutex_unlock(m->mtx);

    if (queue_idx < 0 && can_start && sched_ok) {
        job_start_thread(j);
    } else {
        cdm_mutex_lock(j->mtx);
        j->state = JOB_QUEUED;
        if (queue_idx < 0) j->queue_idx = m->default_queue; /* deferred */
        cdm_mutex_unlock(j->mtx);
    }
    return j->id;
}

int cdm_manager_add_ex(cdm_manager *m, const char *url, const char *outpath,
                       const cdm_config *cfg, int queued, time_t sched_epoch) {
    return add_internal(m, url, outpath, cfg,
                        queued ? m->default_queue : -1, sched_epoch);
}

int cdm_manager_add_to_queue_idx(cdm_manager *m, const char *url,
                                 const char *outpath, const cdm_config *cfg,
                                 int queue_idx, time_t sched_epoch) {
    if (queue_idx < 0 || queue_idx >= m->n_queues)
        queue_idx = m->default_queue;
    return add_internal(m, url, outpath, cfg, queue_idx, sched_epoch);
}

int cdm_manager_add(cdm_manager *m, const char *url, const char *outpath,
                    const cdm_config *cfg) {
    return add_internal(m, url, outpath, cfg, -1, 0);
}

int cdm_manager_add_batch(cdm_manager *m, const char **urls, int n,
                          const cdm_config *cfg, int queue_idx) {
    int ok = 0;
    if (!urls || n <= 0) return 0;
    if (queue_idx < -1 || queue_idx >= m->n_queues)
        queue_idx = m->default_queue;
    for (int i = 0; i < n; i++) {
        if (!urls[i] || !*urls[i]) continue;
        if (add_internal(m, urls[i], NULL, cfg, queue_idx, 0) >= 0) ok++;
    }
    return ok;
}

/* Expand one {start:end} range (e.g. file{001:100}.zip) into out[].
 * Returns the number of entries (1 + plain copy when no range). */
int cdm_expand_range(const char *pattern, char out[][2048], int cap) {
    const char *open, *close, *colon;
    long start, end;
    int width, neg, count, i;
    char fmt[64], prefix[2048], suffix[2048];
    if (!pattern || cap <= 0) return 0;
    if (cap > 4096) cap = 4096;
    open = strchr(pattern, '{');
    close = open ? strchr(open, '}') : NULL;
    colon = open && close ? strchr(open, ':') : NULL;
    if (!open || !close || !colon || colon > close) {
        snprintf(out[0], 2048, "%s", pattern);
        return 1;
    }
    /* bounds may be zero-padded; width = digits of the wider bound */
    {
        char sbuf[32], ebuf[32];
        size_t slen = (size_t)(colon - open - 1);
        size_t elen = (size_t)(close - colon - 1);
        if (slen == 0 || elen == 0 || slen >= sizeof(sbuf) || elen >= sizeof(ebuf)) {
            snprintf(out[0], 2048, "%s", pattern);
            return 1;
        }
        memcpy(sbuf, open + 1, slen); sbuf[slen] = 0;
        memcpy(ebuf, colon + 1, elen); ebuf[elen] = 0;
        start = strtol(sbuf, NULL, 10);
        end = strtol(ebuf, NULL, 10);
        width = (int)(slen > elen ? slen : elen);
    }
    neg = (end < start);
    count = (int)(neg ? (start - end + 1) : (end - start + 1));
    if (count > cap) count = cap;
    if ((size_t)(open - pattern) >= sizeof(prefix)) return 0;
    memcpy(prefix, pattern, (size_t)(open - pattern));
    prefix[open - pattern] = 0;
    snprintf(suffix, sizeof(suffix), "%s", close + 1);
    snprintf(fmt, sizeof(fmt), "%%s%%0%dld%%s", width);
    for (i = 0; i < count; i++) {
        long v = neg ? (start - i) : (start + i);
        snprintf(out[i], 2048, fmt, prefix, v, suffix);
    }
    return count;
}

int cdm_manager_edit(cdm_manager *m, int id, const char *url,
                     const char *outpath, int64_t max_speed_bps,
                     int queue_idx) {
    int rc = -1;
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (!j->running && (j->state == JOB_QUEUED || j->state == JOB_PAUSED ||
                            j->state == JOB_ERROR || j->state == JOB_CANCELED)) {
            if (url && *url) {
                snprintf(j->url, sizeof(j->url), "%s", url);
                j->cfg.url = j->url;
            }
            if (outpath) {
                snprintf(j->outpath, sizeof(j->outpath), "%s", outpath);
                j->cfg.output_path = j->outpath[0] ? j->outpath : NULL;
                if (j->outpath[0]) ensure_parent_dir(j->outpath);
            }
            if (max_speed_bps >= 0) {
                j->cfg.max_speed_bps = max_speed_bps;
            }
            if (queue_idx >= -1 && queue_idx < m->n_queues) {
                j->queue_idx = queue_idx;
                if (queue_idx >= 0) j->state = JOB_QUEUED;
            }
            /* The engine snapshots cfg at create time: rebuild it so a
             * later run uses the edited URL/path/speed. */
            {
                cdm_download *ndl = cdm_download_create(&j->cfg);
                if (!ndl) { cdm_mutex_unlock(j->mtx); cdm_mutex_unlock(m->mtx); return -1; }
                cdm_download_set_progress_cb(ndl, job_progress_cb, j);
                cdm_download_destroy(j->dl);
                j->dl = ndl;
            }
            rc = 0;
        }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
    return rc;
}

int cdm_manager_edit_net(cdm_manager *m, int id, const char *user_agent,
                         const char *referer, const char *cookie,
                         const char *headers_text) {
    int rc = -1;
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (!j->running && (j->state == JOB_QUEUED || j->state == JOB_PAUSED ||
                            j->state == JOB_ERROR || j->state == JOB_CANCELED)) {
            cdm_config tmp = j->cfg;
            if (user_agent) tmp.user_agent = *user_agent ? user_agent : NULL;
            if (referer) tmp.referer = *referer ? referer : NULL;
            if (cookie) tmp.cookie = *cookie ? cookie : NULL;
            if (headers_text) tmp.headers_text = headers_text;
            job_apply_net(j, &tmp);
            /* NOTE: no job_apply_policy here — an explicit user edit wins
             * over host policy (policy applies at add time). */
            {
                cdm_download *ndl = cdm_download_create(&j->cfg);
                if (!ndl) { cdm_mutex_unlock(j->mtx); cdm_mutex_unlock(m->mtx); return -1; }
                cdm_download_set_progress_cb(ndl, job_progress_cb, j);
                cdm_download_destroy(j->dl);
                j->dl = ndl;
            }
            rc = 0;
        }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
    return rc;
}

void cdm_manager_set_proxy(cdm_manager *m, int mode, const char *url) {
    cdm_mutex_lock(m->mtx);
    if (mode >= CDM_PROXY_DIRECT && mode <= CDM_PROXY_MANUAL)
        m->proxy_mode = mode;
    if (url)
        snprintf(m->proxy_url, sizeof(m->proxy_url), "%s", url);
    cdm_mutex_unlock(m->mtx);
}

int cdm_manager_host_add(cdm_manager *m, const char *host) {
    int idx = -1;
    char norm[256];
    cdm_mutex_lock(m->mtx);
    cdm_host_of_url(host ? host : "", norm, sizeof(norm));
    if (norm[0] && m->n_hosts < CDM_MAX_HOSTS) {
        /* dedupe */
        int i;
        for (i = 0; i < m->n_hosts; i++)
            if (strcmp(m->hosts[i].host, norm) == 0) break;
        if (i < m->n_hosts) {
            idx = i;
        } else {
            idx = m->n_hosts++;
            snprintf(m->hosts[idx].host, sizeof(m->hosts[idx].host), "%s", norm);
            m->hosts[idx].max_connections = 0;
            m->hosts[idx].max_speed_bps = 0;
            m->hosts[idx].user_agent[0] = 0;
        }
    }
    cdm_mutex_unlock(m->mtx);
    return idx;
}

int cdm_manager_host_remove(cdm_manager *m, int idx) {
    int rc = -1;
    cdm_mutex_lock(m->mtx);
    if (idx >= 0 && idx < m->n_hosts) {
        memmove(&m->hosts[idx], &m->hosts[idx + 1],
                (size_t)(m->n_hosts - idx - 1) * sizeof(cdm_host_rule));
        m->n_hosts--;
        rc = 0;
    }
    cdm_mutex_unlock(m->mtx);
    return rc;
}

void cdm_manager_host_set(cdm_manager *m, int idx, int max_connections,
                          int64_t max_speed_bps, const char *user_agent) {
    cdm_mutex_lock(m->mtx);
    if (idx >= 0 && idx < m->n_hosts) {
        cdm_host_rule *r = &m->hosts[idx];
        if (max_connections >= 0 && max_connections <= 64)
            r->max_connections = max_connections;
        if (max_speed_bps >= 0)
            r->max_speed_bps = max_speed_bps;
        if (user_agent)
            snprintf(r->user_agent, sizeof(r->user_agent), "%s", user_agent);
    }
    cdm_mutex_unlock(m->mtx);
}

cdm_job *cdm_manager_find(cdm_manager *m, int id) {
    for (int i = 0; i < m->count; i++)
        if (m->jobs[i]->id == id) return m->jobs[i];
    return NULL;
}

void cdm_manager_pause(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_QUEUED) { j->state = JOB_PAUSED; j->queue_idx = -1; }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_resume(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if ((j->state == JOB_PAUSED || j->state == JOB_QUEUED) && !j->running) {
            j->cfg.resume = 1;
            j->queue_idx = -1;
            job_start_thread(j);
        }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_cancel(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_PAUSED || j->state == JOB_QUEUED)
            j->state = JOB_CANCELED;
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_remove(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        else { j->state = JOB_CANCELED; }
        j->remove_req = 1;
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_stop(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_QUEUED) { j->state = JOB_PAUSED; j->queue_idx = -1; }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_stop_all(cdm_manager *m) {
    cdm_mutex_lock(m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        cdm_mutex_lock(j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_delete_all_completed(cdm_manager *m) {
    cdm_mutex_lock(m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        cdm_mutex_lock(j->mtx);
        if (j->state == JOB_DONE) j->remove_req = 1;
        else if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_add_to_queue(cdm_manager *m, int id) {
    cdm_manager_move_to_queue(m, id, m->default_queue);
}

void cdm_manager_move_to_queue(cdm_manager *m, int id, int queue_idx) {
    cdm_mutex_lock(m->mtx);
    if (queue_idx < 0 || queue_idx >= m->n_queues)
        queue_idx = m->default_queue;
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        if (j->running) {
            j->requeue_req = 1;
            j->queue_idx = queue_idx;
            cdm_download_cancel(j->dl);
        } else if (j->state == JOB_PAUSED || j->state == JOB_CANCELED ||
                   j->state == JOB_ERROR) {
            j->queue_idx = queue_idx;
            j->state = JOB_QUEUED;
        }
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_remove_from_queue(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        cdm_mutex_lock(j->mtx);
        j->queue_idx = -1;
        if (j->state == JOB_QUEUED) j->state = JOB_PAUSED;
        cdm_mutex_unlock(j->mtx);
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_set_max_active(cdm_manager *m, int n) {
    cdm_mutex_lock(m->mtx);
    m->max_active = n < 0 ? 0 : n;
    cdm_mutex_unlock(m->mtx);
}

static int count_running_in(cdm_manager *m, int queue_idx) {
    int n = 0;
    for (int i = 0; i < m->count; i++)
        if (m->jobs[i]->running && m->jobs[i]->queue_idx == queue_idx) n++;
    return n;
}

void cdm_manager_pump(cdm_manager *m) {
    cdm_mutex_lock(m->mtx);
    time_t now = (time_t)time(NULL);
    for (int qi = 0; qi < m->n_queues; qi++) {
        const cdm_queue *q = &m->queues[qi];
        int limit, slots;
        if (!q->enabled) continue;
        if (!cdm_manager_queue_window_open(q, now)) continue;
        limit = q->max_active < 0 ? m->max_active : q->max_active;
        slots = (limit <= 0) ? 1000000 : (limit - count_running_in(m, qi));
        for (int i = 0; i < m->count && slots > 0; i++) {
            cdm_job *j = m->jobs[i];
            cdm_mutex_lock(j->mtx);
            int start = 0;
            if (j->state == JOB_QUEUED && !j->running && j->queue_idx == qi) {
                int sched_ok = (j->sched_epoch == 0) || (now >= j->sched_epoch);
                if (sched_ok) start = 1;
            }
            cdm_mutex_unlock(j->mtx);
            if (start) {
                job_start_thread(j);   /* sets RUNNING + running=1 */
                slots--;
            }
        }
    }
    cdm_mutex_unlock(m->mtx);
}

int cdm_manager_queue_add(cdm_manager *m, const char *name) {
    int idx = -1;
    cdm_mutex_lock(m->mtx);
    if (m->n_queues < CDM_MAX_QUEUES) {
        idx = m->n_queues++;
        snprintf(m->queues[idx].name, sizeof(m->queues[idx].name),
                 "%s", (name && *name) ? name : "Queue");
        m->queues[idx].max_active = -1;
        m->queues[idx].sched_start_min = -1;
        m->queues[idx].sched_end_min = -1;
        m->queues[idx].enabled = 1;
    }
    cdm_mutex_unlock(m->mtx);
    return idx;
}

int cdm_manager_queue_remove(cdm_manager *m, int idx) {
    int rc = -1;
    cdm_mutex_lock(m->mtx);
    if (idx > 0 && idx < m->n_queues) { /* queue 0 cannot be removed */
        for (int i = 0; i < m->count; i++) {
            cdm_job *j = m->jobs[i];
            cdm_mutex_lock(j->mtx);
            if (j->queue_idx == idx) {
                j->queue_idx = 0;
            } else if (j->queue_idx > idx) {
                j->queue_idx--;
            }
            cdm_mutex_unlock(j->mtx);
        }
        memmove(&m->queues[idx], &m->queues[idx + 1],
                (size_t)(m->n_queues - idx - 1) * sizeof(cdm_queue));
        m->n_queues--;
        if (m->default_queue >= m->n_queues)
            m->default_queue = 0;
        rc = 0;
    }
    cdm_mutex_unlock(m->mtx);
    return rc;
}

void cdm_manager_queue_set(cdm_manager *m, int idx, const char *name,
                           int max_active, int start_min, int end_min,
                           int enabled) {
    cdm_mutex_lock(m->mtx);
    if (idx >= 0 && idx < m->n_queues) {
        cdm_queue *q = &m->queues[idx];
        if (name && *name)
            snprintf(q->name, sizeof(q->name), "%s", name);
        if (max_active >= -1 && max_active <= CDM_SETTINGS_MAX_ACTIVE)
            q->max_active = max_active;
        q->sched_start_min = (start_min >= -1 && start_min < 24 * 60) ? start_min : -1;
        q->sched_end_min = (end_min >= -1 && end_min < 24 * 60) ? end_min : -1;
        q->enabled = enabled ? 1 : 0;
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_reap(cdm_manager *m) {
    cdm_mutex_lock(m->mtx);
    for (int i = m->count - 1; i >= 0; i--) {
        cdm_job *j = m->jobs[i];
        int reap = 0;
        cdm_mutex_lock(j->mtx);
        if (j->remove_req && !j->running) reap = 1;
        cdm_mutex_unlock(j->mtx);
        if (reap) {
            if (m->selected_id == j->id) m->selected_id = -1;
            memmove(&m->jobs[i], &m->jobs[i + 1],
                    (size_t)(m->count - i - 1) * sizeof(cdm_job *));
            m->count--;
            job_free(j);
        }
    }
    cdm_mutex_unlock(m->mtx);
}

void cdm_manager_select(cdm_manager *m, int id) {
    cdm_mutex_lock(m->mtx);
    m->selected_id = id;
    cdm_mutex_unlock(m->mtx);
}

static void job_free(cdm_job *j) {
    if (!j) return;
    if (j->dl) cdm_download_destroy(j->dl);
    if (j->mtx) cdm_mutex_destroy(j->mtx);
    free(j);
}

void cdm_manager_destroy(cdm_manager *m) {
    if (!m) return;
    cdm_mutex_lock(m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        if (j->running) cdm_download_cancel(j->dl);
        job_free(j);
    }
    free(m->jobs);
    cdm_mutex_unlock(m->mtx);
    cdm_mutex_destroy(m->mtx);
    free(m);
}
