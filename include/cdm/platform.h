#ifndef CDM_PLATFORM_H
#define CDM_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

/*
 * Thin cross-platform abstraction over threads, mutexes, and file I/O.
 * POSIX (Linux/macOS) and Win32 backends live in platform_posix.c /
 * platform_win.c. Only one is compiled per target.
 */

#if defined(_WIN32)
#define CDM_OS_WINDOWS 1
#else
#define CDM_OS_POSIX 1
#endif

typedef struct cdm_thread cdm_thread;
typedef struct cdm_mutex cdm_mutex;

typedef void *(*cdm_thread_fn)(void *arg);

/* Threads */
cdm_thread *cdm_thread_start(cdm_thread_fn fn, void *arg);
void cdm_thread_join(cdm_thread *t);
/* Fire-and-forget thread (detached). No join needed; returns 0 on success. */
int cdm_thread_spawn_detached(cdm_thread_fn fn, void *arg);

/* Mutex */
cdm_mutex *cdm_mutex_create(void);
void cdm_mutex_destroy(cdm_mutex *m);
void cdm_mutex_lock(cdm_mutex *m);
void cdm_mutex_unlock(cdm_mutex *m);

/* Time */
double cdm_now_seconds(void);
void cdm_sleep_ms(unsigned ms);

/*
 * Random-access file handle. Segment workers write into distinct byte
 * offsets of the same output file concurrently (pwrite on POSIX,
 * per-write seek+write under a lock on Win32).
 */
typedef struct cdm_file cdm_file;

cdm_file *cdm_file_open_rw(const char *path);       /* create/open for read+write */
int cdm_file_preallocate(cdm_file *f, int64_t size);/* reserve size, 0 on success */
int64_t cdm_file_write_at(cdm_file *f, const void *buf, size_t len, int64_t off);
int64_t cdm_file_size(cdm_file *f);
void cdm_file_close(cdm_file *f);

/* Filesystem helpers */
int cdm_file_exists(const char *path);
int cdm_rename(const char *from, const char *to);
int cdm_remove(const char *path);

#endif /* CDM_PLATFORM_H */
