#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int hls_ncasecmp(const char *a, const char *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb)) return (int)tolower(ca) - (int)tolower(cb);
        if (ca == 0) return 0;
    }
    return 0;
}

/*
 * HLS (m3u8) downloads: master playlist -> best variant -> media playlist
 * -> segments fetched sequentially and concatenated. Resume continues at
 * the segment index stored in the .cdm sidecar. Encrypted streams
 * (EXT-X-KEY METHOD != NONE) are rejected with a clean error.
 */

#define CDM_HLS_MAX_DOC (4 * 1024 * 1024)
#define CDM_HLS_MAX_SEGS 100000
#define CDM_HLS_MAGIC "CDM-HLS1"

typedef struct {
    char *data;
    size_t len;
} hlsdoc;

static size_t hls_body_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    hlsdoc *d = (hlsdoc *)userp;
    size_t n = size * nmemb;
    char *nd;
    if (d->len + n + 1 > CDM_HLS_MAX_DOC) return 0; /* too big: abort */
    nd = (char *)realloc(d->data, d->len + n + 1);
    if (!nd) return 0;
    d->data = nd;
    memcpy(d->data + d->len, ptr, n);
    d->len += n;
    d->data[d->len] = 0;
    return n;
}

/* GET a URL into memory (connopts applied for proxy/auth). 200 required. */
static int hls_fetch(const char *url, const cdm_config *cfg, hlsdoc *out) {
    CURL *c = curl_easy_init();
    long code = 0;
    CURLcode rc;
    if (!c) return -1;
    out->data = NULL;
    out->len = 0;
    {
        struct curl_slist *hs = NULL;
        cdm_apply_conn_opts(c, cfg, &hs);
        if (hs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
        curl_easy_setopt(c, CURLOPT_URL, url);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, hls_body_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (hs) curl_slist_free_all(hs);
        curl_easy_cleanup(c);
    }
    if (rc != CURLE_OK || code < 200 || code >= 300) {
        free(out->data);
        out->data = NULL;
        out->len = 0;
        return -1;
    }
    return 0;
}

/* Resolve ref against base playlist URL into out. */
void cdm_hls_resolve(const char *base, const char *ref, char *out, size_t cap) {
    const char *scheme, *host_end, *path;
    size_t span;
    if (!ref || !*ref || !base || !out || cap == 0) {
        if (out && cap > 0) out[0] = 0;
        return;
    }
    while (*ref == ' ' || *ref == '\t') ref++;
    if (strstr(ref, "://")) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    scheme = strstr(base, "://");
    if (!scheme) {
        snprintf(out, cap, "%s", ref);
        return;
    }
    span = (size_t)(scheme - base) + 3;
    host_end = strpbrk(base + span, "/?#");
    if (!host_end) host_end = base + strlen(base);
    if (ref[0] == '/') {
        snprintf(out, cap, "%.*s%s", (int)(host_end - base), base, ref);
        return;
    }
    /* relative: base directory (strip query/fragment first) */
    path = host_end;
    {
        const char *dirend = strrchr(path, '/');
        size_t dirlen;
        if (!dirend)
            dirlen = (size_t)(host_end - base);
        else
            dirlen = (size_t)(dirend + 1 - base);
        /* collapse leading ./ and resolve ../ minimally */
        while (strncmp(ref, "./", 2) == 0) ref += 2;
        while (strncmp(ref, "../", 3) == 0) {
            ref += 3;
            if (dirlen > (size_t)(host_end - base)) {
                /* walk one level up, never above host root */
                const char *s = base + dirlen - 2; /* skip trailing '/' */
                while (s > host_end && *s != '/') s--;
                dirlen = (size_t)(s + 1 - base);
            }
        }
        snprintf(out, cap, "%.*s%s", (int)dirlen, base, ref);
    }
}

/* master playlist? pick highest-BANDWIDTH variant URI into out. 1 = found. */
int cdm_hls_pick_variant(const char *doc, char *out, size_t cap) {
    const char *p = doc;
    long long best = -1;
    char besturi[2048] = {0};
    while ((p = strstr(p, "#EXT-X-STREAM-INF:")) != NULL) {
        const char *eol = strchr(p, '\n');
        const char *bw = strstr(p, "BANDWIDTH=");
        long long w = -1;
        const char *u, *uend;
        if (bw && (!eol || bw < eol)) w = strtoll(bw + 10, NULL, 10);
        u = eol ? eol + 1 : NULL;
        while (u && (*u == '\r' || *u == '\n' || *u == ' ' || *u == '\t')) u++;
        if (!u || *u == '#' || *u == 0) {
            p = eol ? eol + 1 : p + 1;
            continue;
        }
        uend = strpbrk(u, "\r\n");
        if (!uend) uend = u + strlen(u);
        if (w >= best && (size_t)(uend - u) < sizeof(besturi)) {
            size_t n = (size_t)(uend - u);
            memcpy(besturi, u, n);
            besturi[n] = 0;
            best = w;
        }
        p = uend;
    }
    if (!besturi[0]) return 0;
    snprintf(out, cap, "%s", besturi);
    return 1;
}

/* media playlist: init-segment MAP uri (may be empty) + segment URIs.
 * Returns segment count, or -1 on encrypted/unsupported input. */
int cdm_hls_segments(const char *doc, char *map_uri, size_t map_cap,
                     char (*uris)[2048], int cap) {
    int n = 0;
    const char *p = doc;
    if (map_uri && map_cap > 0) map_uri[0] = 0;
    while (*p && n < cap && n < CDM_HLS_MAX_SEGS) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[2048];
        if (linelen >= sizeof(line)) linelen = sizeof(line) - 1;
        memcpy(line, p, linelen);
        line[linelen] = 0;
        /* trim trailing \r/spaces */
        while (linelen > 0 && (line[linelen-1] == '\r' || line[linelen-1] == ' ' ||
                               line[linelen-1] == '\t'))
            line[--linelen] = 0;
        if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
            /* only clear content supported */
            if (!strstr(line, "METHOD=NONE") && !strstr(line, "METHOD=none")) {
                const char *m = strstr(line, "METHOD=");
                char meth[32] = {0};
                if (m) {
                    size_t k = 0;
                    m += 7;
                    while (*m && *m != ',' && k + 1 < sizeof(meth))
                        meth[k++] = *m++;
                    meth[k] = 0;
                }
                if (strcmp(meth, "NONE") != 0 && strcmp(meth, "none") != 0)
                    return -1; /* encrypted: bail out cleanly */
            }
        } else if (strncmp(line, "#EXT-X-MAP:", 11) == 0) {
            const char *u = strstr(line, "URI=\"");
            if (u && map_uri && map_cap > 0) {
                size_t k = 0;
                u += 5;
                while (*u && *u != '"' && k + 1 < map_cap)
                    map_uri[k++] = *u++;
                map_uri[k] = 0;
            }
        } else if (line[0] && line[0] != '#') {
            snprintf(uris[n], 2048, "%s", line);
            n++;
        }
        p = eol ? eol + 1 : p + linelen;
        if (!eol) break;
    }
    return n;
}

