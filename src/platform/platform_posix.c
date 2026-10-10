#if !defined(_WIN32)

#define _GNU_SOURCE
#include "cdm/platform.h"

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

struct cdm_thread { pthread_t th; cdm_thread_fn fn; void *arg; void *ret; };
struct cdm_mutex { pthread_mutex_t m; };
struct cdm_file  { int fd; };

static void *trampoline(void *p) {
    cdm_thread *t = (cdm_thread *)p;
    t->ret = t->fn(t->arg);
    return t->ret;
}

cdm_thread *cdm_thread_start(cdm_thread_fn fn, void *arg) {
    cdm_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    if (pthread_create(&t->th, NULL, trampoline, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

void cdm_thread_join(cdm_thread *t) {
    if (!t) return;
    pthread_join(t->th, NULL);
    free(t);
}

struct cdm_detached_arg { cdm_thread_fn fn; void *arg; };

static void *detached_trampoline(void *p) {
    struct cdm_detached_arg *da = (struct cdm_detached_arg *)p;
    cdm_thread_fn fn = da->fn;
    void *arg = da->arg;
    free(da);
    fn(arg);
    return NULL;
}

int cdm_thread_spawn_detached(cdm_thread_fn fn, void *arg) {
    if (!fn) return -1;
    struct cdm_detached_arg *da = malloc(sizeof(*da));
    if (!da) return -1;
    da->fn = fn;
    da->arg = arg;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, detached_trampoline, da);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(da); return -1; }
    return 0;
}

cdm_mutex *cdm_mutex_create(void) {
    cdm_mutex *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->m, NULL) != 0) { free(m); return NULL; }
    return m;
}

void cdm_mutex_destroy(cdm_mutex *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void cdm_mutex_lock(cdm_mutex *m)   { if (m) pthread_mutex_lock(&m->m); }
void cdm_mutex_unlock(cdm_mutex *m) { if (m) pthread_mutex_unlock(&m->m); }

double cdm_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void cdm_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

cdm_file *cdm_file_open_rw(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;
    cdm_file *f = calloc(1, sizeof(*f));
    if (!f) { close(fd); return NULL; }
    f->fd = fd;
    return f;
}

int cdm_file_preallocate(cdm_file *f, int64_t size) {
    if (!f || size < 0) return -1;
#if defined(__linux__)
    if (posix_fallocate(f->fd, 0, (off_t)size) == 0) return 0;
    /* fall through to ftruncate on filesystems that don't support it */
#endif
    return ftruncate(f->fd, (off_t)size) == 0 ? 0 : -1;
}

int64_t cdm_file_write_at(cdm_file *f, const void *buf, size_t len, int64_t off) {
    if (!f) return -1;
    const char *p = (const char *)buf;
    size_t left = len;
    int64_t at = off;
    while (left > 0) {
        ssize_t w = pwrite(f->fd, p, left, (off_t)at);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)w;
        p += w;
        at += w;
    }
    return (int64_t)len;
}

int cdm_file_set_sparse(cdm_file *f) {
    /* POSIX preallocation via ftruncate is already sparse-friendly;
     * filesystems that support it need no flag. */
    (void)f;
    return 0;
}

int cdm_file_set_mtime(cdm_file *f, int64_t unix_seconds) {
    struct timespec ts[2];
    if (!f || unix_seconds < 0) return -1;
    ts[0].tv_sec = 0;
    ts[0].tv_nsec = UTIME_OMIT;
    ts[1].tv_sec = (time_t)unix_seconds;
    ts[1].tv_nsec = 0;
    if (futimens(f->fd, ts) != 0) return -1;
    return 0;
}

int64_t cdm_file_size(cdm_file *f) {
    if (!f) return -1;
    struct stat st;
    if (fstat(f->fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

void cdm_file_close(cdm_file *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int cdm_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int cdm_rename(const char *from, const char *to) { return rename(from, to); }
int cdm_remove(const char *path) { return remove(path); }

#endif /* !_WIN32 */
