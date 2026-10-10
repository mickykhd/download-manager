/* Offline tests for the update version comparator. */
#include "updater.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else         { fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

int main(void) {
    CHECK(cdm_vercmp("v0.2.0", "0.2.0") == 0, "v prefix ignored");
    CHECK(cdm_vercmp("0.2.0", "0.10.0") < 0, "numeric (not lexical) compare");
    CHECK(cdm_vercmp("1.10.4", "1.9.9") > 0, "minor compare");
    CHECK(cdm_vercmp("0.2.0", "0.2.0") == 0, "equal");
    CHECK(cdm_vercmp("0.2.1", "0.2.0") > 0, "patch compare");
    CHECK(cdm_vercmp("1.0", "1.0.0") < 0, "shorter is older");
    CHECK(cdm_update_poll(NULL, 0) == 0, "no result before check");
    if (failures) { fprintf(stderr, "%d test(s) failed\n", failures); return 1; }
    fprintf(stderr, "all update tests passed\n");
    return 0;
}
