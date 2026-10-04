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

int main(void) {
    test_sha256();
    test_status_str();
    test_config_default();
    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all tests passed\n");
    return 0;
}
