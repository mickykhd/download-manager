#include "cdm/download.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static cdm_download *g_active = NULL;

static void on_sigint(int sig) {
    (void)sig;
    if (g_active) cdm_download_cancel(g_active);
}

static void human_bytes(double b, char *out, size_t cap) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    snprintf(out, cap, "%.1f %s", b, u[i]);
}

static void fmt_eta(double s, char *out, size_t cap) {
    if (s < 0) { snprintf(out, cap, "--:--"); return; }
    int t = (int)(s + 0.5);
    int h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h > 0) snprintf(out, cap, "%d:%02d:%02d", h, m, sec);
    else       snprintf(out, cap, "%02d:%02d", m, sec);
}

static void progress(const cdm_progress *p, void *user) {
    (void)user;
    char dl[32], tot[32], spd[32], eta[16];
    human_bytes((double)p->downloaded_bytes, dl, sizeof(dl));
    human_bytes(p->total_bytes > 0 ? (double)p->total_bytes : 0, tot, sizeof(tot));
    human_bytes(p->speed_bps, spd, sizeof(spd));
    fmt_eta(p->eta_seconds, eta, sizeof(eta));

    int pct = 0;
    if (p->total_bytes > 0)
        pct = (int)(100.0 * (double)p->downloaded_bytes / (double)p->total_bytes);

    const int width = 30;
    int fill = p->total_bytes > 0 ? pct * width / 100 : 0;
    char bar[64];
    for (int i = 0; i < width; i++) bar[i] = i < fill ? '#' : '-';
    bar[width] = 0;

    fprintf(stderr,
        "\r[%s] %3d%%  %s/%s  %s/s  conn:%d  chunks:%d/%d  ETA %s   ",
        bar, pct, dl, tot, spd,
        p->active_connections, p->completed_chunks, p->total_chunks, eta);
    if (p->resumed) fprintf(stderr, "(resumed)");
    fflush(stderr);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "cdm - cross-platform download manager\n\n"
        "Usage: %s [options] <url>\n\n"
        "Options:\n"
        "  -o <file>     output path (default: derived from URL)\n"
        "  -n <num>      max connections (default: 16)\n"
        "  -s <num>      starting connections (default: 4)\n"
        "  -k <bytes>    chunk size (default: 8388608)\n"
        "  -l <bps>      max download speed in bytes/sec (0 = unlimited)\n"
        "  -r <num>      max retries per chunk (default: 5)\n"
        "  -x            disable adaptive concurrency\n"
        "  -C            disable resume\n"
        "  -S <sha256>   verify downloaded file against hex digest\n"
        "  -q            quiet (no progress bar)\n"
        "  -h            show this help\n",
        prog);
}

int main(int argc, char **argv) {
    cdm_config cfg;
    cdm_config_default(&cfg);
    const char *url = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2]) {
            char f = a[1];
            if (f == 'h') { usage(argv[0]); return 0; }
            if (f == 'x') { cfg.adaptive = 0; continue; }
            if (f == 'C') { cfg.resume = 0; continue; }
            if (f == 'q') { cfg.quiet = 1; continue; }
            if (i + 1 >= argc) { fprintf(stderr, "missing value for -%c\n", f); return 2; }
            const char *v = argv[++i];
            switch (f) {
                case 'o': cfg.output_path = v; break;
                case 'n': cfg.max_connections = atoi(v); break;
                case 's': cfg.start_connections = atoi(v); break;
                case 'k': cfg.chunk_size = strtoll(v, NULL, 10); break;
                case 'l': cfg.max_speed_bps = strtoll(v, NULL, 10); break;
                case 'r': cfg.max_retries = atoi(v); break;
                case 'S': cfg.expected_sha256 = v; break;
                default: fprintf(stderr, "unknown option -%c\n", f); return 2;
            }
        } else if (!url) {
            url = a;
        } else {
            fprintf(stderr, "unexpected argument: %s\n", a);
            return 2;
        }
    }

    if (!url) { usage(argv[0]); return 2; }
    cfg.url = url;

    if (cdm_global_init() != CDM_OK) {
        fprintf(stderr, "failed to init libcurl\n");
        return 1;
    }

    cdm_download *d = cdm_download_create(&cfg);
    if (!d) {
        fprintf(stderr, "failed to create download\n");
        cdm_global_cleanup();
        return 1;
    }

    if (!cfg.quiet) cdm_download_set_progress_cb(d, progress, NULL);

    g_active = d;
    signal(SIGINT, on_sigint);

    fprintf(stderr, "Downloading: %s\n", url);
    cdm_status st = cdm_download_run(d);

    if (!cfg.quiet) fprintf(stderr, "\n");

    int rc;
    if (st == CDM_OK) {
        fprintf(stderr, "Saved to: %s\n", cdm_download_output_path(d));
        rc = 0;
    } else {
        fprintf(stderr, "Error: %s\n", cdm_status_str(st));
        rc = 1;
    }

    g_active = NULL;
    cdm_download_destroy(d);
    cdm_global_cleanup();
    return rc;
}
