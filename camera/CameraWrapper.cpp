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
 * 预加载相机 blob 的 ABI 兼容层
 * ============================================================================
 * 原厂 blob camera.vendor.tegra.so 是 Android 5.1 时代编译的，直接
 * hw_get_module 加载会失败（缺 2 个 DT_NEEDED 文件 + 一批 Android C++ ABI 符号）。
 * 修补思路见 device/xiaomi/mocha/camera/mocha_camera_shim.cpp 的头部注释。
 *
 * 这里负责把那些补丁库提前塞进本进程。两条注入途径同时保留：
 *   1) BoardConfig.mk 的 TARGET_LD_SHIM_LIBS —— 那套机制把映射表编译进了
 *      /system/bin/linker，是"正统"做法，但改它必须刷新的 system 镜像；
 *   2) 就是这个函数 —— 显式 dlopen。好处是不依赖 linker 二进制，热替换库文件
 *      就能验证，而且在两套机制都生效时 dlopen 本身是幂等的。
 *
 * 两个库的加载顺序有讲究：
 *   - libcameraservice_mocha.so 的 ELF SONAME 声明成了 "libcameraservice.so"
 *     （因为 Make 不允许给 SHARED_LIBRARY 设 LOCAL_MODULE_STEM，模块名又和
 *     Soong 里那个 cc_library 撞车）。它必须比 blob 先加载，blob 的
 *     DT_NEEDED "libcameraservice.so" 才能命中已加载库的 soname 索引
 *     （bionic linker.cpp:1468 find_loaded_library_by_soname）。
 *   - libmocha_camera_shim.so 带 DF_1_GLOBAL（链接时 -Wl,-z,global），
 *     用 RTLD_GLOBAL 打开后其符号进入进程的 global group，blob 才能解析到
 *     CameraMetadata / Fence / SensorManager 那些变形或消失的符号。
 *
 * 失败不致命：相机本来就用不了，这里最多是让日志更早暴露原因。
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
     * 注意：这里**故意不调用** preload_vendor_camera_shims()。
     *
     * 实测结论（2026-11，真机验证）：把 ABI shim 全部补齐之后，blob 确实能
     * dlopen 成功（camera_shim_test 显示 345 个未定义符号全部可解析），但
     * camera.vendor.tegra.so 走的是 NVIDIA 老式 PCL 通路，而本内核的摄像头
     * 设备树已经按 V4L2 subdev 框架重写（tegra124-mocha-camera-mc.dtsi：
     * imx179@10 / ov5693@36 / ad5823@0c），没有 blob 需要的 PCL 节点。
     * 后果是 blob 逐个探测 PCL 芯片全部超时（每个 4 秒），长时间占住 i2c 总线锁，
     * **直接把整个系统卡死**（adb shell 全部阻塞、adb reboot 无响应）。
     *
     * 所以 blob 这条路在 HAL 切换到 V4L2 自研实现（MochaCameraHAL.cpp）之前
     * 不能启用。保留这个函数是为了将来若补齐 PCL 设备树时能一键恢复。
     *
     * 恢复方法：取消下面这行注释，同时把 BoardConfig.mk 里被注释掉的
     * TARGET_LD_SHIM_LIBS 相机两行也恢复。
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
