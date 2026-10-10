#include "internal.h"

#include <stdio.h>
#include <string.h>

/*
 * Write callback: data for the transfer's current chunk is written at its
 * absolute file offset (offset + done). Works for both segmented (206) and
 * single-stream (200) transfers.
 */
size_t cdm_write_cb(char *ptr, size_t size, size_t nmemb, void *userp) {
    cdm_transfer *t = (cdm_transfer *)userp;
    cdm_download *d = t->d;
    size_t len = size * nmemb;
    if (len == 0) return 0;
    if (d->cancel) return 0; /* abort transfer */

    cdm_chunk *ch = t->chunk;
    if (!ch) return 0;

    /* On first byte, confirm the server honoured our range. If it replied 200
     * for a chunk that does not start at offset 0, the ranges were ignored /
     * the resource changed: writing here would corrupt the file. Abort. */
    if (!t->checked) {
        t->checked = 1;
        if (!d->single_stream) {
            long code = 0;
            curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code);
            if (code == 200 && ch->offset != 0) {
                d->remote_changed = 1;
                return 0; /* triggers CURLE_WRITE_ERROR */
            }
        }
    }

    int64_t at = ch->offset + ch->done;

    /* Guard against a server that ignores range and streams the whole file
     * into a chunk sized smaller than the payload. */
    int64_t writable = (int64_t)len;
    if (!d->single_stream && ch->done + writable > ch->length)
        writable = ch->length - ch->done;
    if (writable <= 0) return len; /* swallow extra */

    if (cdm_file_write_at(d->file, ptr, (size_t)writable, at) != writable)
        return 0; /* I/O error aborts */

    cdm_mutex_lock(d->lock);
    ch->done += writable;
    d->downloaded += writable;
    cdm_mutex_unlock(d->lock);

    return len;
}

/* Set per-request options for the transfer's assigned chunk. */
void cdm_transfer_config(cdm_transfer *t) {
    cdm_download *d = t->d;
    CURL *c = t->easy;
    cdm_chunk *ch = t->chunk;

    curl_easy_reset(c);
    if (t->headers) { curl_slist_free_all(t->headers); t->headers = NULL; }
    curl_easy_setopt(c, CURLOPT_URL,
                     d->probe.effective_url[0] ? d->probe.effective_url : d->url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    cdm_apply_conn_opts(c, &d->cfg, &t->headers);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, ""); /* identity */
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cdm_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(c, CURLOPT_PRIVATE, t);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, t->errbuf);
    t->errbuf[0] = 0;
    t->checked = 0;

    /* Aggregate throttle split across the target connection count. */
    if (d->cfg.max_speed_bps > 0) {
        int conns = d->target_conn > 0 ? d->target_conn : 1;
        curl_off_t per = (curl_off_t)(d->cfg.max_speed_bps / conns);
        if (per < 1) per = 1;
        curl_easy_setopt(c, CURLOPT_MAX_RECV_SPEED_LARGE, per);
    }

    if (!d->single_stream) {
        char range[64];
        int64_t from = ch->offset + ch->done;
        int64_t to = ch->offset + ch->length - 1;
        snprintf(range, sizeof(range), "%lld-%lld",
                 (long long)from, (long long)to);
        curl_easy_setopt(c, CURLOPT_RANGE, range);

        /* Safe resume: only accept partial content if the resource is
         * unchanged; otherwise server returns 200 (full) and we detect it. */
        if (d->probe.etag[0]) {
            char ifr[300];
            snprintf(ifr, sizeof(ifr), "If-Range: %s", d->probe.etag);
            t->headers = curl_slist_append(t->headers, ifr);
        }
        if (t->headers)
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, t->headers);
    } else if (ch->done > 0) {
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)ch->done);
    }
}

/* Find the next chunk needing work (work-stealing). Caller must hold lock. */
cdm_chunk *cdm_next_pending_chunk(cdm_download *d) {
    for (int i = 0; i < d->chunk_count; i++) {
        cdm_chunk *ch = &d->chunks[i];
        if (ch->state == CHUNK_PENDING && ch->done < ch->length) {
            ch->state = CHUNK_ACTIVE;
            return ch;
        }
    }
    return NULL;
}
