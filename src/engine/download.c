#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define CDM_MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define CDM_MKDIR(d) mkdir((d), 0755)
#endif

/* mkdir the parent chain of a FILE path (engine-level: the CLI and API
 * bypass the manager, which used to be the only place creating them). */
static void cdm_ensure_parent(const char *path) {
    char tmp[2048];
    char *slash;
    if (!path || !*path) return;
    snprintf(tmp, sizeof(tmp), "%s", path);
    slash = strrchr(tmp, '/');
#ifdef _WIN32
    {
        char *b = strrchr(tmp, '\\');
        if (b && (!slash || b > slash)) slash = b;
    }
#endif
    if (!slash) return;
    *slash = 0;
    if (!tmp[0]) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = 0;
            CDM_MKDIR(tmp);
            *p = sep;
        }
    }
    CDM_MKDIR(tmp);
}

const char *cdm_status_str(cdm_status s) {
    switch (s) {
        case CDM_OK:            return "ok";
        case CDM_ERR_ARG:       return "invalid argument";
        case CDM_ERR_NET:       return "network error";
        case CDM_ERR_HTTP:      return "http error";
        case CDM_ERR_IO:        return "i/o error";
        case CDM_ERR_INTEGRITY: return "integrity check failed";
        case CDM_ERR_CHANGED:   return "remote file changed";
        case CDM_ERR_CANCELED:  return "canceled";
        case CDM_ERR_NOMEM:     return "out of memory";
        default:                return "internal error";
    }
}

void cdm_config_default(cdm_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->max_connections = CDM_DEFAULT_MAX_CONN;
    cfg->start_connections = CDM_DEFAULT_START_CONN;
    cfg->chunk_size = CDM_DEFAULT_CHUNK;
    cfg->max_retries = CDM_DEFAULT_RETRIES;
    cfg->resume = 1;
    cfg->adaptive = 1;
    cfg->proxy_mode = CDM_PROXY_SYSTEM; /* respect env/system proxy */
    cfg->dup_mode = 0;
    cfg->ignore_ssl = 0;
    cfg->sparse = 0;
    cfg->preserve_time = 1;
}

cdm_status cdm_global_init(void) {
    return curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK ? CDM_OK : CDM_ERR_INTERNAL;
}

void cdm_global_cleanup(void) { curl_global_cleanup(); }

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *join_suffix(const char *base, const char *suffix) {
    size_t n = strlen(base) + strlen(suffix) + 1;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s%s", base, suffix);
    return p;
}

cdm_download *cdm_download_create(const cdm_config *cfg) {
    if (!cfg || !cfg->url) return NULL;
    cdm_download *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->cfg = *cfg;
    if (d->cfg.max_connections <= 0)  d->cfg.max_connections = CDM_DEFAULT_MAX_CONN;
    if (d->cfg.start_connections <= 0) d->cfg.start_connections = CDM_DEFAULT_START_CONN;
    if (d->cfg.chunk_size <= 0)       d->cfg.chunk_size = CDM_DEFAULT_CHUNK;
    if (d->cfg.chunk_size < CDM_MIN_CHUNK) d->cfg.chunk_size = CDM_MIN_CHUNK;
    if (d->cfg.max_retries < 0)       d->cfg.max_retries = CDM_DEFAULT_RETRIES;
    if (d->cfg.proxy_mode < CDM_PROXY_DIRECT || d->cfg.proxy_mode > CDM_PROXY_MANUAL)
        d->cfg.proxy_mode = CDM_PROXY_SYSTEM; /* historical curl behaviour */
    if (d->cfg.start_connections > d->cfg.max_connections)
        d->cfg.start_connections = d->cfg.max_connections;

    d->url = dup_str(cfg->url);
    d->total_bytes = -1;
    d->target_conn = d->cfg.start_connections;
    d->lock = cdm_mutex_create();
    if (!d->url || !d->lock) { cdm_download_destroy(d); return NULL; }
    return d;
}

void cdm_download_set_progress_cb(cdm_download *d, cdm_progress_cb cb, void *user) {
    d->progress_cb = cb;
    d->progress_user = user;
}

void cdm_download_cancel(cdm_download *d) { if (d) d->cancel = 1; }

const char *cdm_download_output_path(const cdm_download *d) {
    return d ? d->output_path : NULL;
}

