#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Assign a pending chunk to an idle transfer and add it to the multi stack.
 * Returns 1 if a chunk was started, 0 if none remain. Caller holds no lock. */
static int start_transfer(cdm_download *d, cdm_transfer *t) {
    cdm_mutex_lock(d->lock);
    cdm_chunk *ch = cdm_next_pending_chunk(d);
    cdm_mutex_unlock(d->lock);
    if (!ch) return 0;

    t->chunk = ch;
    t->retries = 0;
    cdm_transfer_config(t);
    curl_multi_add_handle(d->multi, t->easy);
    t->in_multi = 1;
    d->active_transfers++;
    return 1;
}

/* Re-queue a transfer's chunk after a transient failure (keeps progress). */
static void requeue_chunk(cdm_download *d, cdm_transfer *t) {
    cdm_mutex_lock(d->lock);
    if (t->chunk) t->chunk->state = CHUNK_PENDING;
    cdm_mutex_unlock(d->lock);
}

/* Straggler mitigation: when idle capacity exists but nothing is pending,
 * halve the largest in-flight remainder back to PENDING so free transfers
 * can pick it up. Aborting the straggler is safe: fetched bytes stay valid
 * and the fresh ranges continue exactly where each half left off.
 * Single-threaded (same thread drives multi_perform), so no races. */
#define CDM_SPLIT_MIN_REMAINDER (2 * 1024 * 1024)
#define CDM_SPLIT_MAX_CHUNKS 4096

static int steal_remainder(cdm_download *d) {
    int bi = -1;
    int64_t best = 0;
    cdm_chunk *v;
    int64_t rem, half;
    cdm_chunk *grown;
    int i;
    for (i = 0; i < d->transfer_cap; i++) {
        cdm_transfer *t = &d->transfers[i];
        if (!t->in_multi || !t->chunk) continue;
        if (t->chunk->length - t->chunk->done > best) {
            best = t->chunk->length - t->chunk->done;
            bi = i;
        }
    }
    if (bi < 0 || best < CDM_SPLIT_MIN_REMAINDER) return 0;
    if (d->chunk_count >= CDM_SPLIT_MAX_CHUNKS) return 0;
    if (!d->chunks) return 0;

    /* victim index before any reallocation */
    {
        ptrdiff_t vi = d->transfers[bi].chunk - d->chunks;
        if (vi < 0 || vi >= d->chunk_count) return 0;
        /* abort the straggler's request; its fetched prefix stays valid */
        curl_multi_remove_handle(d->multi, d->transfers[bi].easy);
        d->transfers[bi].in_multi = 0;
        d->active_transfers--;
        d->transfers[bi].chunk = NULL;

        /* grow the chunk table (transfers hold pointers: rebase by index) */
        grown = (cdm_chunk *)realloc(d->chunks,
                                     (size_t)(d->chunk_count + 1) * sizeof(cdm_chunk));
        if (!grown) return 0;
        {
            cdm_chunk *old = d->chunks;
            for (i = 0; i < d->transfer_cap; i++) {
                cdm_transfer *t = &d->transfers[i];
                if (t->chunk)
                    t->chunk = grown + (t->chunk - old);
            }
            d->chunks = grown;
        }
        v = &d->chunks[vi];
    }
    rem = v->length - v->done;
    if (rem < CDM_SPLIT_MIN_REMAINDER) return 0;
    half = rem / 2;
    {
        cdm_chunk *second = &d->chunks[d->chunk_count];
        second->offset = v->offset + v->done + half;
        second->length = v->length - v->done - half;
        second->done = 0;
        second->state = CHUNK_PENDING;
        v->length = v->done + half;
        v->state = CHUNK_PENDING;
        d->chunk_count++;
    }
    return 1;
}

/* Adaptive concurrency: periodically add a connection while throughput keeps
 * improving; the whole loop already backs off on failures via retries. */
static void adapt(cdm_download *d) {
    if (!d->cfg.adaptive || d->single_stream) return;
    double now = cdm_now_seconds();
    if (now - d->adapt_last_time < 2.0) return;

    double dt = now - d->adapt_last_time;
    int64_t bytes = d->downloaded - d->adapt_last_bytes;
    double speed = dt > 0 ? (double)bytes / dt : 0.0;

    if (d->adapt_last_speed > 0 && d->target_conn < d->cfg.max_connections) {
        if (speed > d->adapt_last_speed * 1.15) {
            d->target_conn++; /* keep scaling up: last bump helped */
        }
    } else if (d->target_conn < d->cfg.max_connections &&
               d->adapt_last_speed == 0) {
        d->target_conn++; /* initial ramp */
    }

    d->adapt_last_time = now;
    d->adapt_last_bytes = d->downloaded;
    d->adapt_last_speed = speed;

    /* idle capacity with nothing pending: split the slowest straggler so
     * free transfers have work ( placement below picks the halves up ) */
    if (!d->single_stream && d->active_transfers < d->target_conn) {
        int pending = 0;
        int i;
        cdm_mutex_lock(d->lock);
        for (i = 0; i < d->chunk_count; i++) {
            if (d->chunks[i].state == CHUNK_PENDING) {
                pending = 1;
                break;
            }
        }
        cdm_mutex_unlock(d->lock);
        if (!pending) steal_remainder(d);
    }
}

