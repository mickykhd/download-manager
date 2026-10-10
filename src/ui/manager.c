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
    return m;
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
    }
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val;
        if (sscanf(line, "%63s %d", key, &val) != 2) {
            /* queue lines carry strings: queue <name> <max> <s> <e> <en> */
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
