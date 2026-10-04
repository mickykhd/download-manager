#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Sidecar resume format (text, line-oriented) written next to the .part file:
 *
 *   CDM1
 *   size <total>
 *   etag <validator>            (optional)
 *   lastmod <http-date>         (optional)
 *   chunks <n>
 *   c <offset> <length> <done>  (repeated n times)
 */

#define CDM_META_MAGIC "CDM1"

int cdm_meta_save(cdm_download *d) {
    if (!d->meta_path) return -1;
    FILE *f = fopen(d->meta_path, "wb");
    if (!f) return -1;

    fprintf(f, "%s\n", CDM_META_MAGIC);
    fprintf(f, "size %lld\n", (long long)d->total_bytes);
    if (d->probe.etag[0])          fprintf(f, "etag %s\n", d->probe.etag);
    if (d->probe.last_modified[0]) fprintf(f, "lastmod %s\n", d->probe.last_modified);
    fprintf(f, "chunks %d\n", d->chunk_count);

    cdm_mutex_lock(d->lock);
    for (int i = 0; i < d->chunk_count; i++) {
        cdm_chunk *c = &d->chunks[i];
        fprintf(f, "c %lld %lld %lld\n",
                (long long)c->offset, (long long)c->length, (long long)c->done);
    }
    cdm_mutex_unlock(d->lock);

    fflush(f);
    fclose(f);
    return 0;
}

int cdm_meta_load(cdm_download *d) {
    if (!d->meta_path) return 0;
    FILE *f = fopen(d->meta_path, "rb");
    if (!f) return 0;

    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    if (strncmp(line, CDM_META_MAGIC, 4) != 0) { fclose(f); return 0; }

    int64_t size = -1;
    char etag[256] = {0};
    char lastmod[128] = {0};
    int n = 0;
    cdm_chunk *chunks = NULL;
    int idx = 0;
    int ok = 1;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "size ", 5) == 0) {
            size = (int64_t)strtoll(line + 5, NULL, 10);
        } else if (strncmp(line, "etag ", 5) == 0) {
            snprintf(etag, sizeof(etag), "%.255s", line + 5);
            etag[strcspn(etag, "\r\n")] = 0;
        } else if (strncmp(line, "lastmod ", 8) == 0) {
            snprintf(lastmod, sizeof(lastmod), "%.127s", line + 8);
            lastmod[strcspn(lastmod, "\r\n")] = 0;
        } else if (strncmp(line, "chunks ", 7) == 0) {
            n = atoi(line + 7);
            if (n <= 0 || n > 4096) { ok = 0; break; }
            chunks = calloc((size_t)n, sizeof(cdm_chunk));
            if (!chunks) { ok = 0; break; }
        } else if (line[0] == 'c' && line[1] == ' ') {
            if (!chunks || idx >= n) { ok = 0; break; }
            long long off, len, done;
            if (sscanf(line + 2, "%lld %lld %lld", &off, &len, &done) != 3) {
                ok = 0; break;
            }
            chunks[idx].offset = off;
            chunks[idx].length = len;
            chunks[idx].done = done;
            chunks[idx].state = (done >= len) ? CHUNK_DONE : CHUNK_PENDING;
            idx++;
        }
    }
    fclose(f);

    if (!ok || !chunks || idx != n || size < 0) {
        free(chunks);
        return 0;
    }

    /* Validate against current probe: size must match, and if we have a
     * validator on both sides they must be equal (else remote changed). */
    if (d->probe.size >= 0 && d->probe.size != size) { free(chunks); return 0; }
    if (d->probe.etag[0] && etag[0] && strcmp(d->probe.etag, etag) != 0) {
        free(chunks); return 0;
    }
    if (!d->probe.etag[0] && d->probe.last_modified[0] && lastmod[0] &&
        strcmp(d->probe.last_modified, lastmod) != 0) {
        free(chunks); return 0;
    }

    /* Adopt persisted chunk layout and recompute downloaded total. */
    free(d->chunks);
    d->chunks = chunks;
    d->chunk_count = n;
    d->total_bytes = size;

    int64_t done_total = 0;
    for (int i = 0; i < n; i++) done_total += d->chunks[i].done;
    d->downloaded = done_total;
    d->resumed = 1;
    return 1;
}

void cdm_meta_delete(cdm_download *d) {
    if (d->meta_path) cdm_remove(d->meta_path);
}
