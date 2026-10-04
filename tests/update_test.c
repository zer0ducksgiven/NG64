/* Version comparison of the update check. Build: gcc -I helper/src -o tests/.tmp/update_test tests/update_test.c helper/src/update.c -lwinhttp */
#include <stdio.h>
#include "update.h"
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); fails++; } else printf("PASS %s\n", msg); } while (0)
int main(void)
{
    CHECK(ng64_version_newer("v0.1.5", "0.1.4"), "patch bump is newer");
    CHECK(ng64_version_newer("v0.2.0", "0.1.9"), "minor bump beats a bigger patch");
    CHECK(ng64_version_newer("v1.0.0", "0.9.9"), "major bump is newer");
    CHECK(!ng64_version_newer("v0.1.4", "0.1.4"), "same version is not newer");
    CHECK(!ng64_version_newer("v0.1.3", "0.1.4"), "older is not newer");
    CHECK(ng64_version_newer("v0.10.0", "0.9.0"), "0.10 is newer than 0.9 (numbers, not text)");
    CHECK(!ng64_version_newer("nightly", "0.1.4"), "a tag that isn't a version is ignored");
    CHECK(ng64_version_newer("v0.1.5", "0.1"), "a short current version still compares");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
