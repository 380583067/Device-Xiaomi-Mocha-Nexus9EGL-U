/*
 * camera_shim_test - diagnostic for the camera blob load chain
 * ============================================================================
 * Purpose: verify that camera.vendor.tegra.so's dlopen dependency chain is
 *          complete **without flashing a new system image**.
 *
 * Background: normally the shim is injected by the LD_SHIM_LIBS table built into
 *          the linker before the blob loads, but that table lives inside
 *          /system/bin/linker and changing it means flashing system. To check "are
 *          the symbols all there" first, this tool repeats the same order by hand:
 *            1. libmocha_camera_shim.so        (ABI forwarding)
 *            2. libcameraservice_mocha.so      (empty stub, SONAME libcameraservice.so)
 *            3. /system/lib/hw/camera.vendor.tegra.so
 *
 * Usage:
 *   adb push camera_shim_test /data/local/tmp/ && adb shell /data/local/tmp/camera_shim_test
 *
 * Expected results:
 *          1/2 must be OK - otherwise the shim itself is broken (an unexpected
 *              DT_NEEDED, for example).
 *          3   OK      - all 345 undefined symbols resolve, the full image is safe
 *                        to flash.
 *          3   fails   - look at the symbol dlerror() names and add it to
 *                        mocha_camera_shim.cpp.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int try_dlopen(const char* tag, const char* name, int flags) {
    // Clear the previous error state, otherwise dlerror() may return stale text
    (void)dlerror();
    void* h = dlopen(name, flags);
    if (h == NULL) {
        printf("[FAIL] %-24s %s\n", tag, dlerror());
        return -1;
    }
    printf("[ OK ] %-24s handle=%p\n", tag, h);
    return 0;
}

int main(void) {
    int rc = 0;

    // Same order as the linker's shim injection: the shim comes before the blob's DT_NEEDED.
    if (try_dlopen("libmocha_camera_shim", "libmocha_camera_shim.so",
                   RTLD_NOW | RTLD_GLOBAL) != 0) {
        rc = 1;
    }
    if (try_dlopen("libcameraservice_mocha", "libcameraservice_mocha.so",
                   RTLD_NOW | RTLD_GLOBAL) != 0) {
        rc = 1;
    }
    if (try_dlopen("camera.vendor.tegra.so", "/system/lib/hw/camera.vendor.tegra.so",
                   RTLD_NOW) != 0) {
        rc = 1;
    }

    printf("---- %s ----\n", rc == 0 ? "ALL OK" : "has failures");
    return rc;
}
