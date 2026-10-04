#include "manager.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

static void job_free(cdm_job *j);

static void ensure_parent_dir(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            MKDIR(tmp);
            *p = '/';
        }
    }
    MKDIR(tmp);
}

static void job_progress_cb(const cdm_progress *p, void *user) {
    cdm_job *j = (cdm_job *)user;
    pthread_mutex_lock(&j->mtx);
    j->prog = *p;
    pthread_mutex_unlock(&j->mtx);
}

static void *job_runner(void *arg) {
    cdm_job *j = (cdm_job *)arg;
    pthread_detach(pthread_self());

    cdm_status st = cdm_download_run(j->dl);

    pthread_mutex_lock(&j->mtx);
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
    pthread_mutex_unlock(&j->mtx);
    return NULL;
}

static void job_start_thread(cdm_job *j) {
    j->running = 1;
    j->state = JOB_RUNNING;
    j->pause_req = 0;
    j->cancel_req = 0;
    j->requeue_req = 0;
    if (pthread_create(&j->thread, NULL, job_runner, j) != 0) {
        pthread_mutex_lock(&j->mtx);
        j->running = 0;
        j->state = JOB_ERROR;
        snprintf(j->errmsg, sizeof(j->errmsg), "thread spawn failed");
        pthread_mutex_unlock(&j->mtx);
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
    pthread_mutex_init(&m->mtx, NULL);
    return m;
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
#ifdef _WIN32
    base = getenv("APPDATA");
    if (base && *base) {
        suffix = "cdm/settings.conf";
    } else {
        base = getenv("USERPROFILE");
        if (base && *base) suffix = "AppData/Roaming/cdm/settings.conf";
    }
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        suffix = "cdm/settings.conf";
    } else {
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
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        int val;
        if (sscanf(line, "%63s %d", key, &val) != 2) continue;
        if (strcmp(key, "max_active") == 0)
            s->max_active = clamp_int(val, 0, CDM_SETTINGS_MAX_ACTIVE);
        else if (strcmp(key, "theme") == 0)
            s->theme = clamp_int(val, 0, 1);
        else if (strcmp(key, "skin") == 0)
            s->skin = clamp_int(val, 0, 3);
        /* unknown keys ignored for forward compatibility */
    }
    fclose(f);

    if (m) cdm_manager_set_max_active(m, s->max_active);
}

int cdm_settings_save(const cdm_settings *s) {
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
    if      (strcasecmp(ext,"mp4")==0||strcasecmp(ext,"mkv")==0||
             strcasecmp(ext,"avi")==0||strcasecmp(ext,"mov")==0||
             strcasecmp(ext,"webm")==0||strcasecmp(ext,"flv")==0) idx = 1;
    else if (strcasecmp(ext,"mp3")==0||strcasecmp(ext,"wav")==0||
             strcasecmp(ext,"flac")==0||strcasecmp(ext,"ogg")==0||
             strcasecmp(ext,"m4a")==0) idx = 2;
    else if (strcasecmp(ext,"exe")==0||strcasecmp(ext,"msi")==0||
             strcasecmp(ext,"deb")==0||strcasecmp(ext,"dmg")==0||
             strcasecmp(ext,"app")==0||strcasecmp(ext,"apk")==0) idx = 3;
    else if (strcasecmp(ext,"pdf")==0||strcasecmp(ext,"doc")==0||
             strcasecmp(ext,"docx")==0||strcasecmp(ext,"txt")==0||
             strcasecmp(ext,"xls")==0||strcasecmp(ext,"ppt")==0) idx = 4;
    else if (strcasecmp(ext,"zip")==0||strcasecmp(ext,"rar")==0||
             strcasecmp(ext,"7z")==0||strcasecmp(ext,"tar")==0||
             strcasecmp(ext,"gz")==0||strcasecmp(ext,"bz2")==0) idx = 5;
    return &m->cats[idx];
}

static int add_internal(cdm_manager *m, const char *url, const char *outpath,
                        const cdm_config *cfg, int queued, time_t sched_epoch) {
    if (!url || !*url) return -1;

    cdm_job *j = calloc(1, sizeof(*j));
    if (!j) return -1;
    pthread_mutex_init(&j->mtx, NULL);

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

    j->queued = queued;
    j->sched_epoch = sched_epoch;

    j->dl = cdm_download_create(&j->cfg);
    if (!j->dl) {
        snprintf(j->errmsg, sizeof j->errmsg, "create failed");
        j->state = JOB_ERROR;
        pthread_mutex_destroy(&j->mtx);
        free(j);
        return -1;
    }
    cdm_download_set_progress_cb(j->dl, job_progress_cb, j);

    /* decide whether to start now */
    int running = count_running(m);
    int can_start = (m->max_active <= 0) || (running < m->max_active);
    int sched_ok = (sched_epoch == 0) || ((time_t)time(NULL) >= sched_epoch);

    pthread_mutex_lock(&m->mtx);
    if (m->count >= m->capacity) {
        m->capacity *= 2;
        cdm_job **nj = realloc(m->jobs, (size_t)m->capacity * sizeof(cdm_job *));
        if (!nj) { pthread_mutex_unlock(&m->mtx); job_free(j); return -1; }
        m->jobs = nj;
    }
    m->jobs[m->count++] = j;
    m->selected_id = j->id;
    pthread_mutex_unlock(&m->mtx);

    if (!queued && can_start && sched_ok) {
        job_start_thread(j);
    } else {
        pthread_mutex_lock(&j->mtx);
        j->state = JOB_QUEUED;
        if (!queued) j->queued = 1; /* deferred by queue/schedule */
        pthread_mutex_unlock(&j->mtx);
    }
    return j->id;
}

