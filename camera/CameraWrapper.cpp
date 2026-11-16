/*
 * Copyright (C) 2015, The CyanogenMod Project
 *           (C) 2017, The LineageOS Project
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

#define LOG_NDEBUG 0
#define LOG_PARAMETERS

#define LOG_TAG "CameraWrapper"
#include <cutils/log.h>
#include <dlfcn.h>
#include "CameraWrapper.h"
#include "Camera3Wrapper.h"


//------------DEBUG-----------------
static int pfd[2];
static pthread_t thr;

static void *thread_func(void*)
{
    ssize_t rdsz;
    char buf[128];
    while((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        if(buf[rdsz - 1] == '\n') --rdsz;
        buf[rdsz] = 0;  /* add null-terminator */
        __android_log_write(ANDROID_LOG_DEBUG, LOG_TAG, buf);
    }
    return 0;
}


int start_logger()
{

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if(pthread_create(&thr, 0, thread_func, 0) == -1)
        return -1;
    pthread_detach(thr);
    return 0;
}


//-----------DEBUG-------------------------


typedef struct camera_metadata_buffer_entry {
    uint32_t tag;
    uint32_t count;
    union {
        uint32_t offset;
        uint8_t  value[4];
    } data;
    uint8_t  type;
    uint8_t  reserved[3];
} camera_metadata_buffer_entry_t;

typedef uint32_t metadata_uptrdiff_t;
typedef uint32_t metadata_size_t;

struct camera_metadata {
    metadata_size_t          size;
    uint32_t                 version;
    uint32_t                 flags;
    metadata_size_t          entry_count;
    metadata_size_t          entry_capacity;
    metadata_uptrdiff_t      entries_start; // Offset from camera_metadata
    metadata_size_t          data_count;
    metadata_size_t          data_capacity;
    metadata_uptrdiff_t      data_start; // Offset from camera_metadata
    uint8_t                  reserved[];
};

static camera_metadata_buffer_entry_t *get_entries(
        const camera_metadata_t *metadata) {
    return (camera_metadata_buffer_entry_t*)
            ((uint8_t*)metadata + metadata->entries_start);
}


static camera_module_t *gVendorModule = 0;
static char prop[PROPERTY_VALUE_MAX];
static camera_metadata_t* vendorInfo[2] = {0,0};
static camera_info vendor_camera_info;

/*
 * Preload the ABI compatibility layer for the camera blob
 * ============================================================================
 * The vendor blob camera.vendor.tegra.so was compiled in the Android 5.1 era; loading it
 * directly through hw_get_module fails (2 missing DT_NEEDED files plus Android C++ ABI symbols).
 * See the header of device/xiaomi/mocha/camera/mocha_camera_shim.cpp for the approach.
 *
 * This function pushes those shim libraries into the process ahead of time. Two injection
 *   1) TARGET_LD_SHIM_LIBS in BoardConfig.mk - that mechanism compiles the mapping table
 *      into /system/bin/linker. It is the canonical way, but changing it needs a new system image;
 *   2) this function - an explicit dlopen. It does not depend on the linker binary, so hot-
 *      swapping the library is enough to verify, and dlopen is idempotent when both are active.
 *
 * Load order of the two libraries matters:
 *   - libcameraservice_mocha.so declares its ELF SONAME as "libcameraservice.so"
 *     (because Make forbids LOCAL_MODULE_STEM on a SHARED_LIBRARY and the module name
 *     clashes with the Soong cc_library). It must load before the blob so that the blob's
 *     DT_NEEDED "libcameraservice.so" hits the soname index of loaded libraries
 *     （bionic linker.cpp:1468 find_loaded_library_by_soname）。
 *   - libmocha_camera_shim.so is built with DF_1_GLOBAL (-Wl,-z,global), so opening it
 *     with RTLD_GLOBAL puts its symbols in the process global group and the blob can
 *     resolve the reshaped or vanished CameraMetadata / Fence / SensorManager symbols.
 *
 * Failure is not fatal: the camera does not work anyway, so the worst case is an earlier log.
 */
static void preload_vendor_camera_shims() __attribute__((unused));
static void preload_vendor_camera_shims()
{
    static bool preloaded = false;
    if (preloaded)
        return;
    preloaded = true;

    void* svc = dlopen("libcameraservice_mocha.so", RTLD_NOW | RTLD_GLOBAL);
    if (svc == NULL)
        ALOGW("preload libcameraservice_mocha.so failed: %s", dlerror());

    void* shim = dlopen("libmocha_camera_shim.so", RTLD_NOW | RTLD_GLOBAL);
    if (shim == NULL)
        ALOGW("preload libmocha_camera_shim.so failed: %s", dlerror());
    else
        ALOGV("camera ABI shim preloaded");
}