/* Build the chunk table for a fresh (non-resumed) download. */
static cdm_status plan_chunks(cdm_download *d) {
    if (d->single_stream || d->total_bytes < 0) {
        d->chunk_count = 1;
        d->chunks = calloc(1, sizeof(cdm_chunk));
        if (!d->chunks) return CDM_ERR_NOMEM;
        d->chunks[0].offset = 0;
        d->chunks[0].length = d->total_bytes < 0 ? 0 : d->total_bytes;
        d->chunks[0].state = CHUNK_PENDING;
        return CDM_OK;
    }

    int64_t csz = d->cfg.chunk_size;
    int64_t n = (d->total_bytes + csz - 1) / csz;
    if (n < 1) n = 1;
    if (n > 4096) { /* cap chunk count; grow chunk size */
        csz = (d->total_bytes + 4095) / 4096;
        n = (d->total_bytes + csz - 1) / csz;
    }

    d->chunks = calloc((size_t)n, sizeof(cdm_chunk));
    if (!d->chunks) return CDM_ERR_NOMEM;
    d->chunk_count = (int)n;
    for (int64_t i = 0; i < n; i++) {
        int64_t off = i * csz;
        int64_t len = csz;
        if (off + len > d->total_bytes) len = d->total_bytes - off;
        d->chunks[i].offset = off;
        d->chunks[i].length = len;
        d->chunks[i].state = CHUNK_PENDING;
    }
    return CDM_OK;
}

static cdm_status resolve_paths(cdm_download *d) {
    const char *out = d->cfg.output_path;
    if (out && *out) {
        d->output_path = dup_str(out);
    } else {
        d->output_path = dup_str(d->probe.filename[0] ? d->probe.filename
                                                      : "download.bin");
    }
    if (!d->output_path) return CDM_ERR_NOMEM;
    /* existing output with no resume state: rename unless overwriting */
    if (cdm_file_exists(d->output_path) && d->cfg.dup_mode == 0) {
        char part[2200], meta[2200];
        snprintf(part, sizeof(part), "%s.part", d->output_path);
        snprintf(meta, sizeof(meta), "%s.cdm", d->output_path);
        if (!(d->cfg.resume && cdm_file_exists(part) && cdm_file_exists(meta))) {
            char *base = d->output_path;
            char *dot, *slash1;
            const char *stem;
            int n;
            dot = strrchr(base, '.');
            slash1 = strrchr(base, '/');
#ifdef _WIN32
            {
                char *bslash = strrchr(base, '\\');
                if (bslash && (!slash1 || bslash > slash1)) slash1 = bslash;
            }
#endif
            stem = slash1 ? slash1 + 1 : base;
            for (n = 1; n < 1000; n++) {
                char cand[2200];
                if (dot && dot > stem)
                    snprintf(cand, sizeof(cand), "%.*s (%d)%s",
                             (int)(dot - base), base, n, dot);
                else
                    snprintf(cand, sizeof(cand), "%s (%d)", base, n);
                if (!cdm_file_exists(cand)) {
                    free(d->output_path);
                    d->output_path = dup_str(cand);
                    if (!d->output_path) return CDM_ERR_NOMEM;
                    break;
                }
            }
            if (n >= 1000) return CDM_ERR_IO;
        }
    }
    d->part_path = join_suffix(d->output_path, ".part");
    d->meta_path = join_suffix(d->output_path, ".cdm");
    if (!d->part_path || !d->meta_path) return CDM_ERR_NOMEM;
    return CDM_OK;
}

static cdm_status alloc_transfers(cdm_download *d) {
    d->transfer_cap = d->cfg.max_connections;
    d->transfers = calloc((size_t)d->transfer_cap, sizeof(cdm_transfer));
    if (!d->transfers) return CDM_ERR_NOMEM;
    for (int i = 0; i < d->transfer_cap; i++) {
        d->transfers[i].easy = curl_easy_init();
        d->transfers[i].d = d;
        if (!d->transfers[i].easy) return CDM_ERR_INTERNAL;
    }
    return CDM_OK;
}

