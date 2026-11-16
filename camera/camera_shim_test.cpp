/*
 * camera_shim_test -- diagnostic tool for the camera blob load chain
 * ============================================================================
 * Purpose: verify the dlopen dependency chain of camera.vendor.tegra.so *without*
 *          reflashing the system image.
 *
 * Background: normally the shim is injected before the blob by the LD_SHIM_LIBS table
 *       built into /system/bin/linker, and changing it requires a new system image. To first
 *       check whether all symbols are present, this dlopens them in the same order by hand:
 *         1. libmocha_camera_shim.so        (ABI forwarding)
 *         2. libcameraservice_mocha.so      (empty stub whose SONAME is libcameraservice.so)
 *         3. /system/lib/hw/camera.vendor.tegra.so
 *
 * Usage:
 *   adb push camera_shim_test /data/local/tmp/ && adb shell /data/local/tmp/camera_shim_test
 *
 * Expected result per step:
 *   1/2 must be OK - otherwise the shim is built wrong (e.g. an unexpected DT_NEEDED).
 *   3   OK      - all 345 undefined symbols resolve, safe to flash the full package.
 *   3   fails   - check the symbol named in dlerror() and add it in mocha_camera_shim.cpp.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int try_dlopen(const char* tag, const char* name, int flags) {
    // Clear the previous error state, otherwise dlerror() may return stale content
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

    // Same order as the linker injects shims: the shim comes before the blob's DT_NEEDED.
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