/* Pure detection (no network): .m3u8/.m3u suffix or an HLS content type
 * captured by the probe. */
int cdm_hls_should_handle(const char *url, const char *content_type) {
    const char *q, *end, *dot;
    if (content_type && *content_type) {
        if (strstr(content_type, "mpegurl") || strstr(content_type, "mpegURL") ||
            strstr(content_type, "MPEGURL") || strstr(content_type, "x-mpeg"))
            return 1;
    }
    if (!url) return 0;
    q = strpbrk(url, "?#");
    end = q ? q : url + strlen(url);
    dot = NULL;
    for (const char *p = url; p < end; p++)
        if (*p == '.') dot = p;
    if (dot) {
        size_t ext = (size_t)(end - dot);
        if ((ext == 5 && hls_ncasecmp(dot, ".m3u8", 5) == 0) ||
            (ext == 4 && hls_ncasecmp(dot, ".m3u", 4) == 0))
            return 1;
    }
    return 0;
}

static int hls_meta_save(struct cdm_download *d, int seg_done, int seg_total) {
    FILE *f;
    if (!d->meta_path) return -1;
    f = fopen(d->meta_path, "wb");
    if (!f) return -1;
    fprintf(f, "%s\nsegments %d\ndone %d\n", CDM_HLS_MAGIC, seg_total, seg_done);
    fclose(f);
    return 0;
}

static int hls_meta_load(struct cdm_download *d, int *seg_done) {
    FILE *f;
    char line[256];
    if (!d->meta_path || !seg_done) return 0;
    f = fopen(d->meta_path, "rb");
    if (!f) return 0;
    *seg_done = 0;
    if (!fgets(line, sizeof(line), f) ||
        strncmp(line, CDM_HLS_MAGIC, strlen(CDM_HLS_MAGIC)) != 0) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "done %d", seg_done) == 1) {
            /* keep scanning (last wins) */
        }
    }
    fclose(f);
    return *seg_done >= 0;
}

