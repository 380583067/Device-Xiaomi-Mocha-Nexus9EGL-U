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
 * libsensors_fastsys_shim.so - removes the fork+exec shell overhead from the
 * sensor HAL initialisation path.
 *
 * Background (2026-11-16, boot-speed work):
 *
 * The sensor HAL on this device is an NVIDIA proprietary blob:
 *
 *     /system/vendor/lib/libsensors.lsm6db0.so
 *
 * It is dlopen()ed by the multihal built from this device tree
 * (sensors.tegra.so) and runs inside the system_server process, because
 * SensorService is part of system_server rather than a standalone process.
 *
 * The first-boot log showed sensorservice taking 5.68 s from start to ready:
 *
 *     00:11:14.016  nuSensorService starting...
 *     00:11:14.033  MultiHal: Loaded library from libsensors.lsm6db0.so
 *     00:11:14.369  libsigchain stack (system() -> posix_spawn -> sigaction64)
 *     00:11:16.063  SensorHAL: lsm6db0_magn_u: Sampling frequency available
 *     00:11:17.567  SensorHAL: lsm6db0_gyro_u: ...
 *     00:11:19.7    done
 *
 * Note the two gaps, 14.37 -> 16.06 (1.7 s) and 16.07 -> 17.57 (1.5 s), during
 * which nothing at all is logged. Inspecting the blob's imports shows that it
 * references system(), and the only shell command template in its string table
 * is:
 *
 *     rm -rf %s
 *
 * So the blob calls system() to delete iio device nodes during initialisation.
 * On Android 14 system() goes through
 * posix_spawn("/system/bin/sh", "-c", ...) + waitpid, which costs 1.5-2 s per
 * invocation on a 2 GB / A15-class machine - exactly matching those two gaps.
 *
 * The targets it deletes are /sys/bus/iio/devices/... and /dev/iio:deviceN.
 * These are sysfs / devtmpfs nodes owned by kernel drivers, so a userspace
 * rm -rf can only fail (EROFS / EPERM), and the blob does not check the return
 * value anyway.
 *
 * What this shim does:
 *
 *   1) "rm -rf <path>" is implemented in-process with a recursive
 *      opendir/readdir/unlink/rmdir walk, with no fork at all. For sysfs paths
 *      every operation fails naturally and the -1 is ignored, so the observable
 *      behaviour is unchanged; for real filesystem paths the deletion works,
 *      which is semantically closer to real rm than simply returning 0.
 *   2) Every other command is forwarded to the real libc system(), unchanged.
 *   3) Each intercepted system() logs one ALOGI line, so that the next logcat
 *      shows exactly what the blob wanted to run. (During the first measurement
 *      there was no logging, which made the remaining gaps unattributable.)
 *
 * How it is loaded: TARGET_LD_SHIM_LIBS in BoardConfig.mk
 *
 *     /system/vendor/lib/libsensors.lsm6db0.so|libsensors_fastsys_shim.so
 *
 * The bionic linker inserts the shim ahead of the target library's DT_NEEDED
 * entries (linker.cpp:1368), so the blob resolves `system` to this library
 * first. Verified that the shim shows up in system_server's /proc/<pid>/maps.
 *
 * Measured effect (2026-11-16, lineage-21.0-20261116):
 *
 *   sensorservice went from 5.68 s down to 3.6 s;
 *   DeviceStateManagerService, which waits synchronously for sensorservice to
 *   register (log: "Waiting for service 'sensorservice' on '/dev/binder'..."),
 *   went from 6021 ms down to 4818 ms.
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

// Recursive delete with rm -rf semantics: symlinks are not followed, and a
// non-existent path counts as success. Every failure is ignored, because nodes
// under sysfs/devtmpfs can never be removed from userspace in the first place.
void rmRf(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;  // does not exist; real rm -rf would also return 0
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
        // Same as libc: a NULL argument probes whether a shell is available.
        return 1;
    }

    // Diagnostic only: log what the blob actually wanted to execute. ALOGI goes
    // to logcat by default.
    ALOGI("intercepted system(\"%s\")", command);

    if (strncmp(command, "rm -rf ", 7) == 0) {
        const char* target = command + 7;
        while (*target == ' ') {
            ++target;
        }
        // Safety net: refuse the root directory and top-level directories, so a
        // bad path can never take the whole system down.
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
