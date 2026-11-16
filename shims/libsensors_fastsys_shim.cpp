/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * libsensors_fastsys_shim.so —— 去掉传感器 HAL 初始化里 fork shell 的开销
 *
 * 背景（2026-11-16，开机速度专项）：
 *
 * 本机传感器 HAL 是 NVIDIA 原厂 blob：
 *     /system/vendor/lib/libsensors.lsm6db0.so
 * 它被设备树编译的 multihal（sensors.tegra.so）dlopen 加载，运行在 system_server
 * 进程里（SensorService 是 system_server 的一部分，不是独立进程）。
 *
 * 首次开机日志显示 sensorservice 从开始到就绪花了 5.68 秒：
 *     00:11:14.016  nuSensorService starting...
 *     00:11:14.033  MultiHal: Loaded library from libsensors.lsm6db0.so
 *     00:11:14.369  libsigchain 栈（system() -> posix_spawn -> sigaction64）
 *     00:11:16.063  SensorHAL: lsm6db0_magn_u: Sampling frequency available
 *     00:11:17.567  SensorHAL: lsm6db0_gyro_u: ...
 *     00:11:19.7    完成
 *
 * 注意 14.37 → 16.06（1.7 秒）和 16.07 → 17.57（1.5 秒）这两段空白，
 * 期间没有任何日志。反查 blob 的导入符号，它引用了 system()，而字符串表里
 * 唯一一条 shell 命令模板是：
 *
 *     rm -rf %s
 *
 * 也就是说 blob 在初始化时用 system() 去删 iio 设备节点。在 Android 14 上
 * system() 要走 posix_spawn("/system/bin/sh", "-c", ...) + waitpid，在
 * 2GB 内存、A15 级别 CPU 的机器上一次要 1.5~2 秒，正好对上那两段空白。
 *
 * 而它删的目标是 /sys/bus/iio/devices/... 和 /dev/iio:deviceN —— 这些是
 * sysfs / devtmpfs 节点，由内核驱动持有，用户态 rm -rf 必然失败（EROFS /
 * EPERM），blob 本身也不检查返回值。
 *
 * 本 shim 的做法：
 *   1) 把 "rm -rf <path>" 直接在进程内用 opendir/readdir/unlink/rmdir 递归实现，
 *      完全不 fork。对 sysfs 路径自然全部失败（返回 -1 被忽略），行为与原来一致；
 *      对真实文件系统路径也能正确删除，语义比"直接返回 0"更接近真 rm。
 *   2) 其它命令一律转交给真正的 libc system()，行为不变。
 *   3) 每条被接管的 system() 打一条 ALOGI，便于下次从 logcat 直接看出
 *      blob 到底想执行什么（第一次实测时因为没打日志，无法判断剩余空白来自哪里）。
 *
 * 加载方式：BoardConfig.mk 的 TARGET_LD_SHIM_LIBS
 *     /system/vendor/lib/libsensors.lsm6db0.so|libsensors_fastsys_shim.so
 * bionic linker 会把 shim 插到目标库 DT_NEEDED 之前（linker.cpp:1368），
 * 因此 blob 解析 system 符号时会先命中本库。已实测确认 shim 出现在
 * system_server 的 /proc/<pid>/maps 中。
 *
 * 实测效果（2026-11-16，lineage-21.0-20261116）：
 *   sensorservice 从 5.68 秒降到 3.6 秒；
 *   DeviceStateManagerService（会同步等 sensorservice 注册，
 *   日志 "Waiting for service 'sensorservice' on '/dev/binder'..."）
 *   从 6021ms 降到 4818ms。
 */

#define LOG_TAG "SensorSysShim"

#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <log/log.h>

namespace {

typedef int (*SystemFn)(const char*);

SystemFn gRealSystem = nullptr;

SystemFn realSystem() {
    if (gRealSystem == nullptr) {
        gRealSystem = reinterpret_cast<SystemFn>(dlsym(RTLD_NEXT, "system"));
    }
    return gRealSystem;
}

// 递归删除，等价于 rm -rf 的语义（不跟随符号链接、不存在也算成功）。
// 失败一律忽略：sysfs/devtmpfs 下的节点本来就不允许用户态删除。
void rmRf(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;  // 不存在，rm -rf 也返回 0
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (dir != nullptr) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[PATH_MAX];
                int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
                if (n > 0 && static_cast<size_t>(n) < sizeof(child)) {
                    rmRf(child);
                }
            }
            closedir(dir);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

}  // namespace

extern "C" int system(const char* command) {
    if (command == nullptr) {
        // 与 libc 语义一致：传 NULL 时探测 shell 可用性。
        return 1;
    }

    // 诊断用：把 blob 真正想执行的命令打出来（ALOGI 默认会进 logcat）。
    ALOGI("intercepted system(\"%s\")", command);

    if (strncmp(command, "rm -rf ", 7) == 0) {
        const char* target = command + 7;
        while (*target == ' ') {
            ++target;
        }
        // 安全兜底：拒绝根目录和顶层目录，避免误删把系统搞坏。
        if (target[0] == '/' && strchr(target, '/') != strrchr(target, '/')) {
            ALOGD("rm -rf %s done in-process", target);
            rmRf(target);
            return 0;
        }
    }

    SystemFn fn = realSystem();
    if (fn == nullptr) {
        ALOGE("dlsym(RTLD_NEXT, \"system\") failed: %s", dlerror());
        return -1;
    }
    return fn(command);
}
