#ifndef CDM_UPDATER_H
#define CDM_UPDATER_H

/* Async update checker: compares the running build against the latest
 * GitHub release tag. All functions are thread-safe; the check runs on
 * a detached worker thread and the UI polls the result. */

/* Numeric dotted-version compare: -1 (a<b), 0, 1 (a>b). Leading 'v'
 * and pre-release suffixes (-xxx) are ignored. */
int cdm_vercmp(const char *a, const char *b);

/* Start a check for `repo` ("owner/name"). No-op while one is running. */
void cdm_update_check_async(const char *repo);

/* Poll: 0 = no result yet, 1 = up to date, 2 = update available,
 * -1 = check failed. When 2, out_tag holds the newest tag (e.g. v0.3.0). */
int cdm_update_poll(char *out_tag, unsigned cap);

#endif /* CDM_UPDATER_H */
