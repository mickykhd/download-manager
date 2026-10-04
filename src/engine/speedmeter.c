#include "internal.h"

/* Exponentially-smoothed throughput + ETA, sampled roughly per call. */
void cdm_speed_update(cdm_download *d) {
    double now = cdm_now_seconds();
    double dt = now - d->last_sample_time;
    if (dt < 0.25) return; /* sample at most ~4x/sec */

    cdm_mutex_lock(d->lock);
    int64_t cur = d->downloaded;
    cdm_mutex_unlock(d->lock);

    int64_t delta = cur - d->last_sample_bytes;
    double inst = dt > 0 ? (double)delta / dt : 0.0;

    const double alpha = 0.3;
    if (d->speed_bps <= 0) d->speed_bps = inst;
    else d->speed_bps = alpha * inst + (1.0 - alpha) * d->speed_bps;

    d->last_sample_time = now;
    d->last_sample_bytes = cur;
}

void cdm_speed_finalize(struct cdm_download *d) {
    /* Fold this run's overall average into the smoothed speed so short
     * downloads (faster than the 0.25 s sample window) still report a
     * meaningful speed instead of 0. Resume-aware: only bytes gained
     * since transfer start count. */
    double now = cdm_now_seconds();
    double elapsed = now - d->start_time;
    if (elapsed < 1e-3) return;

    cdm_mutex_lock(d->lock);
    int64_t cur = d->downloaded;
    cdm_mutex_unlock(d->lock);

    int64_t gained = cur - d->base_bytes;
    if (gained <= 0) return;
    double avg = (double)gained / elapsed;

    const double alpha = 0.3;
    if (d->speed_bps <= 0) d->speed_bps = avg;
    else d->speed_bps = alpha * avg + (1.0 - alpha) * d->speed_bps;

    d->last_sample_time = now;
    d->last_sample_bytes = cur;
}

void cdm_emit_progress(struct cdm_download *d) {
    if (!d->progress_cb) return;

    cdm_mutex_lock(d->lock);
    int64_t cur = d->downloaded;
    int completed = 0;
    for (int i = 0; i < d->chunk_count; i++)
        if (d->chunks[i].state == CHUNK_DONE) completed++;
    cdm_mutex_unlock(d->lock);

    cdm_progress p;
    p.total_bytes = d->total_bytes;
    p.downloaded_bytes = cur;
    /* If the smoother has no sample yet (fast download), fall back to this
     * run's overall average so speed/ETA are useful from the first emit. */
    double speed = d->speed_bps;
    if (speed <= 1.0 && cur > d->base_bytes) {
        double elapsed = cdm_now_seconds() - d->start_time;
        if (elapsed > 1e-3) speed = (double)(cur - d->base_bytes) / elapsed;
    }
    p.speed_bps = speed;
    p.active_connections = d->active_transfers;
    p.total_chunks = d->chunk_count;
    p.completed_chunks = completed;
    p.resumed = d->resumed;

    if (d->total_bytes > 0 && speed > 1.0) {
        int64_t remain = d->total_bytes - cur;
        p.eta_seconds = remain > 0 ? (double)remain / speed : 0.0;
    } else {
        p.eta_seconds = -1.0;
    }

    d->progress_cb(&p, d->progress_user);
}