cdm_status cdm_download_run(cdm_download *d) {
    if (!d) return CDM_ERR_ARG;

    cdm_status st = cdm_probe_url(d->url, &d->cfg, &d->probe);
    if (st != CDM_OK) return st;

    d->total_bytes = d->probe.size;
    d->single_stream = !d->probe.accept_ranges || d->total_bytes < 0;

    st = resolve_paths(d);
    if (st != CDM_OK) return st;

    st = plan_chunks(d);
    if (st != CDM_OK) return st;

    /* Try to resume from an existing sidecar + .part file. */
    if (d->cfg.resume && cdm_file_exists(d->part_path) &&
        cdm_file_exists(d->meta_path)) {
        cdm_meta_load(d); /* on failure we keep the fresh plan */
    }

    cdm_ensure_parent(d->part_path);
    d->file = cdm_file_open_rw(d->part_path);
    if (!d->file) return CDM_ERR_IO;

    if (d->cfg.sparse) {
        cdm_file_set_sparse(d->file); /* best effort; writes extend sparsely */
    } else if (d->total_bytes > 0) {
        if (cdm_file_preallocate(d->file, d->total_bytes) != 0) {
            /* non-fatal: some filesystems disallow; writes still extend */
        }
    }

    st = alloc_transfers(d);
    if (st != CDM_OK) { cdm_file_close(d->file); d->file = NULL; return st; }

    st = cdm_run_transfers(d);

    /* stamp server time before closing (rename preserves it) */
    if (st == CDM_OK && d->cfg.preserve_time && d->probe.last_modified[0]) {
        int64_t mtime = cdm_parse_http_date(d->probe.last_modified);
        if (mtime >= 0) cdm_file_set_mtime(d->file, mtime);
    }

    cdm_file_close(d->file);
    d->file = NULL;

    if (st != CDM_OK) {
        cdm_meta_save(d); /* keep progress for a later resume */
        return st;
    }

    /* Size gate. */
    if (d->total_bytes > 0) {
        cdm_file *chk = cdm_file_open_rw(d->part_path);
        int64_t actual = chk ? cdm_file_size(chk) : -1;
        if (chk) cdm_file_close(chk);
        if (actual != d->total_bytes) return CDM_ERR_INTEGRITY;
    }

    /* Optional checksum gate before promoting the file. */
    if (d->cfg.expected_sha256 && *d->cfg.expected_sha256) {
        st = cdm_verify_sha256(d->part_path, d->cfg.expected_sha256);
        if (st != CDM_OK) return st;
    }

    /* Promote .part -> final and drop the sidecar. */
    cdm_remove(d->output_path);
    if (cdm_rename(d->part_path, d->output_path) != 0) return CDM_ERR_IO;
    cdm_meta_delete(d);
    return CDM_OK;
}

int64_t cdm_parse_http_date(const char *s) {
    static const char *mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day, year, hh, mm, ss, mi, i;
    int64_t days;
    char mstr[4];
    /* IMF-fixdate: "Wed, 21 Oct 2015 07:28:00 GMT" (also accept without
     * weekday or with '-' separators via %d-%3s-%d tolerant scan) */
    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (sscanf(s, "%*3s , %d %3s %d %d : %d : %d", &day, mstr, &year,
               &hh, &mm, &ss) != 6) {
        if (sscanf(s, "%d-%3s-%d %d:%d:%d", &day, mstr, &year,
                   &hh, &mm, &ss) != 6)
            return -1;
    }
    mstr[3] = 0;
    for (i = 0; i < 12; i++) {
        int match = 1;
        for (int k = 0; k < 3; k++) {
            /* table is mixed-case ("Oct"): fold BOTH sides */
            char a = mstr[k], b = mon[i][k];
            if (a >= 'a' && a <= 'z') a -= (char)('a' - 'A');
            if (b >= 'a' && b <= 'z') b -= (char)('a' - 'A');
            if (a != b) { match = 0; break; }
        }
        if (match) break;
    }
    if (i >= 12) return -1;
    mi = i; /* 0-based month */
    if (day < 1 || day > 31 || year < 1970 || year > 2100 ||
        hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60)
        return -1;
    /* days since epoch (civil date algorithm) */
    {
        int64_t y = year, m = mi + 1, d = day;
        if (m <= 2) {
            y--;
            m += 12;
        }
        days = 365 * y + y / 4 - y / 100 + y / 400 +
               (153 * (m - 3) + 2) / 5 + d - 719469;
    }
    return days * 86400LL + hh * 3600LL + mm * 60LL + ss;
}

void cdm_download_destroy(cdm_download *d) {
    if (!d) return;
    if (d->transfers) {
        for (int i = 0; i < d->transfer_cap; i++) {
            if (d->transfers[i].headers)
                curl_slist_free_all(d->transfers[i].headers);
            if (d->transfers[i].easy)
                curl_easy_cleanup(d->transfers[i].easy);
        }
        free(d->transfers);
    }
    if (d->file) cdm_file_close(d->file);
    free(d->chunks);
    free(d->url);
    free(d->output_path);
    free(d->part_path);
    free(d->meta_path);
    if (d->lock) cdm_mutex_destroy(d->lock);
    free(d);
}