typedef struct {
    struct cdm_download *d;
    int64_t at;
    int failed;
} hls_sink;

static size_t hls_write_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    hls_sink *s = (hls_sink *)userp;
    struct cdm_download *d = s->d;
    size_t len = size * nmemb;
    if (len == 0) return 0;
    if (d->cancel) return 0;
    if (cdm_file_write_at(d->file, ptr, len, s->at) != (int64_t)len) {
        s->failed = 1;
        return 0;
    }
    s->at += (int64_t)len;
    cdm_mutex_lock(d->lock);
    d->downloaded += (int64_t)len;
    cdm_mutex_unlock(d->lock);
    cdm_speed_update(d);
    cdm_emit_progress(d);
    return len;
}

cdm_status cdm_hls_run(struct cdm_download *d) {
    const char *start_url = d->probe.effective_url[0] ? d->probe.effective_url
                                                      : d->url;
    hlsdoc pl = {0, 0};
    char media_url[2048] = {0};
    hlsdoc media = {0, 0};
    char map_rel[2048] = {0};
    static char seg_rel[4096][2048];
    char seg_url[2048];
    int nsegs = 0, start = 0, i;
    CURL *easy = NULL;
    cdm_status st = CDM_ERR_NET;

    /* derived output "stream.m3u8" would be a lie: store TS instead */
    if ((!d->cfg.output_path || !*d->cfg.output_path) && d->output_path) {
        size_t n = strlen(d->output_path);
        if ((n > 5 && hls_ncasecmp(d->output_path + n - 5, ".m3u8", 5) == 0) ||
            (n > 4 && hls_ncasecmp(d->output_path + n - 4, ".m3u", 4) == 0)) {
            char *dot = strrchr(d->output_path, '.');
            char *sl = strrchr(d->output_path, '/');
#ifdef _WIN32
            {
                char *b = strrchr(d->output_path, '\\');
                if (b && (!sl || b > sl)) sl = b;
            }
#endif
            if (dot && (!sl || dot > sl)) {
                strcpy(dot, ".ts");
                free(d->part_path);
                free(d->meta_path);
                d->part_path = NULL;
                d->meta_path = NULL;
                {
                    size_t a = strlen(d->output_path) + 6;
                    size_t b2 = strlen(d->output_path) + 5;
                    d->part_path = (char *)malloc(a);
                    d->meta_path = (char *)malloc(b2);
                    if (!d->part_path || !d->meta_path) return CDM_ERR_NOMEM;
                    snprintf(d->part_path, a, "%s.part", d->output_path);
                    snprintf(d->meta_path, b2, "%s.cdm", d->output_path);
                }
            }
        }
    }

    if (hls_fetch(start_url, &d->cfg, &pl) != 0) return CDM_ERR_NET;
    /* master? pick best variant, else the doc itself is the media list */
    {
        char variant[2048] = {0};
        if (cdm_hls_pick_variant(pl.data ? pl.data : "", variant, sizeof(variant))) {
            cdm_hls_resolve(start_url, variant, media_url, sizeof(media_url));
        } else {
            snprintf(media_url, sizeof(media_url), "%s", start_url);
        }
    }
    free(pl.data);
    pl.data = NULL;
    if (hls_fetch(media_url[0] ? media_url : start_url, &d->cfg, &media) != 0)
        return CDM_ERR_NET;
    nsegs = cdm_hls_segments(media.data ? media.data : "", map_rel,
                             sizeof(map_rel), seg_rel, 4096);
    if (nsegs < 0) {
        free(media.data);
        return CDM_ERR_HTTP; /* encrypted/unsupported */
    }
    if (nsegs == 0) {
        free(media.data);
        return CDM_ERR_HTTP; /* empty playlist */
    }

    /* resume: continue past stored segment index when the part matches */
    {
        int done = 0;
        if (d->cfg.resume && cdm_file_exists(d->part_path) &&
            cdm_file_exists(d->meta_path) && hls_meta_load(d, &done) && done > 0) {
            int64_t sz;
            cdm_file *chk = cdm_file_open_rw(d->part_path);
            sz = chk ? cdm_file_size(chk) : -1;
            if (chk) cdm_file_close(chk);
            if (sz >= 0 && done <= nsegs) {
                start = done;
                d->resumed = 1;
            } else {
                cdm_remove(d->part_path);
                cdm_meta_delete(d);
            }
        }
    }

    if (start == 0) cdm_remove(d->part_path); /* fresh: drop stale bytes */
    d->file = cdm_file_open_rw(d->part_path);
    if (!d->file) {
        free(media.data);
        return CDM_ERR_IO;
    }
    if (d->cfg.sparse) cdm_file_set_sparse(d->file);

    /* chunk table mirrors segments for progress display */
    d->chunk_count = nsegs + (map_rel[0] ? 1 : 0);
    d->chunks = (cdm_chunk *)calloc((size_t)d->chunk_count, sizeof(cdm_chunk));
    if (!d->chunks) {
        free(media.data);
        cdm_file_close(d->file);
        d->file = NULL;
        return CDM_ERR_NOMEM;
    }
    for (i = 0; i < d->chunk_count; i++) {
        d->chunks[i].offset = i;
        d->chunks[i].length = 1;
        d->chunks[i].done = 0;
        d->chunks[i].state = CHUNK_PENDING;
    }

    d->start_time = cdm_now_seconds();
    d->last_sample_time = d->start_time;
    d->base_bytes = d->downloaded;
    d->last_sample_bytes = d->downloaded;
    d->total_bytes = -1; /* unknown for segmented media */

    easy = curl_easy_init();
    if (!easy) {
        free(media.data);
        cdm_file_close(d->file);
        d->file = NULL;
        return CDM_ERR_INTERNAL;
    }
    {
        struct curl_slist *hs = NULL;
        hls_sink sink;
        int64_t at;
        cdm_file *szf;
        cdm_apply_conn_opts(easy, &d->cfg, &hs);
        if (hs) curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hs);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 90L);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, hls_write_cb);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
        sink.d = d;
        sink.failed = 0;
        /* resume point = current part size */
        szf = d->file;
        at = cdm_file_size(szf);
        if (at < 0) at = 0;
        if (start > 0) d->downloaded = at;
        d->base_bytes = d->downloaded;
        d->last_sample_bytes = d->downloaded;

        /* init segment first (fMP4 streams need it) */
        if (map_rel[0] && start == 0) {
            long code = 0;
            cdm_hls_resolve(media_url[0] ? media_url : start_url, map_rel,
                            seg_url, sizeof(seg_url));
            sink.at = at;
            sink.failed = 0;
            curl_easy_setopt(easy, CURLOPT_URL, seg_url);
            if (curl_easy_perform(easy) != CURLE_OK || sink.failed) goto fetch_fail;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            if (code < 200 || code >= 300) goto fetch_fail;
            at = sink.at;
            d->downloaded = at;
            d->chunks[0].done = 1;
            d->chunks[0].state = CHUNK_DONE;
            hls_meta_save(d, 0, nsegs);
        } else if (map_rel[0]) {
            d->chunks[0].done = 1;
            d->chunks[0].state = CHUNK_DONE;
        }

        for (i = start; i < nsegs; i++) {
            long code = 0;
            int ci = i + (map_rel[0] ? 1 : 0);
            if (d->cancel) {
                st = CDM_ERR_CANCELED;
                goto done;
            }
            cdm_hls_resolve(media_url[0] ? media_url : start_url, seg_rel[i],
                            seg_url, sizeof(seg_url));
            sink.at = at;
            sink.failed = 0;
            curl_easy_setopt(easy, CURLOPT_URL, seg_url);
            if (curl_easy_perform(easy) != CURLE_OK || sink.failed) goto fetch_fail;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
            if (code < 200 || code >= 300) goto fetch_fail;
            at = sink.at;
            d->downloaded = at;
            if (ci < d->chunk_count) {
                d->chunks[ci].done = 1;
                d->chunks[ci].state = CHUNK_DONE;
            }
            hls_meta_save(d, i + 1, nsegs);
            cdm_speed_update(d);
            cdm_emit_progress(d);
        }
        st = CDM_OK;
        goto done;
fetch_fail:
        if (d->cancel)
            st = CDM_ERR_CANCELED;
        else
            st = CDM_ERR_NET;
done:
        if (hs) curl_slist_free_all(hs);
    }
    curl_easy_cleanup(easy);
    free(media.data);
    if (st == CDM_OK && d->cfg.preserve_time && d->probe.last_modified[0]) {
        int64_t mtime = cdm_parse_http_date(d->probe.last_modified);
        if (mtime >= 0) cdm_file_set_mtime(d->file, mtime);
    }
    cdm_file_close(d->file);
    d->file = NULL;
    cdm_speed_finalize(d);
    cdm_emit_progress(d);

    if (st != CDM_OK) {
        hls_meta_save(d, start, nsegs);
        return st;
    }

    /* checksum gate + promote .part -> final (shared tail in run()) */
    d->total_bytes = d->downloaded;
    return CDM_OK;
}
