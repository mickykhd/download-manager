#define _GNU_SOURCE
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <ctype.h>

/* Portable case-insensitive compare (strncasecmp is POSIX-only, missing on MSVC). */
static int cdm_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

/* Collected header values during probe. */
typedef struct {
    cdm_probe *p;
    int64_t content_range_total; /* full size parsed from Content-Range */
} probe_ctx;

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\r' || s[n-1] == '\n' ||
                 s[n-1] == ' '  || s[n-1] == '\t')) s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static int hdr_match(const char *line, const char *name, const char **val) {
    size_t nl = strlen(name);
    if (cdm_strncasecmp(line, name, nl) == 0 && line[nl] == ':') {
        const char *v = line + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        *val = v;
        return 1;
    }
    return 0;
}

/* Portable case-insensitive substring search (strcasestr is GNU-only and
 * missing from MSVC/MinGW headers). */
static const char *cistrstr(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return hay;
    }
    return NULL;
}

/* Extract filename from a Content-Disposition value. */
static void parse_disposition(const char *v, char *out, size_t cap) {
    const char *f = cistrstr(v, "filename");
    if (!f) return;
    f += 8;
    if (*f == '*') { /* filename*=UTF-8''name */
        const char *q = strchr(f, '\'');
        if (q) { q = strchr(q + 1, '\''); if (q) f = q + 1; }
    } else {
        const char *eq = strchr(f, '=');
        if (!eq) return;
        f = eq + 1;
    }
    while (*f == ' ' || *f == '"') f++;
    size_t i = 0;
    while (*f && *f != '"' && *f != ';' && *f != '\r' && *f != '\n' && i + 1 < cap)
        out[i++] = *f++;
    out[i] = 0;
}

static size_t header_cb(char *buf, size_t size, size_t nitems, void *userp) {
    probe_ctx *ctx = (probe_ctx *)userp;
    size_t len = size * nitems;
    char line[2048];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, buf, n);
    line[n] = 0;

    const char *v;
    if (hdr_match(line, "Accept-Ranges", &v)) {
        if (cdm_strncasecmp(v, "bytes", 5) == 0) ctx->p->accept_ranges = 1;
    } else if (hdr_match(line, "ETag", &v)) {
        /* Only keep strong validators (reject weak W/ prefixed). */
        if (!(v[0] == 'W' && v[1] == '/')) {
            snprintf(ctx->p->etag, sizeof(ctx->p->etag), "%.255s", v);
            trim(ctx->p->etag);
        }
    } else if (hdr_match(line, "Last-Modified", &v)) {
        snprintf(ctx->p->last_modified, sizeof(ctx->p->last_modified), "%.127s", v);
        trim(ctx->p->last_modified);
    } else if (hdr_match(line, "Content-Disposition", &v)) {
        parse_disposition(v, ctx->p->filename, sizeof(ctx->p->filename));
    } else if (hdr_match(line, "Content-Range", &v)) {
        /* Format: "bytes 0-0/12345" -> capture total after '/'. */
        const char *slash = strchr(v, '/');
        if (slash && slash[1] && slash[1] != '*') {
            long long t = strtoll(slash + 1, NULL, 10);
            if (t > 0) ctx->content_range_total = (int64_t)t;
        }
        ctx->p->accept_ranges = 1;
    }
    return len;
}

/* discard body */
static size_t sink_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    (void)ptr; (void)userp;
    return size * nmemb;
}

static void derive_name_from_url(const char *url, char *out, size_t cap) {
    const char *q = strpbrk(url, "?#");
    const char *end = q ? q : url + strlen(url);
    const char *slash = end;
    while (slash > url && *(slash - 1) != '/') slash--;
    size_t n = (size_t)(end - slash);
    if (n == 0 || n >= cap) { snprintf(out, cap, "download.bin"); return; }
    memcpy(out, slash, n);
    out[n] = 0;
    /* very light URL-decode of %20 style is skipped for MVP */
    if (out[0] == 0) snprintf(out, cap, "download.bin");
}

/* Extra headers are appended to *slist (may be NULL to skip); the list
 * stays owned by the caller and must outlive curl_easy_perform. */
static void apply_common_opts(CURL *c, const cdm_config *cfg,
                              struct curl_slist **slist) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    cdm_apply_conn_opts(c, cfg, slist);
    if (slist && *slist)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, *slist);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* identity for ranges */
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
}

cdm_status cdm_probe_url(const char *url, const cdm_config *cfg, cdm_probe *out) {
    memset(out, 0, sizeof(*out));
    out->size = -1;

    CURL *c = curl_easy_init();
    if (!c) return CDM_ERR_INTERNAL;

    probe_ctx ctx = { out, -1 };
    struct curl_slist *hdrs = NULL;
    apply_common_opts(c, cfg, &hdrs);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink_cb);

    /* Prefer a ranged GET (bytes=0-0): reveals Content-Range/Accept-Ranges
     * and works where HEAD is refused (405). */
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    if (rc != CURLE_OK) {
        /* Retry once as a plain HEAD in case the ranged GET was rejected. */
        curl_easy_setopt(c, CURLOPT_RANGE, NULL);
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        if (rc != CURLE_OK) {
            if (hdrs) curl_slist_free_all(hdrs);
            curl_easy_cleanup(c);
            return CDM_ERR_NET;
        }
    }

    if (code >= 400) {
        if (hdrs) curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        return CDM_ERR_HTTP;
    }

    /* 206 => ranges definitely supported. */
    if (code == 206) out->accept_ranges = 1;

    if (code == 206 && ctx.content_range_total > 0) {
        /* Best case: full size came straight from Content-Range. */
        out->size = ctx.content_range_total;
    } else if (code == 200) {
        /* Server ignored the range; Content-Length is the full size. */
        curl_off_t total = -1;
        if (curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total)
            == CURLE_OK && total >= 0)
            out->size = (int64_t)total;
        out->accept_ranges = 0; /* it ignored our range */
    } else {
        /* Fallback: HEAD for the full Content-Length. */
        curl_easy_setopt(c, CURLOPT_RANGE, NULL);
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        if (curl_easy_perform(c) == CURLE_OK) {
            curl_off_t full = -1;
            if (curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &full)
                == CURLE_OK && full >= 0)
                out->size = (int64_t)full;
        }
    }

    char *eff = NULL;
    if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff)
        snprintf(out->effective_url, sizeof(out->effective_url), "%.2047s", eff);

    if (out->filename[0] == 0) {
        const char *src = out->effective_url[0] ? out->effective_url : url;
        derive_name_from_url(src, out->filename, sizeof(out->filename));
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return CDM_OK;
}