cdm_status cdm_run_transfers(cdm_download *d) {
    d->multi = curl_multi_init();
    if (!d->multi) return CDM_ERR_INTERNAL;
    curl_multi_setopt(d->multi, CURLMOPT_MAXCONNECTS, (long)d->cfg.max_connections);
    curl_multi_setopt(d->multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    d->start_time = cdm_now_seconds();
    d->last_sample_time = d->start_time;
    d->base_bytes = d->downloaded;        /* resume-aware baseline */
    d->last_sample_bytes = d->downloaded; /* else 1st delta counts prior runs */
    d->adapt_last_time = d->start_time;

    /* Prime initial transfers up to target_conn. */
    int spawned = 0;
    for (int i = 0; i < d->transfer_cap && spawned < d->target_conn; i++) {
        if (!start_transfer(d, &d->transfers[i])) break;
        spawned++;
        cdm_sleep_ms(100); /* stagger spawns to avoid a SYN burst */
    }

    cdm_status result = CDM_OK;
    int still_running = 0;

    do {
        if (d->cancel) { result = CDM_ERR_CANCELED; break; }

        CURLMcode mc = curl_multi_perform(d->multi, &still_running);
        if (mc != CURLM_OK) { result = CDM_ERR_NET; break; }

        /* Drain completion messages. */
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(d->multi, &left)) != NULL) {
            if (msg->msg != CURLMSG_DONE) continue;

            CURL *e = msg->easy_handle;
            cdm_transfer *t = NULL;
            curl_easy_getinfo(e, CURLINFO_PRIVATE, &t);
            CURLcode code = msg->data.result;

            long http = 0;
            curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http);

            curl_multi_remove_handle(d->multi, e);
            t->in_multi = 0;
            d->active_transfers--;

            cdm_chunk *ch = t->chunk;
            int chunk_complete = ch && (ch->done >= ch->length);

            if (d->remote_changed) { result = CDM_ERR_CHANGED; goto done; }

            if (code == CURLE_OK && (chunk_complete || d->single_stream)) {
                cdm_mutex_lock(d->lock);
                if (ch) ch->state = CHUNK_DONE;
                cdm_mutex_unlock(d->lock);
                t->chunk = NULL;
                cdm_meta_save(d);
                /* Steal the next pending chunk, or retire this transfer. */
                start_transfer(d, t);
            } else if (code == CURLE_OK && !chunk_complete) {
                /* Connection closed early with bytes still owed; continue
                 * the same chunk from where we left off. */
                requeue_chunk(d, t);
                t->chunk = NULL;
                start_transfer(d, t);
            } else {
                /* Failure path. */
                if (cdm_error_is_permanent(code, http)) {
                    result = (http >= 400) ? CDM_ERR_HTTP : CDM_ERR_NET;
                    goto done;
                }
                if (t->retries >= d->cfg.max_retries) {
                    result = CDM_ERR_NET;
                    goto done;
                }
                double wait = cdm_backoff_seconds(t->retries);
                t->retries++;
                cdm_sleep_ms((unsigned)(wait * 1000));
                /* keep chunk & its progress, re-issue */
                cdm_transfer_config(t);
                curl_multi_add_handle(d->multi, t->easy);
                t->in_multi = 1;
                d->active_transfers++;
            }
        }

        /* Scale up if adaptive and throughput is improving. */
        adapt(d);
        while (d->active_transfers < d->target_conn) {
            int placed = 0;
            for (int i = 0; i < d->transfer_cap; i++) {
                if (!d->transfers[i].in_multi && !d->transfers[i].chunk) {
                    if (start_transfer(d, &d->transfers[i])) { placed = 1; break; }
                }
            }
            if (!placed) break;
        }

        cdm_speed_update(d);
        cdm_emit_progress(d);

        if (still_running || d->active_transfers > 0) {
            curl_multi_poll(d->multi, NULL, 0, 200, NULL);
        }

    } while (still_running || d->active_transfers > 0);

    /* Verify every chunk finished. */
    if (result == CDM_OK) {
        for (int i = 0; i < d->chunk_count; i++) {
            if (d->chunks[i].done < d->chunks[i].length) {
                result = CDM_ERR_NET;
                break;
            }
        }
    }

done:
    /* Tear down any handles still in the stack. */
    for (int i = 0; i < d->transfer_cap; i++) {
        if (d->transfers[i].in_multi) {
            curl_multi_remove_handle(d->multi, d->transfers[i].easy);
            d->transfers[i].in_multi = 0;
        }
    }
    curl_multi_cleanup(d->multi);
    d->multi = NULL;
    cdm_speed_finalize(d);
    cdm_emit_progress(d);
    return result;
}
