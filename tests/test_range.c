/*
 * Smoke tests that don't require network: exercise the SHA-256 helper and
 * status strings. Network integration is validated manually via the CLI.
 */
#include "cdm/download.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static void test_sha256(void) {
    const char *path = "test_sha_tmp.bin";
    FILE *f = fopen(path, "wb");
    fputs("abc", f);
    fclose(f);
    /* sha256("abc") */
    const char *want =
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(cdm_verify_sha256(path, want) == CDM_OK, "sha256(abc) matches");
    CHECK(cdm_verify_sha256(path, "deadbeef") != CDM_OK, "sha256 mismatch rejected");
    remove(path);
}

static void test_status_str(void) {
    CHECK(strcmp(cdm_status_str(CDM_OK), "ok") == 0, "status ok string");
    CHECK(cdm_status_str(CDM_ERR_NET) != NULL, "status net string");
}

static void test_config_default(void) {
    cdm_config c;
    cdm_config_default(&c);
    CHECK(c.max_connections > 0, "default max_connections set");
    CHECK(c.chunk_size >= 1, "default chunk_size set");
    CHECK(c.resume == 1, "resume on by default");
}

static void test_hls(void) {
    char out[2048];
    /* detection */
    CHECK(cdm_hls_should_handle("http://x/v.m3u8", "") == 1, "m3u8 suffix");
    CHECK(cdm_hls_should_handle("http://x/v.M3U8?tok=1", "") == 1, "suffix+query");
    CHECK(cdm_hls_should_handle("http://x/v.mp4", "") == 0, "mp4 ignored");
    CHECK(cdm_hls_should_handle("http://x/v", "application/vnd.apple.mpegurl") == 1,
          "content-type match");
    /* URI resolve */
    cdm_hls_resolve("https://h.com/a/b/pl.m3u8", "seg1.ts", out, sizeof(out));
    CHECK(strcmp(out, "https://h.com/a/b/seg1.ts") == 0, "relative resolve");
    cdm_hls_resolve("https://h.com/a/b/pl.m3u8", "/r/seg.ts", out, sizeof(out));
    CHECK(strcmp(out, "https://h.com/r/seg.ts") == 0, "root resolve");
    cdm_hls_resolve("https://h.com/a/b/pl.m3u8", "http://c.net/s.ts", out, sizeof(out));
    CHECK(strcmp(out, "http://c.net/s.ts") == 0, "absolute kept");
    cdm_hls_resolve("https://h.com/a/b/pl.m3u8", "../s.ts", out, sizeof(out));
    CHECK(strcmp(out, "https://h.com/a/s.ts") == 0, "dotdot resolve");
    /* master pick */
    {
        const char *master =
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\n"
            "low.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x720\n"
            "hi.m3u8\n";
        CHECK(cdm_hls_pick_variant(master, out, sizeof(out)) == 1, "variant found");
        CHECK(strcmp(out, "hi.m3u8") == 0, "best bandwidth wins");
    }
    /* media segments incl. MAP + encryption gate */
    {
        static char segs[8][2048];
        char map[2048];
        const char *media =
            "#EXTM3U\n#EXT-X-TARGETDURATION:6\n"
            "#EXT-X-MAP:URI=\"init.mp4\"\n"
            "#EXTINF:6.0,\nseg1.ts\n#EXTINF:6.0,\nseg2.ts\n"
            "#EXT-X-ENDLIST\n";
        int n = cdm_hls_segments(media, map, sizeof(map), segs, 8);
        CHECK(n == 2, "two segments parsed");
        CHECK(strcmp(map, "init.mp4") == 0, "MAP captured");
        CHECK(strcmp(segs[1], "seg2.ts") == 0, "segment order kept");
    }
    {
        static char segs[8][2048];
        char map[2048];
        const char *enc =
            "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k.key\"\n"
            "#EXTINF:6.0,\nseg1.ts\n";
        CHECK(cdm_hls_segments(enc, map, sizeof(map), segs, 8) < 0,
              "encrypted stream rejected");
    }
}

int main(void) {
    test_sha256();
    test_status_str();
    test_config_default();
    test_hls();
    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all tests passed\n");
    return 0;
}