static int check_vendor_module()
{
    int rv = 0;
    ALOGV("%s", __FUNCTION__);

    if(gVendorModule)
        return 0;

    /*
     * NOTE: preload_vendor_camera_shims() is *deliberately not called* here.
     *
     * Measured result (2026-11, on device): with all ABI shims in place the blob does
     * dlopen successfully (camera_shim_test shows all 345 undefined symbols resolvable), but
     * camera.vendor.tegra.so uses NVIDIA's legacy PCL path while this kernel's camera
     * device tree was rewritten around the V4L2 subdev framework (tegra124-mocha-camera-mc.dtsi:
     * imx179@10 / ov5693@36 / ad5823@0c) and has no PCL nodes the blob expects.
     * The blob then probes PCL chips one by one, all time out (4s each) and hold the i2c
     * bus lock for a long time, *freezing the entire system* (adb shell blocks, adb reboot hangs).
     *
     * Therefore the blob path cannot be enabled before the HAL switches to the in-house
     * V4L2 implementation (MochaCameraHAL.cpp). The function is kept so it can be restored
     *
     * To restore: uncomment the line below and also restore the two commented-out camera
     * lines of TARGET_LD_SHIM_LIBS in BoardConfig.mk.
     */
    // preload_vendor_camera_shims();

    rv = hw_get_module_by_class("camera", "vendor", (const hw_module_t **)&gVendorModule);
    if (rv)
        ALOGE("failed to open vendor camera module");
    return rv;
}

static struct hw_module_methods_t camera_module_methods = {
        open: camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = CAMERA_MODULE_API_VERSION_2_3,
         .hal_api_version = HARDWARE_HAL_API_VERSION,
         .id = CAMERA_HARDWARE_MODULE_ID,
         .name = "MI PAD Camera Wrapper",
         .author = "The LineageOS Project",
         .methods = &camera_module_methods,
         .dso = NULL,
         .reserved = {0},
    },
    .get_number_of_cameras = camera_get_number_of_cameras,
    .get_camera_info = camera_get_camera_info,
    .set_callbacks = camera_set_callbacks,
    .get_vendor_tag_ops = camera_get_vendor_tag_ops,
    .open_legacy = NULL,
    .set_torch_mode = NULL,
    .init = NULL,
    .reserved = {0},
};

static int camera_device_open(const hw_module_t* module, const char* name,
                hw_device_t** device)
{
    int rv = -EINVAL;
    start_logger(); //DEBUG

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        rv = camera3_device_open(module, name, device);
    }

    return rv;
}

static int camera_get_number_of_cameras(void)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    return gVendorModule->get_number_of_cameras();
}

static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGV("%s camera_id: = %d", __FUNCTION__, camera_id);
    if (check_vendor_module())
        return 0;
//    int ret = gVendorModule->get_camera_info(camera_id, info);
    int ret = gVendorModule->get_camera_info(camera_id, &vendor_camera_info);
//    fillStaticInfo();

    info->facing = vendor_camera_info.facing;
    info->orientation = vendor_camera_info.orientation;
    info->device_version = vendor_camera_info.device_version;

    if (vendorInfo[camera_id] == 0 ) {
        vendorInfo[camera_id] = (camera_metadata_t*)vendor_camera_info.static_camera_characteristics;
        camera_metadata_entry_t found_entry;
        find_camera_metadata_entry(vendorInfo[camera_id], ANDROID_CONTROL_MAX_REGIONS, &found_entry);
        size_t del_index = found_entry.index;
        delete_camera_metadata_entry(vendorInfo[camera_id], del_index);

        find_camera_metadata_entry(vendorInfo[camera_id], ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS, &found_entry);
        del_index = found_entry.index;
        delete_camera_metadata_entry(vendorInfo[camera_id], del_index);

        int32_t max_regions[3] = {8,8,8};
        add_camera_metadata_entry(vendorInfo[camera_id], ANDROID_CONTROL_MAX_REGIONS, max_regions, 3);

        find_camera_metadata_entry(vendorInfo[camera_id], ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &found_entry);
        found_entry.data.u8[0] = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE;
        add_camera_metadata_entry(vendorInfo[camera_id], ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &found_entry, found_entry.index);
    }

    info->static_camera_characteristics = vendorInfo[camera_id];
    dump_camera_metadata(info->static_camera_characteristics, 1, 2);

    return ret;
}

static int camera_set_callbacks(const camera_module_callbacks_t *callbacks)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    return gVendorModule->set_callbacks(callbacks);
}

static void camera_get_vendor_tag_ops(vendor_tag_ops_t* ops)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return;
    return gVendorModule->get_vendor_tag_ops(ops);
}

static int camera_open_legacy(const struct hw_module_t* module, const char* id, uint32_t halVersion, struct hw_device_t** device)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;

    return gVendorModule->open_legacy(module, id, halVersion, device);
}
