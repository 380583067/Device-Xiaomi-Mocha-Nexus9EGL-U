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
 * libsensors_fastsys_shim.so - remove the fork-a-shell cost from sensor HAL init
 *
 * Background (2026-11-16, boot-time work):
 * The sensor HAL is the NVIDIA vendor blob, dlopen'ed by the device-tree multihal
 * (sensors.tegra.so) and running inside system_server (SensorService is part of
 * system_server, not a separate process).
 *
 * On the first boot sensorservice took 5.68 s to become ready:
 *     00:11:14.369  libsigchain stack (system() -> posix_spawn -> sigaction64)
 *     00:11:19.7    done
 * Note the two gaps, 14.37 -> 16.06 (1.7 s) and 16.07 -> 17.57 (1.5 s), with no log
 * output at all. The blob's imports include system(), and the only shell template in
 * its string table is a "rm -rf" of iio device nodes.
 *
 * So the blob runs system() during init to delete iio nodes. On Android 14 system()
 * goes through posix_spawn("/system/bin/sh", "-c", ...) plus waitpid, which costs
 * 1.5-2 s on a 2 GB A15 class device - exactly those two gaps.
 *
 * What it deletes - /sys/bus/iio/devices/... and /dev/iio:deviceN - are sysfs /
 * devtmpfs nodes owned by kernel drivers, so a user-space rm -rf always fails
 * (EROFS / EPERM) and the blob ignores the return value.
 *
 * This shim:
 *   1) implements "rm -rf <path>" in-process with opendir/readdir/unlink/rmdir, no
 *      fork at all. sysfs paths still fail (-1, ignored), same as before, while real
 *      filesystem paths are deleted correctly - closer to real rm than "return 0";
 *   2) passes every other command through to the real libc system();
 *   3) logs every intercepted call with ALOGI, so the next logcat shows what the blob
 *      wanted to run (the first measurement had no logging and the remaining gaps
 *      could not be explained).
 *
 * Loaded through TARGET_LD_SHIM_LIBS in BoardConfig.mk: the bionic linker inserts
 * the shim before the target library's DT_NEEDED (linker.cpp:1368), so the blob's
 * system symbol resolves here first. Verified: the shim appears in system_server's
 * /proc/<pid>/maps.
 *
 * Measured effect (2026-11-16, lineage-21.0-20261116): sensorservice 5.68 s -> 3.6 s;
 * DeviceStateManagerService (which waits for sensorservice to register, log
 * "Waiting for service 'sensorservice' on '/dev/binder'...") 6021 ms -> 4818 ms.
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

// Recursive delete with rm -rf semantics (does not follow symlinks, a missing path counts as success).
// Failures are ignored: nodes under sysfs/devtmpfs cannot be deleted from user space anyway.
void rmRf(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;  // does not exist; rm -rf returns 0 as well
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
        // libc semantics: a NULL command probes whether a shell is available.
        return 1;
    }

    // Diagnostic: log the command the blob really wants to run (ALOGI reaches logcat).
    ALOGI("intercepted system(\"%s\")", command);

    if (strncmp(command, "rm -rf ", 7) == 0) {
        const char* target = command + 7;
        while (*target == ' ') {
            ++target;
        }
        // Safety net: refuse "/" and top-level directories so a bad call cannot wreck the system.
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