int cdm_manager_add_ex(cdm_manager *m, const char *url, const char *outpath,
                       const cdm_config *cfg, int queued, time_t sched_epoch) {
    return add_internal(m, url, outpath, cfg, queued, sched_epoch);
}

int cdm_manager_add(cdm_manager *m, const char *url, const char *outpath,
                    const cdm_config *cfg) {
    return add_internal(m, url, outpath, cfg, 0, 0);
}

cdm_job *cdm_manager_find(cdm_manager *m, int id) {
    for (int i = 0; i < m->count; i++)
        if (m->jobs[i]->id == id) return m->jobs[i];
    return NULL;
}

void cdm_manager_pause(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_QUEUED) { j->state = JOB_PAUSED; j->queued = 0; }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_resume(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if ((j->state == JOB_PAUSED || j->state == JOB_QUEUED) && !j->running) {
            j->cfg.resume = 1;
            j->queued = 0;
            job_start_thread(j);
        }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_cancel(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_PAUSED || j->state == JOB_QUEUED)
            j->state = JOB_CANCELED;
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_remove(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        else { j->state = JOB_CANCELED; }
        j->remove_req = 1;
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_stop(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        else if (j->state == JOB_QUEUED) { j->state = JOB_PAUSED; j->queued = 0; }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_stop_all(cdm_manager *m) {
    pthread_mutex_lock(&m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        pthread_mutex_lock(&j->mtx);
        if (j->running) { j->pause_req = 1; cdm_download_cancel(j->dl); }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_delete_all_completed(cdm_manager *m) {
    pthread_mutex_lock(&m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        pthread_mutex_lock(&j->mtx);
        if (j->state == JOB_DONE) j->remove_req = 1;
        else if (j->running) { j->cancel_req = 1; cdm_download_cancel(j->dl); }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_add_to_queue(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        if (j->running) {
            j->requeue_req = 1;
            j->queued = 1;
            cdm_download_cancel(j->dl);
        } else if (j->state == JOB_PAUSED || j->state == JOB_CANCELED ||
                   j->state == JOB_ERROR) {
            j->queued = 1;
            j->state = JOB_QUEUED;
        }
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_remove_from_queue(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    cdm_job *j = cdm_manager_find(m, id);
    if (j) {
        pthread_mutex_lock(&j->mtx);
        j->queued = 0;
        if (j->state == JOB_QUEUED) j->state = JOB_PAUSED;
        pthread_mutex_unlock(&j->mtx);
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_set_max_active(cdm_manager *m, int n) {
    pthread_mutex_lock(&m->mtx);
    m->max_active = n < 0 ? 0 : n;
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_pump(cdm_manager *m) {
    pthread_mutex_lock(&m->mtx);
    int running = count_running(m);
    int slots = (m->max_active <= 0) ? 1000000 : (m->max_active - running);
    time_t now = (time_t)time(NULL);
    for (int i = 0; i < m->count && slots > 0; i++) {
        cdm_job *j = m->jobs[i];
        pthread_mutex_lock(&j->mtx);
        int start = 0;
        if (j->state == JOB_QUEUED && !j->running) {
            int sched_ok = (j->sched_epoch == 0) || (now >= j->sched_epoch);
            if (sched_ok && (j->queued || j->sched_epoch > 0)) start = 1;
        }
        pthread_mutex_unlock(&j->mtx);
        if (start) {
            job_start_thread(j);   /* sets RUNNING + running=1 */
            slots--;
        }
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_reap(cdm_manager *m) {
    pthread_mutex_lock(&m->mtx);
    for (int i = m->count - 1; i >= 0; i--) {
        cdm_job *j = m->jobs[i];
        int reap = 0;
        pthread_mutex_lock(&j->mtx);
        if (j->remove_req && !j->running) reap = 1;
        pthread_mutex_unlock(&j->mtx);
        if (reap) {
            if (m->selected_id == j->id) m->selected_id = -1;
            memmove(&m->jobs[i], &m->jobs[i + 1],
                    (size_t)(m->count - i - 1) * sizeof(cdm_job *));
            m->count--;
            job_free(j);
        }
    }
    pthread_mutex_unlock(&m->mtx);
}

void cdm_manager_select(cdm_manager *m, int id) {
    pthread_mutex_lock(&m->mtx);
    m->selected_id = id;
    pthread_mutex_unlock(&m->mtx);
}

static void job_free(cdm_job *j) {
    if (!j) return;
    if (j->dl) cdm_download_destroy(j->dl);
    pthread_mutex_destroy(&j->mtx);
    free(j);
}

void cdm_manager_destroy(cdm_manager *m) {
    if (!m) return;
    pthread_mutex_lock(&m->mtx);
    for (int i = 0; i < m->count; i++) {
        cdm_job *j = m->jobs[i];
        if (j->running) cdm_download_cancel(j->dl);
        job_free(j);
    }
    free(m->jobs);
    pthread_mutex_unlock(&m->mtx);
    pthread_mutex_destroy(&m->mtx);
    free(m);
}
