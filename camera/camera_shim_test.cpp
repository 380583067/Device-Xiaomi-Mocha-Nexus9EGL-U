/*
 * camera_shim_test —— 相机 blob 加载链路诊断工具
 * ============================================================================
 * 用途：在**不重新刷 system 镜像**的前提下，验证 camera.vendor.tegra.so 的
 *       dlopen 依赖链是否已经补齐。
 *
 * 背景：正常情况下 shim 是由 linker 内置的 LD_SHIM_LIBS 表在 blob 之前注入的，
 *       但那张表编译进了 /system/bin/linker，改它必须刷 system 镜像。为了先确认
 *       "符号到底齐没齐"，这里手工按同样的顺序 dlopen 一遍：
 *         1. libmocha_camera_shim.so        （ABI 转发）
 *         2. libcameraservice_mocha.so      （SONAME = libcameraservice.so 的空 stub）
 *         3. /system/lib/hw/camera.vendor.tegra.so
 *
 * 用法：
 *   adb push camera_shim_test /data/local/tmp/ && adb shell /data/local/tmp/camera_shim_test
 *
 * 各步预期：
 *   1/2 必须 OK —— 不 OK 就是 shim 自己编坏了（比如多了不该有的 DT_NEEDED）。
 *   3   OK      —— 说明 345 个未定义符号全部可解析，可以放心刷整包。
 *   3   失败    —— 看 dlerror() 里点名的符号，回到 mocha_camera_shim.cpp 里补。
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int try_dlopen(const char* tag, const char* name, int flags) {
    // 清掉上一次的错误状态，否则 dlerror() 可能返回陈旧内容
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

    // 顺序与 linker 注入 shim 的顺序一致：shim 在 blob 的 DT_NEEDED 之前。
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
