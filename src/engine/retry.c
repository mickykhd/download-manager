#include "internal.h"

#include <stdlib.h>

/*
 * Classify failures: permanent errors fail fast, transient ones are retried
 * with exponential backoff + jitter.
 */
int cdm_error_is_permanent(CURLcode code, long http_status) {
    switch (http_status) {
        case 400: case 401: case 403: case 404:
        case 405: case 410: case 451:
            return 1;
        case 416: /* range not satisfiable => our state is wrong, don't loop */
            return 1;
        default: break;
    }

    switch (code) {
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_LOGIN_DENIED:
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_WRITE_ERROR: /* our own I/O or cancel */
            return 1;
        default:
            return 0;
    }
}

/* Exponential backoff (base 0.5s, cap ~30s) with +-25% jitter. */
double cdm_backoff_seconds(int attempt) {
    double base = 0.5 * (double)(1 << (attempt > 6 ? 6 : attempt));
    if (base > 30.0) base = 30.0;
    double jitter = ((double)rand() / (double)RAND_MAX - 0.5) * 0.5; /* +-25% */
    double v = base * (1.0 + jitter);
    return v < 0.1 ? 0.1 : v;
}
