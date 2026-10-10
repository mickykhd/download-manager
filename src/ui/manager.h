#ifndef CDM_MANAGER_H
#define CDM_MANAGER_H

#include "cdm/download.h"
#include "cdm/platform.h"
#include <time.h>

#define CDM_MAX_QUEUES 8
#define CDM_MAX_HOSTS 16
#define CDM_MAX_JOB_HEADERS 32
#define CDM_MAX_PENDING_URLS 16

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
    int queue_idx;       /* owning queue, -1 = run immediately (not queued) */
    time_t sched_epoch;  /* 0 = no schedule; else start >= this */
    int cat_idx;         /* category index (-1 unknown) */

    /* job-owned network strings (cfg points here, never dangles) */
    char net_ua[256];
    char net_referer[512];
    char net_cookie[1024];
    char hdr_blob[2048];
    const char *hdr_ptrs[CDM_MAX_JOB_HEADERS];
    int n_hdrs;

    /* runtime */
    int running;          /* worker thread alive */
    cdm_mutex *mtx;
} cdm_job;

typedef struct {
    char name[64];
    char dir[1024];
} cdm_category;

/* A named run queue with its own concurrency limit and optional daily
 * time window (minutes since midnight, -1 = no bound). Jobs whose
 * queue_idx points here start only while the queue is enabled, the
 * window is open, and a slot is free. Queue 0 is always "Default". */
typedef struct {
    char name[64];
    int max_active;      /* per-queue limit; <0 = use manager default */
    int sched_start_min; /* -1 = no window bound */
    int sched_end_min;   /* -1 = no window bound */
    int enabled;
} cdm_queue;

/* Per-host connection policy. Non-zero/non-empty fields override the
 * download's own settings. */
typedef struct {
    char host[256];      /* lowercase hostname, no port */
    int max_connections; /* 0 = default */
    int64_t max_speed_bps;
    char user_agent[256];/* empty = default */
} cdm_host_rule;

typedef struct cdm_manager {
    cdm_job **jobs;
    int count;
    int capacity;
    int next_id;
    int selected_id;

    cdm_category cats[16];
    int n_cats;
    int max_active;       /* default concurrency limit (0 = unlimited) */

    cdm_queue queues[CDM_MAX_QUEUES];
    int n_queues;
    int default_queue;    /* index used by the Add dialog's queue option */

    /* connection policy */
    int proxy_mode;       /* CDM_PROXY_* */
    char proxy_url[512];  /* manual mode */
    cdm_host_rule hosts[CDM_MAX_HOSTS];
    int n_hosts;

    /* inbox for URLs fed by the integration server; the UI drains it */
    char pending_urls[CDM_MAX_PENDING_URLS][2048];
    int n_pending;

    cdm_mutex *mtx;
} cdm_manager;

cdm_manager *cdm_manager_create(void);
void cdm_manager_destroy(cdm_manager *m);

/* Add a download. queued=>wait in the default queue; sched_epoch>0=>start
 * at/after time. */
int cdm_manager_add_ex(cdm_manager *m, const char *url, const char *outpath,
                       const cdm_config *cfg, int queued, time_t sched_epoch);
/* Add straight into a queue (queue_idx in [0, n_queues)). */
int cdm_manager_add_to_queue_idx(cdm_manager *m, const char *url,
                                 const char *outpath, const cdm_config *cfg,
                                 int queue_idx, time_t sched_epoch);
int cdm_manager_add(cdm_manager *m, const char *url, const char *outpath,
                    const cdm_config *cfg);
/* Add many URLs at once; returns number accepted. */
int cdm_manager_add_batch(cdm_manager *m, const char **urls, int n,
                          const cdm_config *cfg, int queue_idx);
/* Expand one {start:end} range in a URL pattern into out[][2048].
 * Supports zero-padded bounds ({001:100}). Returns count (0 = no range
 * found, out[0] gets a copy of the pattern). At most cap entries. */
int cdm_expand_range(const char *pattern, char out[][2048], int cap);
/* Edit a non-running job's URL/output/speed/queue. Returns 0 ok, -1 busy. */
int cdm_manager_edit(cdm_manager *m, int id, const char *url,
                     const char *outpath, int64_t max_speed_bps,
                     int queue_idx);
/* Edit a non-running job's UA/referer/cookie/extra-headers. NULL keeps.
 * Returns 0 ok, -1 busy. */
int cdm_manager_edit_net(cdm_manager *m, int id, const char *user_agent,
                         const char *referer, const char *cookie,
                         const char *headers_text);

void cdm_manager_pause(cdm_manager *m, int id);
void cdm_manager_resume(cdm_manager *m, int id);
void cdm_manager_cancel(cdm_manager *m, int id);     /* stop + mark canceled */
void cdm_manager_stop(cdm_manager *m, int id);       /* stop -> requeue if queued */
void cdm_manager_remove(cdm_manager *m, int id);     /* cancel + free when idle */
void cdm_manager_stop_all(cdm_manager *m);
void cdm_manager_delete_all_completed(cdm_manager *m);

/* Queue controls */
void cdm_manager_add_to_queue(cdm_manager *m, int id);
void cdm_manager_move_to_queue(cdm_manager *m, int id, int queue_idx);
void cdm_manager_remove_from_queue(cdm_manager *m, int id);
void cdm_manager_set_max_active(cdm_manager *m, int n);
/* Queue definitions. Returns index, or -1 when full/invalid. */
int cdm_manager_queue_add(cdm_manager *m, const char *name);
int cdm_manager_queue_remove(cdm_manager *m, int idx);
void cdm_manager_queue_set(cdm_manager *m, int idx, const char *name,
                           int max_active, int start_min, int end_min,
                           int enabled);
int cdm_manager_queue_window_open(const cdm_queue *q, time_t now);

/* Connection policy. */
void cdm_manager_set_proxy(cdm_manager *m, int mode, const char *url);
int cdm_manager_host_add(cdm_manager *m, const char *host);
int cdm_manager_host_remove(cdm_manager *m, int idx);
void cdm_manager_host_set(cdm_manager *m, int idx, int max_connections,
                          int64_t max_speed_bps, const char *user_agent);
/* hostname of url, lowercased, no port. host="" when unparsable. */
void cdm_host_of_url(const char *url, char *host, size_t cap);
const cdm_host_rule *cdm_manager_host_for_url(cdm_manager *m, const char *url);
/* Split "Name: value" lines into blob/ptrs. Returns header count. */
int cdm_parse_headers(const char *text, char *blob, size_t bcap,
                      const char **ptrs, int pcap);

/* Integration inbox: queue a URL for the UI thread (drops when full),
 * and drain all pending URLs into out (returns count). */
void cdm_manager_push_url(cdm_manager *m, const char *url);
int cdm_manager_poll_urls(cdm_manager *m, char out[][2048], int cap);

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
    int api_enabled; /* local integration server on/off */
    int api_port;    /* 0 = default (15151) */
    char api_key[128]; /* empty = no auth */
    int tray_close;  /* close button hides to tray instead of quitting */
    int autostart;   /* start with OS login (managed live, persisted too) */
    int update_check;/* check GitHub releases at startup */
} cdm_settings;

void cdm_settings_default(cdm_settings *s);
/* Load from disk (defaults when missing/corrupt) and apply max_active. */
void cdm_settings_load(cdm_manager *m, cdm_settings *s);
/* Persist to disk (including queue definitions). Returns 0 on success. */
int cdm_settings_save(const cdm_manager *m, const cdm_settings *s);

#endif /* CDM_MANAGER_H */
