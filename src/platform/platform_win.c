#if defined(_WIN32)

#include "cdm/platform.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

struct cdm_thread { HANDLE h; cdm_thread_fn fn; void *arg; void *ret; };
struct cdm_mutex { CRITICAL_SECTION cs; };
struct cdm_file  { HANDLE h; CRITICAL_SECTION cs; }; /* seek+write serialized */

static DWORD WINAPI trampoline(LPVOID p) {
    cdm_thread *t = (cdm_thread *)p;
    t->ret = t->fn(t->arg);
    return 0;
}

cdm_thread *cdm_thread_start(cdm_thread_fn fn, void *arg) {
    cdm_thread *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, trampoline, t, 0, NULL);
    if (!t->h) { free(t); return NULL; }
    return t;
}

void cdm_thread_join(cdm_thread *t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

struct cdm_detached_arg { cdm_thread_fn fn; void *arg; };

static DWORD WINAPI detached_trampoline(LPVOID p) {
    struct cdm_detached_arg *da = (struct cdm_detached_arg *)p;
    cdm_thread_fn fn = da->fn;
    void *arg = da->arg;
    free(da);
    fn(arg);
    return 0;
}

int cdm_thread_spawn_detached(cdm_thread_fn fn, void *arg) {
    if (!fn) return -1;
    struct cdm_detached_arg *da = (struct cdm_detached_arg *)calloc(1, sizeof(*da));
    if (!da) return -1;
    da->fn = fn;
    da->arg = arg;
    HANDLE h = CreateThread(NULL, 0, detached_trampoline, da, 0, NULL);
    if (!h) { free(da); return -1; }
    CloseHandle(h); /* detached: no join; thread frees its own args */
    return 0;
}

cdm_mutex *cdm_mutex_create(void) {
    cdm_mutex *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void cdm_mutex_destroy(cdm_mutex *m) {
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void cdm_mutex_lock(cdm_mutex *m)   { if (m) EnterCriticalSection(&m->cs); }
void cdm_mutex_unlock(cdm_mutex *m) { if (m) LeaveCriticalSection(&m->cs); }

double cdm_now_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

void cdm_sleep_ms(unsigned ms) { Sleep(ms); }

cdm_file *cdm_file_open_rw(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    cdm_file *f = calloc(1, sizeof(*f));
    if (!f) { CloseHandle(h); return NULL; }
    f->h = h;
    InitializeCriticalSection(&f->cs);
    return f;
}

int cdm_file_preallocate(cdm_file *f, int64_t size) {
    if (!f || size < 0) return -1;
    LARGE_INTEGER li; li.QuadPart = size;
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) return -1;
    if (!SetEndOfFile(f->h)) return -1;
    return 0;
}

int64_t cdm_file_write_at(cdm_file *f, const void *buf, size_t len, int64_t off) {
    if (!f) return -1;
    EnterCriticalSection(&f->cs);
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) {
        LeaveCriticalSection(&f->cs);
        return -1;
    }
    const char *p = (const char *)buf;
    DWORD left = (DWORD)len, wrote = 0;
    while (left > 0) {
        DWORD w = 0;
        if (!WriteFile(f->h, p + wrote, left, &w, NULL)) {
            LeaveCriticalSection(&f->cs);
            return -1;
        }
        left -= w; wrote += w;
    }
    LeaveCriticalSection(&f->cs);
    return (int64_t)len;
}

int64_t cdm_file_size(cdm_file *f) {
    if (!f) return -1;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(f->h, &li)) return -1;
    return (int64_t)li.QuadPart;
}

void cdm_file_close(cdm_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    DeleteCriticalSection(&f->cs);
    free(f);
}

int cdm_file_exists(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int cdm_rename(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return 0;
    return -1;
}

int cdm_remove(const char *path) { return DeleteFileA(path) ? 0 : -1; }

#endif /* _WIN32 */
