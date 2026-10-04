#ifndef CDM_MANAGER_H
#define CDM_MANAGER_H

#include "cdm/download.h"
#include "cdm/platform.h"
#include <time.h>

typedef enum {
    JOB_QUEUED,    /* waiting in queue / for schedule */
    JOB_RUNNING,
    JOB_PAUSED,
    JOB_DONE,
    JOB_ERROR,
    JOB_CANCELED
} cdm_job_state;

typedef struct cdm_job {
    int id;
    char url[2048];
    char outpath[2048];
    cdm_config cfg;
    cdm_download *dl;
    cdm_job_state state;

    /* live snapshot (guarded by mtx) */
    cdm_progress prog;
    cdm_status result;
    char errmsg[128];

    /* control flags (guarded by mtx) */
    int pause_req;
    int cancel_req;
    int remove_req;      /* delete when worker idle */
    int requeue_req;     /* cancel -> return to QUEUED */
    int queued;          /* belongs to the run queue */
    time_t sched_epoch;  /* 0 = no schedule; else start >= this */
    int cat_idx;         /* category index (-1 unknown) */

    /* runtime */
    int running;          /* worker thread alive */
    cdm_mutex *mtx;
} cdm_job;

typedef struct {
    char name[64];
    char dir[1024];
} cdm_category;

typedef struct {
    cdm_job **jobs;
    int count;
    int capacity;
    int next_id;
    int selected_id;

    cdm_category cats[16];
    int n_cats;
    int max_active;       /* queue concurrency limit (0 = unlimited) */

    cdm_mutex *mtx;
} cdm_manager;

cdm_manager *cdm_manager_create(void);
void cdm_manager_destroy(cdm_manager *m);

/* Add a download. queued=>wait in queue; sched_epoch>0=>start at/after time. */
int cdm_manager_add_ex(cdm_manager *m, const char *url, const char *outpath,
                       const cdm_config *cfg, int queued, time_t sched_epoch);
int cdm_manager_add(cdm_manager *m, const char *url, const char *outpath,
                    const cdm_config *cfg);

void cdm_manager_pause(cdm_manager *m, int id);
void cdm_manager_resume(cdm_manager *m, int id);
void cdm_manager_cancel(cdm_manager *m, int id);     /* stop + mark canceled */
void cdm_manager_stop(cdm_manager *m, int id);       /* stop -> requeue if queued */
void cdm_manager_remove(cdm_manager *m, int id);     /* cancel + free when idle */
void cdm_manager_stop_all(cdm_manager *m);
void cdm_manager_delete_all_completed(cdm_manager *m);

/* Queue controls */
void cdm_manager_add_to_queue(cdm_manager *m, int id);
void cdm_manager_remove_from_queue(cdm_manager *m, int id);
void cdm_manager_set_max_active(cdm_manager *m, int n);

/* Scheduler: promote queued jobs whose slot/time has arrived. */
void cdm_manager_pump(cdm_manager *m);

cdm_job *cdm_manager_find(cdm_manager *m, int id);

/* Reap finished jobs marked for removal (call from UI thread). */
void cdm_manager_reap(cdm_manager *m);

void cdm_manager_select(cdm_manager *m, int id);

/* Category helpers */
void cdm_manager_init_categories(cdm_manager *m, const char *base_dir);
const cdm_category *cdm_manager_category_for_ext(cdm_manager *m,
                                                 const char *name_or_url);

/* Persistent app settings (Options dialog). Stored under the user config
 * dir ($XDG_CONFIG_HOME/cdm or %APPDATA%/cdm on Windows). Unknown keys
 * and out-of-range values are ignored, so old/new versions interoperate. */
typedef struct {
    int max_active;  /* max concurrent downloads; 0 = unlimited */
    int theme;       /* 0 dark, 1 light */
    int skin;        /* 0 blue, 1 teal, 2 red, 3 purple */
} cdm_settings;

void cdm_settings_default(cdm_settings *s);
/* Load from disk (defaults when missing/corrupt) and apply max_active. */
void cdm_settings_load(cdm_manager *m, cdm_settings *s);
/* Persist to disk. Returns 0 on success, -1 when unwritable. */
int cdm_settings_save(const cdm_settings *s);

#endif /* CDM_MANAGER_H */
