#ifndef CDM_INTERNAL_H
#define CDM_INTERNAL_H

#include "cdm/download.h"
#include "cdm/platform.h"
#include <curl/curl.h>

#define CDM_DEFAULT_MAX_CONN     16
#define CDM_DEFAULT_START_CONN   4
#define CDM_DEFAULT_CHUNK        (8 * 1024 * 1024)
#define CDM_MIN_CHUNK            (1 * 1024 * 1024)
#define CDM_DEFAULT_RETRIES      5
#define CDM_USER_AGENT           "cdm/0.1 (+libcurl)"

typedef enum {
    CHUNK_PENDING = 0,
    CHUNK_ACTIVE,
    CHUNK_DONE
} chunk_state;

/* One contiguous byte range of the output file. */
typedef struct {
    int64_t offset;   /* absolute start in file */
    int64_t length;   /* total bytes of this chunk */
    int64_t done;     /* bytes already written */
    chunk_state state;
} cdm_chunk;

/* Remote resource facts gathered by the probe. */
typedef struct {
    int64_t size;          /* -1 if unknown */
    int accept_ranges;     /* server advertises byte ranges */
    char etag[256];        /* strong validator (empty if none/weak) */
    char last_modified[128];
    char filename[512];    /* derived name (Content-Disposition or URL) */
    char effective_url[2048];
} cdm_probe;

/* Per easy-handle transfer state. */
typedef struct cdm_transfer {
    CURL *easy;
    struct cdm_download *d;
    cdm_chunk *chunk;
    struct curl_slist *headers;
    long response_code;
    int retries;
    int checked;          /* verified response code on first write */
    char errbuf[CURL_ERROR_SIZE];
    int in_multi;
} cdm_transfer;

struct cdm_download {
    cdm_config cfg;
    cdm_probe probe;

    char *url;
    char *output_path;     /* final resolved path */
    char *part_path;       /* output_path + ".part" */
    char *meta_path;       /* output_path + ".cdm" */

    cdm_file *file;

    cdm_chunk *chunks;
    int chunk_count;

    cdm_transfer *transfers;
    int transfer_cap;      /* allocated slots (== max_connections) */
    int active_transfers;

    CURLM *multi;

    int64_t total_bytes;   /* -1 if unknown */
    int64_t downloaded;    /* bytes written this + prior runs */
    int single_stream;     /* no range support => one connection */
    int resumed;

    /* speed metering */
    double start_time;
    double last_sample_time;
    int64_t last_sample_bytes;
    int64_t base_bytes;      /* downloaded at transfer start (resume-aware) */
    double speed_bps;

    /* adaptive concurrency */
    int target_conn;       /* desired concurrent handles */
    double adapt_last_time;
    int64_t adapt_last_bytes;
    double adapt_last_speed;

    volatile int cancel;
    volatile int remote_changed;  /* server returned 200 for a ranged chunk */

    cdm_progress_cb progress_cb;
    void *progress_user;

    cdm_mutex *lock;       /* guards downloaded/chunks during callbacks */
};

/* httpinfo.c */
cdm_status cdm_probe_url(const char *url, const cdm_config *cfg, cdm_probe *out);

/* segment.c */
size_t cdm_write_cb(char *ptr, size_t size, size_t nmemb, void *userp);
void cdm_transfer_config(cdm_transfer *t);              /* set curl opts for its chunk */
cdm_chunk *cdm_next_pending_chunk(struct cdm_download *d);

/* scheduler.c */
cdm_status cdm_run_transfers(struct cdm_download *d);

/* retry.c */
int cdm_error_is_permanent(CURLcode code, long http_status);
double cdm_backoff_seconds(int attempt);                 /* exponential + jitter */

/* connopts.c: UA/referer/cookie/headers/proxy from cfg. Appends to *slist
 * when slist != NULL (caller applies CURLOPT_HTTPHEADER). */
void cdm_apply_conn_opts(CURL *c, const cdm_config *cfg,
                         struct curl_slist **slist);

/* resume.c */
int cdm_meta_load(struct cdm_download *d);   /* 1 if valid resumable state loaded */
int cdm_meta_save(struct cdm_download *d);
void cdm_meta_delete(struct cdm_download *d);

/* speedmeter.c */
void cdm_speed_update(struct cdm_download *d);
void cdm_speed_finalize(struct cdm_download *d); /* fold run average into speed */
void cdm_emit_progress(struct cdm_download *d);

/* integrity.c */
cdm_status cdm_verify_sha256(const char *path, const char *expected_hex);

/* download.c: parse an HTTP date ("Wed, 21 Oct 2015 07:28:00 GMT") to
 * unix seconds, -1 when unparsable. */
int64_t cdm_parse_http_date(const char *s);

#endif /* CDM_INTERNAL_H */
