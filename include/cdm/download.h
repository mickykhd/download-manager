#ifndef CDM_DOWNLOAD_H
#define CDM_DOWNLOAD_H

#include <stdint.h>
#include <stddef.h>

/*
 * Public engine API. The UI (CLI/TUI now, GUI later) talks only to this
 * header; the engine has no knowledge of how it is presented.
 */

typedef enum {
    CDM_OK = 0,
    CDM_ERR_ARG,          /* bad argument */
    CDM_ERR_NET,          /* network / curl failure */
    CDM_ERR_HTTP,         /* non-recoverable HTTP status */
    CDM_ERR_IO,           /* filesystem error */
    CDM_ERR_INTEGRITY,    /* size / checksum mismatch */
    CDM_ERR_CHANGED,      /* remote file changed mid-download (ETag) */
    CDM_ERR_CANCELED,
    CDM_ERR_NOMEM,
    CDM_ERR_INTERNAL
} cdm_status;

const char *cdm_status_str(cdm_status s);

/* Configuration for a single download. Zero-initialise then override. */
typedef struct {
    const char *url;
    const char *output_path;   /* NULL => derive from URL/Content-Disposition */
    int max_connections;       /* hard cap; 0 => default (16) */
    int start_connections;     /* initial concurrency; 0 => default (4) */
    int64_t chunk_size;        /* work-stealing piece size; 0 => default (8 MiB) */
    int64_t max_speed_bps;     /* aggregate throttle; 0 => unlimited */
    int max_retries;           /* per-chunk transient retries; 0 => default (5) */
    int resume;                /* 1 => attempt resume from .cdm sidecar */
    int adaptive;              /* 1 => adaptive concurrency probing */
    const char *expected_sha256; /* optional hex digest for integrity gate */
    int quiet;                 /* suppress progress callbacks driving output */
} cdm_config;

void cdm_config_default(cdm_config *cfg);

/* Live snapshot passed to the progress callback. */
typedef struct {
    int64_t total_bytes;       /* -1 if unknown */
    int64_t downloaded_bytes;
    double speed_bps;          /* smoothed */
    double eta_seconds;        /* -1 if unknown */
    int active_connections;
    int total_chunks;
    int completed_chunks;
    int resumed;               /* 1 if this run resumed prior progress */
} cdm_progress;

typedef void (*cdm_progress_cb)(const cdm_progress *p, void *user);

typedef struct cdm_download cdm_download;

/* Lifecycle */
cdm_status cdm_global_init(void);
void cdm_global_cleanup(void);

cdm_download *cdm_download_create(const cdm_config *cfg);
void cdm_download_set_progress_cb(cdm_download *d, cdm_progress_cb cb, void *user);

/* Blocking run. Returns CDM_OK on success. */
cdm_status cdm_download_run(cdm_download *d);

/* Cooperative cancel (safe from another thread / signal handler). */
void cdm_download_cancel(cdm_download *d);

/* Resolved output path after run (valid until destroy). */
const char *cdm_download_output_path(const cdm_download *d);

void cdm_download_destroy(cdm_download *d);

#endif /* CDM_DOWNLOAD_H */
