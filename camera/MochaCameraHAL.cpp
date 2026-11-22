/*
 * Mocha Camera Device - HAL3 implementation
 * Camera3 device stub for Xiaomi Mi Pad
 * For LineageOS 15.1 (Android 8.1)
 */

#define LOG_TAG "MochaCameraHAL"
#define LOG_NDEBUG 0

#include <cutils/log.h>
#include <cutils/properties.h>
#include <cutils/native_handle.h>
#include <hardware/camera_common.h>
#include <hardware/camera3.h>
#include <hardware/gralloc.h>
#include <utils/threads.h>
#include <utils/Vector.h>
/* 【拍照保存修复 2026-11-17】设备 gralloc 是 mapper@2.1（HIDL passthrough），
 * App(Aperture/CameraX) 通过 gralloc2.1 的 GraphicBufferMapper 锁 buffer，而
 * NVIDIA gralloc 的跨进程锁（共享内存 LockState + pthread mutex）要求所有进程
 * 用同一套接口锁/解锁。本 HAL 之前用 gralloc0 的 gralloc_module_t->lock()，
 * 与 App 的 gralloc2.1 锁语义不一致，导致 HAL 写 JPEG 后共享 LockState 残留
 * 为 -1，App 侧 AcquireBufferLock 永远超时（拍照卡死、无法保存）。改用
 * GraphicBufferMapper（走 mapper@2.1，与 App 一致）后锁状态正确配对。 */
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <system/graphics.h>
#include <system/camera_metadata.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <sys/poll.h>
#include <time.h>
#include <cstdio>
#include <vector>

#include "MochaCameraHAL.h"
#include "CameraPipeline.h"
#include "InFlightTracker.h"
#include "JpegEncoder.h"

using namespace android;

// ---------------------------------------------------------------------------
// JPEG (BLOB) output buffer layout
// ---------------------------------------------------------------------------
// 【2026-11-19 拍照文件被 4MB 零填充】framework 为 BLOB 流分配的缓冲，
// 其“宽”就等于字节容量、“高”为 1（Camera3Device::getJpegBufferSize()
// 算容量，Camera3OutputStream::setBufferSizeLocked() 用
// native_window_set_buffers_dimensions(mMaxSize, 1) 分配）。缓冲回收时
// Camera3OutputStream::fixUpHidlJpegBlobHeader() 只在【缓冲最后 8 字节】
// 里找 camera3_jpeg_blob_t 尾标；找不到就报 "jpeg blob id 0 is not correct"
// 并回退用 ANDROID_JPEG_MAX_SIZE 当图像长度，于是照片被 4MB 零填充。
// 以前我们把尾标写在 blobW*blobH*2 处（1280x720 只有 1.8MB），位置偏前，
// 所以永远命中不了。这里按照 framework 的公式复算容量：
//   capacity(w,h) = scale * (maxSize - kMin) + kMin
//   scale         = (w * h) / (maxJpegW * maxJpegH)
// 其中 kMin = 256KiB + sizeof(CameraBlob)，maxJpeg* 是本 HAL 对外声明的
// 最大 BLOB 分辨率（见 ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS）。
// 三处数值必须与静态元数据保持一致，否则尾标又会错位。
static constexpr int32_t kMaxJpegSize = 4149248;
static constexpr size_t kMinJpegBufferSize = 256 * 1024 + 8;
static constexpr uint32_t kMaxJpegWidth = 1920;
static constexpr uint32_t kMaxJpegHeight = 1080;

// 按 framework 的算法复算 BLOB 缓冲容量（字节）。注意必须整体做一次
// float 求值再截断，不能先截断乘积——否则会和 framework 差几个字节。
static size_t jpegBufferCapacity(uint32_t width, uint32_t height) {
    const float scale = static_cast<float>(width * height) /
                        static_cast<float>(kMaxJpegWidth * kMaxJpegHeight);
    return static_cast<size_t>(scale * (kMaxJpegSize - kMinJpegBufferSize) +
                               kMinJpegBufferSize);
}

// Forward declaration of HAL module info (defined at end of file)
extern camera_module_t HMI;

namespace mocha {

// Camera configurations
// 【2026-11-17】补上前摄 ov5693：内核侧已 probe 成功并绑定到 channel 1
//（tegra-vi-1 -> /dev/video1），CameraPipeline 也已按 cameraId 选 video0/video1，
// 此前只是 kCameras 里没登记，导致 getNumberOfCameras() 只返回 1。
const MochaCameraInfo MochaCameraHAL::kCameras[] = {
    { 0, "IMX179", CAMERA_FACING_BACK, 90, 3280, false },
    { 1, "OV5693", CAMERA_FACING_FRONT, 270, 2624, false },
};

const int MochaCameraHAL::kNumCameras = sizeof(MochaCameraHAL::kCameras) / sizeof(MochaCameraHAL::kCameras[0]);

// Static camera characteristics cache
static camera_metadata_t* gCameraCharacteristics[2] = { nullptr, nullptr };

/* 当前打开的相机 id，供 build_result_metadata() 拼 crop region 用（见 gActiveCameraId
 * 的赋值处 camera_device_open）。默认 0 = 后摄。 */
static int gActiveCameraId = 0;

/* Digital zoom: the crop actually applied to the last frame, in
 * active-array coordinates (what the framework reads back via
 * ANDROID_SCALER_CROP_REGION). Full frame by default. */
static int32_t gAppliedCrop[4] = {0, 0, 0, 0};

/* 【2026-11-18 录像方案1】编码器输入流标记。
 *
 * 之前是把这条流的 format 改写成 YCbCr_420_888，让拷贝路径走 YUV；但那样
 * framework 要和 ACodec 建好的 BufferQueue 对齐时谈不拢，一取缓冲就
 * `Stream 2: Can't dequeue next output buffer: -22`。
 * 现在改成：**格式保持 IMPLEMENTATION_DEFINED 不动**（让 gralloc 自己决定
 * 布局），只在这里记下指针，拷贝时用 isEncoderStream() 判断走 YUV 写入。 */
static camera3_stream_t* gEncoderStreams[4] = { nullptr, nullptr, nullptr, nullptr };

static bool isEncoderStream(const camera3_stream_t* s) {
    if (s == nullptr) return false;
    for (int i = 0; i < 4; i++) {
        if (gEncoderStreams[i] == s) return true;
    }
    return false;
}

// Module callbacks
static camera_module_callbacks_t gModuleCallbacks;

// Forward declarations
static int camera_device_init(const hw_module_t *module, hw_device_t **device);
static int camera_device_close(hw_device_t *device);
static int camera_device_initialize(const camera3_device_t *device, const camera3_callback_ops_t *ops);
static int camera_device_configure_streams(const camera3_device_t *device, camera3_stream_configuration_t *config);
static const camera_metadata_t* camera_device_construct_default_request_settings(const camera3_device_t *device, int type);
static int camera_device_process_capture_request(const camera3_device_t *device, camera3_capture_request_t *request);
static void camera_device_dump(const camera3_device_t *device, int fd);
static int camera_device_flush(const camera3_device_t *device);

static camera3_device_ops_t camera_device_ops = {
    .initialize = camera_device_initialize,
    .configure_streams = camera_device_configure_streams,
    .register_stream_buffers = nullptr,
    .construct_default_request_settings = camera_device_construct_default_request_settings,
    .process_capture_request = camera_device_process_capture_request,
    .get_metadata_vendor_tag_ops = nullptr,
    .dump = camera_device_dump,
    .flush = camera_device_flush,
    .reserved = { 0 },
};

struct mocha_camera_device_t {
    hw_device_t common;
    camera3_device_ops_t *ops;
    int camera_id;
    const camera3_callback_ops_t *callback_ops;
    bool is_initialized;
    bool streams_configured;

    void* pipeline;
    InFlightTracker* inflight_tracker;
    camera3_stream_t* output_stream;
    uint32_t pipeline_width;

    uint32_t pipeline_height;
    uint32_t last_config_width;
    uint32_t last_config_height;

    mocha::JpegEncoder* jpeg_encoder;
    uint8_t* temp_rgba;
    uint32_t temp_rgba_size;

    uint8_t af_mode;
    uint8_t af_trigger;
    bool af_trigger_handled;
};

// Initialize static camera characteristics
static camera_metadata_t* init_static_characteristics(int cameraId) {
    if (cameraId < 0 || cameraId >= 2) return nullptr;
    if (gCameraCharacteristics[cameraId] != nullptr) return gCameraCharacteristics[cameraId];

    const MochaCameraInfo& cam = MochaCameraHAL::kCameras[cameraId];
    
    size_t entry_capacity = 100;
    size_t data_capacity = 8192;
    camera_metadata_t* metadata = allocate_camera_metadata(entry_capacity, data_capacity);
    if (!metadata) {
        ALOGE("Failed to allocate camera metadata");
        return nullptr;
    }

    // Camera facing (map CAMERA_FACING_* to ANDROID_LENS_FACING_*)
    uint8_t facing = (cam.facing == CAMERA_FACING_BACK)
        ? ANDROID_LENS_FACING_BACK : ANDROID_LENS_FACING_FRONT;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FACING, &facing, 1);

    // Orientation
    int32_t orientation = cam.orientation;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

    // Available stream configurations
    // 【2026-11-18 对齐 A12】A12 那台（能正常拍照/录像）对外只报两种尺寸：
    // 1920x1080 与 1280x720，格式只有 BLOB / IMPLEMENTATION_DEFINED / YCbCr_420_888，
    // 而且**没有任何大于 1080p 的 BLOB**（它的 jpeg.maxSize 就是 1920*1080*2）。
    // 我们之前报了 3280x2464 的 8MP BLOB，Aperture 每次都挑它，于是走进
    // NVIDIA 老 gralloc 的 8MP BLOB 加锁路径（AcquireBufferLock ... LockState -1），
    // 拍照永远存不下来。这里按 A12 收敛尺寸，同时把 4:3 换成 16:9 —— 传感器
    // 实际只输出 720p，多出来的尺寸本来就是最近邻放大出来的。
    // 采集端（IMPL_DEF/YCbCr/YV12）只保留 1280x720：那是 V4L2 真正能出的尺寸。
    int32_t configs[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, CAMERA3_STREAM_OUTPUT,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, configs, sizeof(configs)/sizeof(int32_t));

    // Available min frame durations
    int64_t durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, 33333333LL,
    };
    int ret = add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, durations, sizeof(durations)/sizeof(int64_t));
    ALOGI("DEBUG: Added min frame durations, ret=%d", ret);

    // Available stall durations (format, width, height, stall_ns)
    int64_t stall_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, 0,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 0,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 100000000LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, 100000000LL,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stall_durations, sizeof(stall_durations)/sizeof(int64_t));

    // Available processed sizes
    int32_t processed_sizes[] = {
        1280, 720,
        1920, 1080,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, processed_sizes, sizeof(processed_sizes)/sizeof(int32_t));

    // Available JPEG sizes
    int32_t jpeg_sizes[] = {
        1280, 720,
        1920, 1080,
    };
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_JPEG_SIZES, jpeg_sizes, sizeof(jpeg_sizes)/sizeof(int32_t));

    // Max digital zoom
    float max_digital_zoom = 4.0f;
    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &max_digital_zoom, 1);

    // Android 12 zoomRatio control (CameraX maps pinch-zoom onto this)
    float zoom_ratio_range[] = { 1.0f, max_digital_zoom };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_ZOOM_RATIO_RANGE,
                              zoom_ratio_range, 2);

    // Request max pipeline depth (removed - not available in 8.1)
    
    // Flash available
    uint8_t flash_available = 0;
    add_camera_metadata_entry(metadata, ANDROID_FLASH_INFO_AVAILABLE, &flash_available, 1);

    // Sensor info
    // 【2026-11-17 修正】前摄 pixelArray 由 2592x1944 改为 2624x1956。
    // 依据：Android 12 参考机（Shield 图形栈那台）实测
    //   android.sensor.info.pixelArraySize = 2624x1956（OV5693）
    // 之前按 2592x1944 上报，导致 4:3 之外的宽高比对不上，Aperture 选尺寸时
    // 会出现找不到匹配项、回落奇怪分辨率的情况。
    int32_t sensor_width = cam.maxResolution;
    int32_t sensor_height = (cam.maxResolution == 3280) ? 2464 : 1956;
    /* 【2026-11-18 对齐 A12】有效阵列改成 16:9。A12 的 activeArraySize 是
     * [20 320 3240 1823]（约 3260x1823，1.79:1），pixelArray 仍是 3280x2464，
     * 也就是说它把传感器裁成 16:9 再用。我们原来按整块 4:3 报，Aperture 出来的
     * 取景框是 4:3，和 A12 的观感对不上（"画面比例"问题的由来）。
     * 这里 3280/16*9 = 1845，取整到 1845。 */
    int32_t active_height = (cam.maxResolution == 3280) ? 1845 : 1104;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, (int32_t[]){0, 0, sensor_width, active_height}, 4);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, (int32_t[]){sensor_width, sensor_height}, 2);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, (int32_t[]){0, 0, sensor_width, active_height}, 4);

    // Physical sensor size (required for FOV calculation)
    // 【2026-11-17 修正】原来两路写反了：后摄写 3.676x2.757（那其实是 OV5693
    // 的量级），前摄写 2.8x2.1。按 Android 12 参考机实测值改回：
    //   IMX179（后）: 4.5920 x 3.4496 mm  (1/3.2", 1.4um)
    //   OV5693（前）: 3.6736 x 2.7384 mm
    // 物理尺寸直接参与 FOV / 等效焦距换算，写反会让取景视野和"隐藏参数"里的
    // 参数对不上。
    float phys_size[2];
    if (cameraId == 0) {
        phys_size[0] = 4.5920f;
        phys_size[1] = 3.4496f;
    } else {
        phys_size[0] = 3.6736f;
        phys_size[1] = 2.7384f;
    }
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PHYSICAL_SIZE, phys_size, 2);

    // Sensor timestamp source
    int32_t sensor_timestamp_source = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &sensor_timestamp_source, 1);

    // Supported hardware level
    uint8_t hw_level = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
    add_camera_metadata_entry(metadata, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &hw_level, 1);

    // Request available capabilities
    uint8_t capabilities[] = {
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, capabilities, sizeof(capabilities)/sizeof(uint8_t));

    // Available request keys
    int32_t request_keys[] = {
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_COLOR_CORRECTION_MODE,
        ANDROID_CONTROL_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_JPEG_QUALITY,
        ANDROID_LENS_FOCUS_DISTANCE,
        ANDROID_NOISE_REDUCTION_MODE,
        ANDROID_CONTROL_ZOOM_RATIO,
        ANDROID_REQUEST_ID,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_SENSOR_EXPOSURE_TIME,
        ANDROID_SENSOR_SENSITIVITY,
        ANDROID_STATISTICS_FACE_DETECT_MODE,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, request_keys, sizeof(request_keys)/sizeof(int32_t));

    // Available result keys
    int32_t result_keys[] = {
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_STATE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_AWB_STATE,
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_ZOOM_RATIO,
        ANDROID_FLASH_MODE,
        ANDROID_JPEG_QUALITY,
        ANDROID_LENS_FOCUS_DISTANCE,
        ANDROID_LENS_STATE,
        ANDROID_REQUEST_ID,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_EXPOSURE_TIME,
        ANDROID_SENSOR_FRAME_DURATION,
        ANDROID_SENSOR_SENSITIVITY,
        ANDROID_SENSOR_TIMESTAMP,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, result_keys, sizeof(result_keys)/sizeof(int32_t));

    // Available scene modes (required by deriveCameraCharacteristicsKeys)
    uint8_t scene_modes[] = {
        ANDROID_CONTROL_SCENE_MODE_DISABLED,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, scene_modes, sizeof(scene_modes)/sizeof(uint8_t));

    // Available AE modes (required by deriveCameraCharacteristicsKeys)
    uint8_t ae_modes[] = {
        ANDROID_CONTROL_AE_MODE_ON,
        ANDROID_CONTROL_AE_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_MODES, ae_modes, sizeof(ae_modes)/sizeof(uint8_t));

    // Available AF modes (required by deriveCameraCharacteristicsKeys)
    // 【2026-11-17 修正】前摄 OV5693 是定焦（没有 VCM，AD5823 只挂在后摄
    // channel 0 上），原来两路都上报 AF，导致"隐藏参数"里前摄显示成
    // "1-Front:5MP f/2.8, AF"，且 Aperture 会给前摄发 AF 请求、HAL 空跑扫描。
    // 按 Android 12 参考机实测：后摄 afAvailableModes = {OFF, AUTO, CONTINUOUS_PICTURE}，
    // 前摄只有 {OFF}。
    if (cameraId == 0) {
        uint8_t af_modes[] = {
            ANDROID_CONTROL_AF_MODE_OFF,
            ANDROID_CONTROL_AF_MODE_AUTO,
            ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE,
        };
        add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, af_modes, sizeof(af_modes)/sizeof(uint8_t));
    } else {
        uint8_t af_modes[] = {
            ANDROID_CONTROL_AF_MODE_OFF,
        };
        add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES, af_modes, sizeof(af_modes)/sizeof(uint8_t));
    }

    // Available AWB modes (required by deriveCameraCharacteristicsKeys)
    uint8_t awb_modes[] = {
        ANDROID_CONTROL_AWB_MODE_AUTO,
        ANDROID_CONTROL_AWB_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awb_modes, sizeof(awb_modes)/sizeof(uint8_t));

    // Available AE target FPS ranges (required by Parameters::initialize)
    // Note: Reference HAL uses simple FPS units (15, 30), not milli-fps
    int32_t ae_fps_ranges[] = {
        15, 30,  // 15-30 fps
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, ae_fps_ranges, sizeof(ae_fps_ranges)/sizeof(int32_t));

    // Available JPEG thumbnail sizes (required by Parameters::initialize)
    int32_t jpeg_thumbnail_sizes[] = {
        0, 0,       // No thumbnail
        160, 120,   // Small
        320, 240,   // Medium
    };
    add_camera_metadata_entry(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpeg_thumbnail_sizes, sizeof(jpeg_thumbnail_sizes)/sizeof(int32_t));

    // Available hot pixel modes
    uint8_t hot_pixel_modes[] = {
        ANDROID_HOT_PIXEL_MODE_FAST,
        ANDROID_HOT_PIXEL_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, hot_pixel_modes, sizeof(hot_pixel_modes)/sizeof(uint8_t));

    // Available edge modes
    uint8_t edge_modes[] = {
        ANDROID_EDGE_MODE_OFF,
        ANDROID_EDGE_MODE_FAST,
        ANDROID_EDGE_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edge_modes, sizeof(edge_modes)/sizeof(uint8_t));

    // Available noise reduction modes
    uint8_t nr_modes[] = {
        ANDROID_NOISE_REDUCTION_MODE_OFF,
        ANDROID_NOISE_REDUCTION_MODE_FAST,
        ANDROID_NOISE_REDUCTION_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, nr_modes, sizeof(nr_modes)/sizeof(uint8_t));

    // Available shading modes
    uint8_t shading_modes[] = {
        ANDROID_SHADING_MODE_OFF,
        ANDROID_SHADING_MODE_FAST,
        ANDROID_SHADING_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_SHADING_AVAILABLE_MODES, shading_modes, sizeof(shading_modes)/sizeof(uint8_t));

    // Available lens shading map modes
    uint8_t lsc_map_modes[] = {
        ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES, lsc_map_modes, sizeof(lsc_map_modes)/sizeof(uint8_t));

    // Max face count (required when STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES is present)
    int32_t max_face_count = 0;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &max_face_count, 1);

    // Available face detect modes (required by OpenCamera deriveCameraCharacteristicsKeys)
    uint8_t face_detect_modes[] = { ANDROID_STATISTICS_FACE_DETECT_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, face_detect_modes, 1);

    // Available tonemap modes
    uint8_t tonemap_modes[] = {
        ANDROID_TONEMAP_MODE_CONTRAST_CURVE,
        ANDROID_TONEMAP_MODE_FAST,
        ANDROID_TONEMAP_MODE_HIGH_QUALITY,
    };
    add_camera_metadata_entry(metadata, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, tonemap_modes, sizeof(tonemap_modes)/sizeof(uint8_t));

    // Available cfa layout
    uint8_t cfa_layout = ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT, &cfa_layout, 1);

    // AE lock available
    uint8_t ae_lock_available = ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_LOCK_AVAILABLE, &ae_lock_available, 1);

    // AWB lock available
    uint8_t awb_lock_available = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awb_lock_available, 1);

    // AE compensation range (required by Parameters::initialize)
    int32_t ae_comp_range[] = { -9, 9 };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_COMPENSATION_RANGE, ae_comp_range, 2);

    // AE compensation step (required)
    camera_metadata_rational ae_comp_step = { 1, 3 };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_COMPENSATION_STEP, &ae_comp_step, 1);

    // Sensor exposure time range (required)
    int64_t exposure_time_range[] = { 10000LL, 500000000LL };  // 10us to 500ms
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, exposure_time_range, 2);

    // Sensor sensitivity range (required)
    int32_t sensitivity_range[] = { 100, 1600 };
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, sensitivity_range, 2);

    // Max analog sensitivity (required)
    int32_t max_analog_sensitivity = 1600;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY, &max_analog_sensitivity, 1);

    // JPEG max size (required)
    // 【2026-11-18 对齐 A12】A12 的 jpeg.maxSize 是 4149248（约 1920x1080x2），
    // 我们原来按 8MP 报 16163840，framework 会据此给 BLOB 分配超大缓冲。
    int32_t jpeg_max_size = kMaxJpegSize;
    add_camera_metadata_entry(metadata, ANDROID_JPEG_MAX_SIZE, &jpeg_max_size, 1);

    // Pipeline max depth (required)
    uint8_t pipeline_depth = 4;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipeline_depth, 1);

    // Max input streams (required)
    int32_t max_input_streams = 0;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, &max_input_streams, 1);

    // Lens hyperfocal distance (required)
    float hyperfocal = 0.0f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperfocal, 1);

    // Lens minimum focus distance (required)
    float min_focus_distance = (cameraId == 0) ? 10.0f : 0.0f;  // diopters
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &min_focus_distance, 1);

    // Focus distance calibration (required)
    uint8_t focus_cal = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &focus_cal, 1);

    // Available effects (required)
    uint8_t effects[] = { ANDROID_CONTROL_EFFECT_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_EFFECTS, effects, 1);

    // Available antibanding modes (required)
    uint8_t antibanding[] = { ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, antibanding, 1);

    // Available video stabilization modes (required)
    uint8_t video_stab[] = { ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, video_stab, 1);

    // Lens optical stabilization (required)
    uint8_t optical_stab[] = { ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, optical_stab, 1);

    // Color correction aberration modes (required)
    uint8_t cc_aberration[] = { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF };
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, cc_aberration, 1);

    // Available modes
    uint8_t control_modes[] = {
        ANDROID_CONTROL_MODE_AUTO,
        ANDROID_CONTROL_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AVAILABLE_MODES, control_modes, sizeof(control_modes)/sizeof(uint8_t));

    // Test pattern data modes
    uint8_t test_pattern_modes[] = {
        ANDROID_SENSOR_TEST_PATTERN_MODE_OFF,
    };
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES, test_pattern_modes, sizeof(test_pattern_modes)/sizeof(uint8_t));

    // Lens focal length (required by buildFastInfo)
    // 【2026-11-17 修正】原来两路都写 3.5mm（占位值）。按 Android 12 参考机实测：
    // 后摄 IMX179 = 3.36mm，前摄 OV5693 = 2.1mm。焦距参与 FOV 计算，
    // 写错会让"隐藏参数"里显示的镜头信息和实际不符。
    float focal_lengths[1];
    focal_lengths[0] = (cameraId == 0) ? 3.36f : 2.10f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focal_lengths, 1);

    // Lens aperture (required by some framework paths)
    // 【2026-11-17 修正】原来写 2.8（占位值）。两颗模组实际都是 f/2.0，
    // Android 12 参考机实测 availableApertures = {2.0}，这里对齐。
    float apertures[] = { 2.0f };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_APERTURES, apertures, sizeof(apertures)/sizeof(float));

    // Filter density
    float filter_densities[] = { 0.0f };
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, filter_densities, sizeof(filter_densities)/sizeof(float));

    // Max 3A regions
    int32_t max_3a_regions[] = { 1, 1, 0 };  // AE, AWB, AF
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MAX_REGIONS, max_3a_regions, sizeof(max_3a_regions)/sizeof(int32_t));

    // Request max num output streams
    int32_t max_output_streams[] = { 3, 3, 1 };  // PREVIEW, RECORD, MAX
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS, max_output_streams, sizeof(max_output_streams)/sizeof(int32_t));

    // Partial result count
    int32_t partial_result_count = 1;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partial_result_count, 1);

    // Sync max latency
    int64_t sync_max_latency = ANDROID_SYNC_MAX_LATENCY_PER_FRAME_CONTROL;
    add_camera_metadata_entry(metadata, ANDROID_SYNC_MAX_LATENCY, &sync_max_latency, 1);

    // Available characteristics keys (required by OpenCamera deriveCameraCharacteristicsKeys)
    int32_t characteristics_keys[] = {
        ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_MODES,
        ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
        ANDROID_CONTROL_AE_COMPENSATION_RANGE,
        ANDROID_CONTROL_AE_COMPENSATION_STEP,
        ANDROID_CONTROL_AE_LOCK_AVAILABLE,
        ANDROID_CONTROL_AF_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_EFFECTS,
        ANDROID_CONTROL_AVAILABLE_MODES,
        ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
        ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
        ANDROID_CONTROL_AWB_AVAILABLE_MODES,
        ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
        ANDROID_CONTROL_MAX_REGIONS,
        ANDROID_EDGE_AVAILABLE_EDGE_MODES,
        ANDROID_FLASH_INFO_AVAILABLE,
        ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
        ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
        ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
        ANDROID_JPEG_MAX_SIZE,
        ANDROID_LENS_FACING,
        ANDROID_LENS_INFO_AVAILABLE_APERTURES,
        ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
        ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
        ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
        ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
        ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
        ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
        ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
        ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
        ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
        ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
        ANDROID_REQUEST_AVAILABLE_RESULT_KEYS,
        ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
        ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
        ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
        ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
        ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
        ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
        ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
        ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
        ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
        ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
        ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
        ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
        ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
        ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
        ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
        ANDROID_SENSOR_ORIENTATION,
        ANDROID_SHADING_AVAILABLE_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
        ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
        ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
        ANDROID_SYNC_MAX_LATENCY,
        ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
    };
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, characteristics_keys, sizeof(characteristics_keys)/sizeof(int32_t));

    sort_camera_metadata(metadata);
    
    // Debug: dump stream configurations
    camera_metadata_entry_t debug_configs;
    if (find_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &debug_configs) == 0 && debug_configs.count > 0) {
        ALOGI("DEBUG: Stream configs count=%zu", debug_configs.count);
        for (size_t i = 0; i < debug_configs.count; i += 4) {
            ALOGI("DEBUG: Config[%zu] format=%d width=%d height=%d input=%d",
                  i/4, debug_configs.data.i32[i], debug_configs.data.i32[i+1],
                  debug_configs.data.i32[i+2], debug_configs.data.i32[i+3]);
        }
    } else {
        ALOGE("DEBUG: No stream configs found!");
    }
    
    // Debug: dump FPS ranges
    camera_metadata_entry_t debug_fps;
    if (find_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, &debug_fps) == 0 && debug_fps.count > 0) {
        ALOGI("DEBUG: FPS ranges count=%zu", debug_fps.count);
        for (size_t i = 0; i < debug_fps.count; i += 2) {
            ALOGI("DEBUG: FPS[%zu] min=%d max=%d", i/2, debug_fps.data.i32[i], debug_fps.data.i32[i+1]);
        }
    }
    
    // Debug: dump focal lengths
    camera_metadata_entry_t debug_focal;
    if (find_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, &debug_focal) == 0 && debug_focal.count > 0) {
        ALOGI("DEBUG: Focal lengths count=%zu", debug_focal.count);
        for (size_t i = 0; i < debug_focal.count; i++) {
            ALOGI("DEBUG: Focal[%zu] = %f", i, debug_focal.data.f[i]);
        }
    }
    
    ALOGI("DEBUG: Metadata sorted successfully for camera %d", cameraId);
    
    gCameraCharacteristics[cameraId] = metadata;
    return metadata;
}

static int camera_device_init(const hw_module_t *module, hw_device_t **device) {
    ALOGI("camera_device_init");
    
    mocha_camera_device_t *dev = new mocha_camera_device_t();
    if (!dev) {
        ALOGE("Failed to allocate camera device");
        return -ENOMEM;
    }

    memset(dev, 0, sizeof(mocha_camera_device_t));
    
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_3_2;
    dev->common.module = const_cast<hw_module_t *>(module);
    dev->common.close = camera_device_close;
    dev->ops = &camera_device_ops;
    
    dev->camera_id = 0;
    dev->callback_ops = nullptr;
    dev->is_initialized = false;
    dev->streams_configured = false;
    dev->pipeline = nullptr;
    dev->inflight_tracker = new InFlightTracker();
    dev->output_stream = nullptr;
    dev->last_config_width = 0;
    dev->last_config_height = 0;
    dev->jpeg_encoder = nullptr;
    dev->temp_rgba = nullptr;
    dev->temp_rgba_size = 0;
    dev->af_mode = ANDROID_CONTROL_AF_MODE_AUTO;
    dev->af_trigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    dev->af_trigger_handled = false;

    *device = &dev->common;
    
    ALOGI("Camera device initialized");
    return 0;
}

static int camera_device_close(hw_device_t *device) {
    ALOGI("camera_device_close");
    
    if (!device) {
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;

    if (dev->jpeg_encoder) {
        delete dev->jpeg_encoder;
        dev->jpeg_encoder = nullptr;
    }
    if (dev->temp_rgba) {
        free(dev->temp_rgba);
        dev->temp_rgba = nullptr;
        dev->temp_rgba_size = 0;
    }

    if (dev->inflight_tracker) {
        dev->inflight_tracker->markAllAsError();
        delete dev->inflight_tracker;
        dev->inflight_tracker = nullptr;
    }

    if (dev->pipeline) {
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
    }

    delete dev;
    
    ALOGI("Camera device closed");
    return 0;
}

static int camera_device_initialize(const camera3_device_t *device, const camera3_callback_ops_t *ops) {
    ALOGI("camera_device_initialize");
    
    if (!device || !ops) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    dev->callback_ops = ops;
    dev->is_initialized = true;

    ALOGI("Camera initialized with callbacks");
    return 0;
}

static int camera_device_configure_streams(const camera3_device_t *device, camera3_stream_configuration_t *config) {
    ALOGI("camera_device_configure_streams: num_streams=%d", config ? config->num_streams : -1);

    if (!device || !config) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    // Validate num_streams to catch garbled HAL1→HAL3 fallback
    if (config->num_streams == 0 || config->num_streams > 20) {
        ALOGE("Invalid num_streams=%d, rejecting garbled config", config->num_streams);
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;

    // Find pipeline stream (prefer RGBA_8888, then IMPLEMENTATION_DEFINED, then any non-BLOB)
    camera3_stream_t* pipelineStream = nullptr;
    camera3_stream_t* outputStream = nullptr;
    camera3_stream_t* rgbaStream = nullptr;
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        ALOGI("Stream %d: type=%d, width=%d, height=%d, format=%d",
              i, stream->stream_type, stream->width, stream->height, stream->format);
        
        if (stream->width == 0 || stream->height == 0) {
            ALOGE("Invalid stream dimensions");
            return -EINVAL;
        }

        if (stream->stream_type == CAMERA3_STREAM_OUTPUT) {
            stream->max_buffers = 2;
            outputStream = stream;
            if (!pipelineStream && stream->format != HAL_PIXEL_FORMAT_BLOB) {
                // Prefer RGBA_8888 for pipeline to avoid YUV format issues
                if (stream->format == HAL_PIXEL_FORMAT_RGBA_8888) {
                    pipelineStream = stream;
                    rgbaStream = stream;
                } else if (!rgbaStream && stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                    // IMPLEMENTATION_DEFINED will be overridden to RGBA_8888
                    rgbaStream = stream;
                } else if (!pipelineStream) {
                    pipelineStream = stream;
                }
            }
        }
    }
    
    // If we found RGBA/IMPLEMENTATION_DEFINED, use that as pipeline stream
    if (rgbaStream) pipelineStream = rgbaStream;

    /* Pipeline resolution = the largest stream the framework asked for
     * (preview, video encoder, JPEG BLOB, analysis). The imx179/ov5693
     * kernels program a 1:1 centered crop window for whatever size is
     * requested, so capturing at the highest resolution costs nothing
     * extra and every output just downscales - this is what makes stills
     * sharp instead of a 720p capture upscaled to 1080p. */
    uint32_t pipeW = pipelineStream->width, pipeH = pipelineStream->height;
    uint32_t pipeArea = pipeW * pipeH;
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *s = config->streams[i];
        if (s->stream_type != CAMERA3_STREAM_OUTPUT) continue;
        uint32_t area = s->width * s->height;
        if (area > pipeArea) { pipeArea = area; pipeW = s->width; pipeH = s->height; }
    }
    /* CSI link bandwidth caps, measured on hardware: the OV5693 streams over
     * a SINGLE lane (CSI-E) - asking it for 1080p makes VI capture time out
     * (-110) and the channel falls back to the TPG test pattern. The IMX179
     * uses 4 lanes (CSI-A) and does 1080p natively. Anything bigger is also
     * capped: 1080p is the sweet spot of the 1:1 readout at 30fps. */
    const uint32_t kMaxArea = (dev->camera_id == 0) ? (1920u * 1080u)
                                                    : (1280u * 720u);
    if (pipeW * pipeH > kMaxArea) { pipeW = 1280; pipeH = 720;
        if (dev->camera_id == 0) { pipeW = 1920; pipeH = 1080; } }
    ALOGI("Pipeline resolution: %ux%u (from %ux%u stream)",
          pipeW, pipeH, pipelineStream->width, pipelineStream->height);

    // Early exit if same resolution - avoids costly STREAMOFF/STREAMON cycle
    if (dev->last_config_width == pipeW &&
        dev->last_config_height == pipeH &&
        dev->streams_configured && dev->pipeline) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        if (p->getState() == mocha::PIPELINE_STREAMING) {
            ALOGI("configureStreams: same %ux%u capture as last - no-op reinit",
                  pipeW, pipeH);
            return 0;
        }
        // NOTE: removed PIPELINE_OPENED early exit - sensor can get into bad state,
        // need full reconfigure to power cycle it
        ALOGI("configureStreams: same resolution but pipeline not streaming, reconfiguring");
    }

    // Error-complete any in-flight requests before reconfiguring
    if (dev->inflight_tracker && dev->inflight_tracker->count() > 0) {
        ALOGI("configureStreams: draining %zu in-flight requests", dev->inflight_tracker->count());
        std::vector<uint32_t> frames = dev->inflight_tracker->drainAll();
        for (uint32_t frameNum : frames) {
            camera3_capture_result_t result;
            memset(&result, 0, sizeof(result));
            result.frame_number = frameNum;
            result.result = nullptr;
            result.num_output_buffers = 0;
            result.output_buffers = nullptr;
            result.partial_result = 0;
            dev->callback_ops->process_capture_result(dev->callback_ops, &result);
            ALOGI("Drained in-flight frame %u", frameNum);
        }
    }

    // Close previous pipeline if exists
    if (dev->pipeline) {
        ALOGI("Closing previous pipeline for reconfiguration");
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
    }

    mocha::CameraPipeline* pipeline = new mocha::CameraPipeline();
    if (!pipeline) {
        ALOGE("Failed to create pipeline");
        return -ENOMEM;
    }

    int ret = pipeline->open(dev->camera_id);
    if (ret != 0) {
        ALOGE("Failed to open pipeline: %d (V4L2 device may not be available)", ret);
        delete pipeline;
        pipeline = nullptr;
    }

    if (!pipeline) {
        ALOGE("configureStreams: cannot proceed without pipeline");
        return -ENODEV;
    }

    mocha::PipelineConfig pipelineConfig;
    pipelineConfig.width = pipeW;
    pipelineConfig.height = pipeH;
    /* Bayer pattern must match the V4L2 format requested below:
     * camera 0 (IMX179) uses SRGGB10 -> RGGB (pat 3); camera 1 (OV5693)
     * uses SBGGR10 -> BGGR (pat 2). Sharing one value gave the front
     * camera a strong green cast (G/B channels swapped). */
    pipelineConfig.bayerPattern = (dev->camera_id == 0) ? 3 : 1;
    pipelineConfig.offset_x = 0;
    pipelineConfig.offset_y = 0;
    pipelineConfig.flipV = false;  // IMX179 mount normal; framework handles rotation

    pipelineConfig.enableISP = true;

     /* 【画面全黑修复 2026-11-16】blackLevel 单位是 raw10_to_8bit 输出的
      * 8-bit 刻度（代码里 v>>2 之后才减）。IMX179 10-bit 黑电平典型 64
      * （10-bit 域），换算到 8-bit 是 64/4=16。上一轮误把 10-bit 的 64
      * 直接填进来，而夜间画面均值 239(10-bit)=59(8-bit)<64，被饱和减法
      * 钳到 0，整幅预览全黑（AE luma=0.00）。改为 16。 */
     pipelineConfig.blackLevel = 16;
     pipelineConfig.wbGain[0] = 1.0f;  // R gain
     pipelineConfig.wbGain[1] = 1.0f;  // G gain
     pipelineConfig.wbGain[2] = 1.0f;  // B gain
     pipelineConfig.wbGain[3] = 1.0f;
     pipelineConfig.ccm[0] = 1.0f; pipelineConfig.ccm[1] = 0.0f; pipelineConfig.ccm[2] = 0.0f;
     pipelineConfig.ccm[3] = 0.0f; pipelineConfig.ccm[4] = 1.0f; pipelineConfig.ccm[5] = 0.0f;
     pipelineConfig.ccm[6] = 0.0f; pipelineConfig.ccm[7] = 0.0f; pipelineConfig.ccm[8] = 1.0f;
     pipelineConfig.gamma = 0.55f;

     // Auto Exposure y Auto White Balance
     pipelineConfig.enableAE = true;
     pipelineConfig.enableAWB = true;  // corrige tinte verdoso
    pipelineConfig.targetLuma = 0.55f;
    pipelineConfig.digitalGain = 1.5f;  // modest boost; 6.0f caused clipping + noise
 
    // Override IMPLEMENTATION_DEFINED to RGBA_8888 (Tegra gralloc allocates
    // RGBA for non-YUV formats). Keep YCbCr_420_888 and BLOB as-is.
    /* 每次 configure 前清空编码器流登记，避免上次会话的悬空指针。 */
    for (int k = 0; k < 4; k++) gEncoderStreams[k] = nullptr;
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        if (stream->stream_type == CAMERA3_STREAM_OUTPUT &&
            stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            /* 【录像修复 2026-11-17】录像时框架多配一条"编码器输入流"
             * （usage 带 GRALLOC_USAGE_HW_VIDEO_ENCODER）。MediaCodec 的
             * input Surface 只接受 YUV，之前把所有 PRIV 流统一改成 RGBA_8888，
             * 编码器拿到 RGBA 缓冲后一帧都出不来（CameraX 报
             * ERROR_NO_VALID_DATA、screenrecord 也是 recorded 0 frames）。
             * 这里把编码器流保留为 YCbCr_420_888，pipeline 原生支持写 YUV；
             * 预览流仍然用 RGBA_8888。 */
            if ((stream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0) {
                /* 【2026-11-18】真机 GraphiBuffer 构造日志已定性：
                 *   err=3 (GRALLOC1_ERROR_UNSUPPORTED) fmt=0x23 usage=0x10033 mapperVersion=2
                 * 即 **vendor gralloc1 不支持 YCbCr_420_888 + HW_VIDEO_ENCODER** 这个组合，
                 * 编码器输入缓冲根本分配不出来。改用 YV12 —— 老 NVIDIA 相机/编码路径
                 * 一直支持的格式，我们的 fan-out 也有 YV12 铺平面的兜底分支。 */
                stream->format = HAL_PIXEL_FORMAT_YV12;
                stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                for (int k = 0; k < 4; k++) {
                    if (gEncoderStreams[k] == nullptr) {
                        gEncoderStreams[k] = stream;
                        break;
                    }
                }
                /* 【2026-11-18】回到 4：实测 max_buffers=2 时 Camera3 的 dequeue 直接
                 * 返回 -EINVAL（mocha-dequeue 日志：max_buffers=2 total=4 handedOut=0），
                 * 4 个槽位时至少能正常 dequeue、录制能起来。 */
                if (stream->max_buffers < 4) {
                    stream->max_buffers = 4;
                }
                ALOGI("stream %u is the video encoder input (format stays %#x, max_buffers=%u)",
                      i, stream->format, stream->max_buffers);
            } else {
                stream->format = HAL_PIXEL_FORMAT_RGBA_8888;
            }
        }
    }
    uint32_t v4l2Format = (dev->camera_id == 0) ? V4L2_PIX_FMT_SRGGB10 : V4L2_PIX_FMT_SBGGR10;

    pipelineConfig.pixelFormat = v4l2Format;

    ret = pipeline->configure(pipelineConfig);
    if (ret != 0) {
        ALOGE("Failed to configure pipeline: %d", ret);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
        return ret;
    }

    ret = pipeline->startStreaming();
    if (ret != 0) {
        ALOGE("Failed to start streaming: %d", ret);
        pipeline->close();
        delete pipeline;
        dev->pipeline = nullptr;
        return ret;
    }

    dev->pipeline = pipeline;
    dev->output_stream = outputStream;
    dev->pipeline_width = pipeW;
    dev->pipeline_height = pipeH;
    dev->last_config_width = pipeW;
    dev->last_config_height = pipeH;

    dev->streams_configured = true;
    ALOGI("Streams configured successfully: pipeline=%dx%d preview=%dx%d BLOB=%s", 
          dev->pipeline_width, dev->pipeline_height,
          pipelineStream->width, pipelineStream->height,
          (outputStream->format == HAL_PIXEL_FORMAT_BLOB) ? "yes" : "no");
    return 0;
}

static const camera_metadata_t* camera_device_construct_default_request_settings(const camera3_device_t *device, int type) {
    ALOGI("camera_device_construct_default_request_settings: type=%d", type);
    
    if (!device) {
        ALOGE("Null device pointer");
        return nullptr;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    int cameraId = dev->camera_id;
    
    size_t entry_capacity = 20;
    size_t data_capacity = 80;
    camera_metadata_t* metadata = allocate_camera_metadata(entry_capacity, data_capacity);
    if (!metadata) {
        ALOGE("Failed to allocate metadata");
        return nullptr;
    }

    // Common settings for all templates
    uint8_t controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);
    uint8_t metadataMode = ANDROID_REQUEST_METADATA_MODE_FULL;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_METADATA_MODE, &metadataMode, 1);
    int32_t requestId = 0;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_ID, &requestId, 1);
    
    int32_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
    int32_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
    int32_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MODE, &controlMode, 1);
    int32_t sceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_SCENE_MODE, &sceneMode, 1);
    int32_t aeTargetFpsRange[] = {15, 30};
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, aeTargetFpsRange, 2);
    int32_t aePrecaptureTrigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &aePrecaptureTrigger, 1);
    uint8_t afMode = ANDROID_CONTROL_AF_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_MODE, &afMode, 1);
    uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);
    int32_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
    int32_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);
    int32_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_EFFECT_MODE, &effectMode, 1);
    uint8_t antibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_ANTIBANDING_MODE, &antibandingMode, 1);
    int32_t videoStabilizationMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &videoStabilizationMode, 1);
    int32_t edgeMode = ANDROID_EDGE_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_EDGE_MODE, &edgeMode, 1);
    int32_t nrMode = ANDROID_NOISE_REDUCTION_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_NOISE_REDUCTION_MODE, &nrMode, 1);
    int32_t colorCorrectMode = ANDROID_COLOR_CORRECTION_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_MODE, &colorCorrectMode, 1);
    int32_t transformMatrix[] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_TRANSFORM, transformMatrix, 9);
    int32_t gains[] = {1, 0, 1, 0};
    add_camera_metadata_entry(metadata, ANDROID_COLOR_CORRECTION_GAINS, gains, 4);
    int32_t tonemapMode = ANDROID_TONEMAP_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_TONEMAP_MODE, &tonemapMode, 1);
    int32_t shadingMode = ANDROID_SHADING_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_SHADING_MODE, &shadingMode, 1);
    int32_t lensShadingMapMode = ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, &lensShadingMapMode, 1);
    int32_t hotPixelMode = ANDROID_HOT_PIXEL_MODE_FAST;
    add_camera_metadata_entry(metadata, ANDROID_HOT_PIXEL_MODE, &hotPixelMode, 1);
    int32_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_STATISTICS_FACE_DETECT_MODE, &faceDetectMode, 1);
    int32_t testPatternMode = ANDROID_SENSOR_TEST_PATTERN_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_TEST_PATTERN_MODE, &testPatternMode, 1);

    // Template-specific settings
    switch (type) {
        case CAMERA3_TEMPLATE_PREVIEW:
            ALOGI("Using preview template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:
            ALOGI("Using video record template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            break;
        case CAMERA3_TEMPLATE_STILL_CAPTURE: {
            ALOGI("Using still capture template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            uint8_t jpegQuality = 95;
            add_camera_metadata_entry(metadata, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
            uint8_t thumbnailQuality = 95;
            add_camera_metadata_entry(metadata, ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbnailQuality, 1);
            int32_t thumbnailSize[] = {320, 240};
            add_camera_metadata_entry(metadata, ANDROID_JPEG_THUMBNAIL_SIZE, thumbnailSize, 2);
            break;
        }
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG:
            ALOGI("Using ZSL template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;
            break;
        case CAMERA3_TEMPLATE_MANUAL:
            ALOGI("Using manual template");
            controlIntent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
            aeMode = ANDROID_CONTROL_AE_MODE_OFF;
            add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
            awbMode = ANDROID_CONTROL_AWB_MODE_OFF;
            add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
            break;
        default:
            ALOGW("Unknown request template %d, using preview defaults", type);
            break;
    }
    
    // Update intent after override
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_CAPTURE_INTENT, &controlIntent, 1);

    sort_camera_metadata(metadata);
    ALOGI("Request template %d created for camera %d (entries=%zu)", type, cameraId, get_camera_metadata_entry_count(metadata));
    return metadata;
}

static camera_metadata_t* build_result_metadata(uint32_t frameNumber, int64_t timestamp, int32_t exposureVal, int32_t sensitivity, int afState, int focusPos) {
    camera_metadata_t* metadata = allocate_camera_metadata(30, 1024);
    if (!metadata) return nullptr;

    /* ANDROID_CONTROL_AE_MODE */
    uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_MODE, &aeMode, 1);

    /* ANDROID_CONTROL_AE_STATE */
    uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AE_STATE, &aeState, 1);

    /* ANDROID_CONTROL_AWB_MODE */
    uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);

    /* ANDROID_CONTROL_AWB_STATE */
    uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AWB_STATE, &awbState, 1);

    /* ANDROID_CONTROL_MODE */
    uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_MODE, &controlMode, 1);

    /* ANDROID_FLASH_MODE */
    uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
    add_camera_metadata_entry(metadata, ANDROID_FLASH_MODE, &flashMode, 1);

    /* ANDROID_JPEG_QUALITY */
    uint8_t jpegQuality = 95;
    add_camera_metadata_entry(metadata, ANDROID_JPEG_QUALITY, &jpegQuality, 1);

    /* ANDROID_LENS_FOCUS_DISTANCE: map position 0-1023 to diopters 0.0-10.0 */
    float focusDistance = (focusPos > 0) ? 10.0f * focusPos / 1023.0f : 0.0f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_FOCUS_DISTANCE, &focusDistance, 1);

    /* ANDROID_LENS_STATE */
    uint8_t lensState;
    switch (afState) {
        case 0:  lensState = ANDROID_LENS_STATE_STATIONARY; break;
        case 1:  lensState = ANDROID_LENS_STATE_MOVING; break;
        case 4:  lensState = ANDROID_LENS_STATE_STATIONARY; break;
        default: lensState = ANDROID_LENS_STATE_STATIONARY; break;
    }
    add_camera_metadata_entry(metadata, ANDROID_LENS_STATE, &lensState, 1);

    /* ANDROID_CONTROL_AF_MODE */
    uint8_t resultAfMode = ANDROID_CONTROL_AF_MODE_AUTO;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_MODE, &resultAfMode, 1);

    /* ANDROID_CONTROL_AF_TRIGGER */
    uint8_t resultAfTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
    add_camera_metadata_entry(metadata, ANDROID_CONTROL_AF_TRIGGER, &resultAfTrigger, 1);

    /* ANDROID_REQUEST_ID */
    int32_t requestId = (int32_t)frameNumber;
    add_camera_metadata_entry(metadata, ANDROID_REQUEST_ID, &requestId, 1);

    /* ANDROID_SCALER_CROP_REGION - full active array (16:9, see init_static_characteristics) */
    /* 【2026-11-18】原来硬编码 3280x2464（4:3），前摄也照抄；现在跟随 16:9 有效阵列。 */
    int32_t cropRegion[4] = {
        gAppliedCrop[0], gAppliedCrop[1], gAppliedCrop[2], gAppliedCrop[3]
    };
    if (cropRegion[2] <= 0 || cropRegion[3] <= 0) {
        cropRegion[2] = (gActiveCameraId == 0) ? 3280 : 2624;
        cropRegion[3] = (gActiveCameraId == 0) ? 1845 : 1104;
    }
    add_camera_metadata_entry(metadata, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    /* report the zoom ratio that matches the applied crop */
    {
        float reportedZoom = 1.0f;
        int32_t fullW = (gActiveCameraId == 0) ? 3280 : 2624;
        if (cropRegion[2] > 0 && cropRegion[2] < fullW)
            reportedZoom = (float)fullW / (float)cropRegion[2];
        add_camera_metadata_entry(metadata, ANDROID_CONTROL_ZOOM_RATIO, &reportedZoom, 1);
    }

    /* ANDROID_SENSOR_EXPOSURE_TIME (ns) */
    int64_t expTimeNs = (int64_t)exposureVal * 1000LL;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_EXPOSURE_TIME, &expTimeNs, 1);

    /* ANDROID_SENSOR_FRAME_DURATION (ns for 30fps) */
    int64_t frameDuration = 33333333LL;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_FRAME_DURATION, &frameDuration, 1);

    /* ANDROID_SENSOR_SENSITIVITY */
    int32_t sensorSensitivity = sensitivity;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_SENSITIVITY, &sensorSensitivity, 1);

    /* ANDROID_SENSOR_TIMESTAMP */
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_TIMESTAMP, &timestamp, 1);

    sort_camera_metadata(metadata);
    return metadata;
}

// Copy a captured frame from one gralloc buffer to another (same format/dimensions).
// Used to fan a single captured frame out to every output stream (preview + video).
// 【拍照保存修复 2026-11-17】改用 GraphicBufferMapper（mapper@2.1），与 App 侧
// 锁语义一致，避免 NVIDIA 跨进程 LockState 残留。
/* 【录像修复 2026-11-17】RGBA_8888 最近邻缩放。
 * 录像时框架会配两个 output 流：预览流和编码器输入流，两者分辨率常常不同
 * （例如预览 1280x720、录像 1920x1080）。原来 copyFrameBetweenBuffers 遇到
 * 尺寸不同直接 return，导致**编码器那条流从来没被写过**，MediaCodec 等不到
 * 关键帧，CameraX 最终报 ERROR_NO_VALID_DATA、DCIM 里没有 mp4。
 * 这里按目标分辨率做一次最近邻缩放（先预算列映射，内层只有查表和拷贝，
 * 720p→1080p 在 TK1 上大约几十毫秒，比双线性省一半以上内存带宽）。 */
static void scaleRgbaNearest(const uint8_t* src, int srcW, int srcH,
                             uint8_t* dst, int dstW, int dstH) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    /* 列映射：dstX -> srcX，避免每像素做除法 */
    std::vector<int> xmap(dstW);
    for (int x = 0; x < dstW; x++)
        xmap[x] = (int)((int64_t)x * srcW / dstW);

    for (int y = 0; y < dstH; y++) {
        int srcY = (int)((int64_t)y * srcH / dstH);
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(
                src + (size_t)srcY * srcW * 4);
        uint32_t* drow = reinterpret_cast<uint32_t*>(
                dst + (size_t)y * dstW * 4);
        for (int x = 0; x < dstW; x++)
            drow[x] = srow[xmap[x]];
    }
}

/* 【录像修复 2026-11-17】RGBA_8888 -> YUV420（BT.601，limited range）。
 * 编码器输入 Surface 的缓冲是 YUV，而 pipeline 出的画面是 RGBA，
 * 所以 fan-out 到编码器那条流时必须做一次颜色空间转换。
 * android_ycbcr 的布局由 gralloc 决定：chroma_step==2 是 NV12/NV21 那样的
 * 半平面（Cb/Cr 交错），==1 是 I420 的独立平面，这里两种都支持。 */
static void rgbaToYuv420Scaled(const uint8_t* rgba, int srcW, int srcH,
                               int dstW, int dstH, struct android_ycbcr* out) {
    if (!rgba || !out || !out->y || !out->cb || !out->cr) return;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    const int w = dstW, h = dstH;

    uint8_t* yPlane = static_cast<uint8_t*>(out->y);
    uint8_t* uPlane = static_cast<uint8_t*>(out->cb);
    uint8_t* vPlane = static_cast<uint8_t*>(out->cr);
    const size_t yStride = out->ystride;
    const size_t cStride = out->cstride;
    const size_t cStep = out->chroma_step ? out->chroma_step : 1;

    for (int y = 0; y < h; y++) {
        const uint8_t* row = rgba + (size_t)((int64_t)y * srcH / h) * srcW * 4;
        uint8_t* yRow = yPlane + (size_t)y * yStride;
        for (int x = 0; x < w; x++) {
            int sx = (int)((int64_t)x * srcW / w);
            int r = row[sx * 4 + 0];
            int g = row[sx * 4 + 1];
            int b = row[sx * 4 + 2];
            int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[x] = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
        }
    }

    for (int y = 0; y < h; y += 2) {
        int sy0 = (int)((int64_t)y * srcH / h);
        int sy1 = (int)((int64_t)(y + 1 < h ? y + 1 : y) * srcH / h);
        const uint8_t* row0 = rgba + (size_t)sy0 * srcW * 4;
        const uint8_t* row1 = rgba + (size_t)sy1 * srcW * 4;
        uint8_t* uRow = uPlane + (size_t)(y / 2) * cStride;
        uint8_t* vRow = vPlane + (size_t)(y / 2) * cStride;
        for (int x = 0; x < w; x += 2) {
            int x1 = (x + 1 < w) ? x + 1 : x;
            int sx0 = (int)((int64_t)x * srcW / w);
            int sx1 = (int)((int64_t)x1 * srcW / w);
            int r = (row0[sx0 * 4 + 0] + row0[sx1 * 4 + 0] + row1[sx0 * 4 + 0] + row1[sx1 * 4 + 0]) >> 2;
            int g = (row0[sx0 * 4 + 1] + row0[sx1 * 4 + 1] + row1[sx0 * 4 + 1] + row1[sx1 * 4 + 1]) >> 2;
            int b = (row0[sx0 * 4 + 2] + row0[sx1 * 4 + 2] + row1[sx0 * 4 + 2] + row1[sx1 * 4 + 2]) >> 2;
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            u = u < 0 ? 0 : (u > 255 ? 255 : u);
            v = v < 0 ? 0 : (v > 255 ? 255 : v);
            uRow[(x / 2) * cStep] = (uint8_t)u;
            vRow[(x / 2) * cStep] = (uint8_t)v;
        }
    }
}

static void copyFrameBetweenBuffers(
        const camera3_stream_buffer_t* src, const camera3_stream_buffer_t* dst) {
    if (!src || !dst || !src->buffer || !dst->buffer) return;
    const bool srcRgba = (src->stream->format == HAL_PIXEL_FORMAT_RGBA_8888);
    /* 【2026-11-18 方案1】编码器流的 format 现在是 IMPLEMENTATION_DEFINED（没改写），
     * 这里用内部标记把它仍然按 YUV 目标处理，走 NV12/YV12 写入路径。 */
    const bool dstYuv  = (dst->stream->format == HAL_PIXEL_FORMAT_YCBCR_420_888) ||
                         isEncoderStream(dst->stream);
    if (src->stream->format != dst->stream->format && !(srcRgba && dstYuv)) {
        ALOGW("copyFrameBetweenBuffers: unsupported format %d -> %d",
              src->stream->format, dst->stream->format);
        return;
    }
    const bool needScale =
            (src->stream->width != dst->stream->width ||
             src->stream->height != dst->stream->height);
    if (needScale) {
        /* 不再直接放弃：缩放到目标流分辨率（见 scaleRgbaNearest 注释）。
         * 只在尺寸组合变化时打一条日志，避免每帧刷屏。 */
        static uint32_t lastSrcW = 0, lastSrcH = 0, lastDstW = 0, lastDstH = 0;
        if (lastSrcW != src->stream->width || lastSrcH != src->stream->height ||
            lastDstW != dst->stream->width || lastDstH != dst->stream->height) {
            lastSrcW = src->stream->width; lastSrcH = src->stream->height;
            lastDstW = dst->stream->width; lastDstH = dst->stream->height;
            ALOGI("copyFrameBetweenBuffers: scaling %ux%u -> %ux%u",
                  src->stream->width, src->stream->height,
                  dst->stream->width, dst->stream->height);
        }
    }
    void* srcVaddr = nullptr;
    void* dstVaddr = nullptr;
    const android::Rect srcRect((int)src->stream->width, (int)src->stream->height);
    const android::Rect dstRect((int)dst->stream->width, (int)dst->stream->height);
    int ret = android::GraphicBufferMapper::get().lock(*src->buffer,
                GRALLOC_USAGE_SW_READ_OFTEN, srcRect, &srcVaddr);
    if (ret != 0 || !srcVaddr) {
        ALOGE("copyFrameBetweenBuffers: failed to lock src buffer: %d", ret);
        return;
    }
    if (dstYuv) {
        /* 目标是编码器那条 YUV 流：先用 lockYCbCr 拿到真实的平面布局，
         * 再把 RGBA 转成 YUV420 写进去（同时按需缩放）。
         *
         * 【录像修复 2026-11-17】原厂 gralloc 是新旧混杂的：README 里记着它对
         * YV12 的 lockYCbCr 直接返回 -ENOSYS（SoftwareRenderer 那次遇到过），
         * 但 HAL_PIXEL_FORMAT_YCBCR_420_888 走的是另一条分支，未必也不支持。
         * 所以这里两条路都走：lockYCbCr 能用就用它给的 ystride/cstride/
         * chroma_step；用不了就退化成 lock() + 按 YV12 手工铺平面。
         * 不管走哪条，第一次都打一行布局日志——下次真机验证时一眼就能看出
         * 编码器那条流到底是哪种布局。 */
        struct android_ycbcr ycbcr;
        memset(&ycbcr, 0, sizeof(ycbcr));
        ret = android::GraphicBufferMapper::get().lockYCbCr(*dst->buffer,
                    GRALLOC_USAGE_SW_WRITE_OFTEN, dstRect, &ycbcr);
        if (ret == 0 && ycbcr.y) {
            static bool layoutLogged = false;
            if (!layoutLogged) {
                layoutLogged = true;
                ALOGI("encoder YUV via lockYCbCr: ystride=%zu cstride=%zu "
                      "chroma_step=%zu", (size_t)ycbcr.ystride,
                      (size_t)ycbcr.cstride, (size_t)ycbcr.chroma_step);
            }
            rgbaToYuv420Scaled(static_cast<const uint8_t*>(srcVaddr),
                               (int)src->stream->width, (int)src->stream->height,
                               (int)dst->stream->width, (int)dst->stream->height,
                               &ycbcr);
            android::GraphicBufferMapper::get().unlock(*dst->buffer);
        } else {
            /* 退化路径：gralloc 不支持对这条流 lockYCbCr。
             * 假设内部就是 YV12（Y 平面 + V + U），stride 取宽——录像用的
             * 1280/1920 都是 32 的整数倍，原厂 YV12 stride 就是按 32 对齐的，
             * 这两个分辨率下 stride == width 成立。 */
            void* yuvVaddr = nullptr;
            int lockRet = android::GraphicBufferMapper::get().lock(*dst->buffer,
                    GRALLOC_USAGE_SW_WRITE_OFTEN, dstRect, &yuvVaddr);
            static bool fallbackLogged = false;
            if (lockRet == 0 && yuvVaddr) {
                const int w = (int)dst->stream->width;
                const int h = (int)dst->stream->height;
                const size_t ySize = (size_t)w * h;
                memset(&ycbcr, 0, sizeof(ycbcr));
                ycbcr.ystride = (size_t)w;
                ycbcr.cstride = (size_t)w / 2;
                ycbcr.chroma_step = 1;
                ycbcr.y = static_cast<uint8_t*>(yuvVaddr);
                /* YV12：V 平面在前，U 平面在后，各占 w*h/4 */
                ycbcr.cr = static_cast<uint8_t*>(yuvVaddr) + ySize;
                ycbcr.cb = static_cast<uint8_t*>(yuvVaddr) + ySize + ySize / 4;
                if (!fallbackLogged) {
                    fallbackLogged = true;
                    ALOGW("encoder YUV via lockYCbCr failed (%d), falling back "
                          "to lock() with an assumed YV12 layout, stride=%d", ret, w);
                }
                rgbaToYuv420Scaled(static_cast<const uint8_t*>(srcVaddr),
                                   (int)src->stream->width, (int)src->stream->height,
                                   w, h, &ycbcr);
                android::GraphicBufferMapper::get().unlock(*dst->buffer);
            } else {
                ALOGE("copyFrameBetweenBuffers: encoder buffer not lockable: "
                      "lockYCbCr=%d lock=%d", ret, lockRet);
            }
        }
        android::GraphicBufferMapper::get().unlock(*src->buffer);
        return;
    }

    ret = android::GraphicBufferMapper::get().lock(*dst->buffer,
                GRALLOC_USAGE_SW_WRITE_OFTEN, dstRect, &dstVaddr);
    if (ret != 0 || !dstVaddr) {
        android::GraphicBufferMapper::get().unlock(*src->buffer);
        ALOGE("copyFrameBetweenBuffers: failed to lock dst buffer: %d", ret);
        return;
    }
    // RGBA_8888: 4 bytes/pixel（configure_streams 已把所有非 BLOB 流改成 RGBA_8888）
    if (needScale) {
        scaleRgbaNearest(static_cast<const uint8_t*>(srcVaddr),
                         (int)src->stream->width, (int)src->stream->height,
                         static_cast<uint8_t*>(dstVaddr),
                         (int)dst->stream->width, (int)dst->stream->height);
    } else {
        size_t size = (size_t)dst->stream->width * dst->stream->height * 4;
        memcpy(dstVaddr, srcVaddr, size);
    }
    android::GraphicBufferMapper::get().unlock(*dst->buffer);
    android::GraphicBufferMapper::get().unlock(*src->buffer);
}

static int camera_device_process_capture_request(const camera3_device_t *device, camera3_capture_request_t *request) {
    if (!device || !request) {
        ALOGE("Invalid parameters");
        return -EINVAL;
    }

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    ALOGI("camera_device_process_capture_request: frame_number=%llu", (unsigned long long)request->frame_number);

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    
    if (!dev->is_initialized || !dev->callback_ops) {
        ALOGE("Camera not initialized");
        return -ENOSYS;
    }

    if (request->num_output_buffers < 1 || !request->output_buffers) {
        ALOGE("No output buffers");
        return -EINVAL;
    }

    // Check if this frame was marked as error by flush()
    uint32_t frameNum = request->frame_number;
    if (dev->inflight_tracker && dev->inflight_tracker->isError(frameNum)) {
        ALOGW("Frame %u marked as error by flush, returning EAGAIN", frameNum);
        dev->inflight_tracker->remove(frameNum);
        return -EAGAIN;
    }

    // Track this request
    if (dev->inflight_tracker) {
        dev->inflight_tracker->add(frameNum, request->output_buffers[0].buffer);
    }

    // Handle AF mode and trigger from request metadata
    if (request->settings) {
        camera_metadata_t* mutableSettings = const_cast<camera_metadata_t*>(request->settings);
        camera_metadata_entry_t entry;
        if (find_camera_metadata_entry(mutableSettings, ANDROID_CONTROL_AF_MODE, &entry) == 0 && entry.count > 0) {
            dev->af_mode = entry.data.u8[0];
        }
        if (find_camera_metadata_entry(mutableSettings, ANDROID_CONTROL_AF_TRIGGER, &entry) == 0 && entry.count > 0) {
            dev->af_trigger = entry.data.u8[0];
        }
        float focusDist = -1.0f;
        if (find_camera_metadata_entry(mutableSettings, ANDROID_LENS_FOCUS_DISTANCE, &entry) == 0 && entry.count > 0) {
            focusDist = entry.data.f[0];
        }
        ALOGI("AF request: mode=%d trigger=%d focusDist=%.2f", dev->af_mode, dev->af_trigger, focusDist);

        /* Digital zoom: honour both the classic SCALER_CROP_REGION and the
         * Android 12 CONTROL_ZOOM_RATIO. The active array is 16:9
         * (3280x1845 rear / 2624x1104 front); map the effective window
         * onto pipeline pixels and let the ISP sampler do the crop. */
        if (dev->pipeline) {
            mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
            const int32_t actW = (dev->camera_id == 0) ? 3280 : 2624;
            const int32_t actH = (dev->camera_id == 0) ? 1845 : 1104;
            float zoomRatio = 1.0f;
            if (find_camera_metadata_entry(mutableSettings, ANDROID_CONTROL_ZOOM_RATIO, &entry) == 0 && entry.count > 0) {
                zoomRatio = entry.data.f[0];
                if (zoomRatio < 1.0f) zoomRatio = 1.0f;
                if (zoomRatio > 4.0f) zoomRatio = 4.0f;
            }
            int32_t cx = 0, cy = 0, cw = actW, ch = actH;
            if (zoomRatio > 1.0f) {
                cw = (int32_t)(actW / zoomRatio) & ~1;
                ch = (int32_t)(actH / zoomRatio) & ~1;
                cx = (actW - cw) / 2 & ~1;
                cy = (actH - ch) / 2 & ~1;
            } else if (find_camera_metadata_entry(mutableSettings, ANDROID_SCALER_CROP_REGION, &entry) == 0 && entry.count >= 4) {
                cx = entry.data.i32[0]; cy = entry.data.i32[1];
                cw = entry.data.i32[2]; ch = entry.data.i32[3];
                if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0) { cx = 0; cy = 0; cw = actW; ch = actH; }
                if (cw > actW) cw = actW;
                if (ch > actH) ch = actH;
                if (cx + cw > actW) cx = actW - cw;
                if (cy + ch > actH) cy = actH - ch;
            }
            /* active-array pixels -> pipeline pixels */
            int px = (int)((int64_t)cx * dev->pipeline_width / actW);
            int py = (int)((int64_t)cy * dev->pipeline_height / actH);
            int pw2 = (int)((int64_t)cw * dev->pipeline_width / actW);
            int ph2 = (int)((int64_t)ch * dev->pipeline_height / actH);
            p->setCrop(px, py, pw2, ph2);
            gAppliedCrop[0] = cx; gAppliedCrop[1] = cy;
            gAppliedCrop[2] = cw; gAppliedCrop[3] = ch;
        }
    }

    // 【2026-11-17 修正】前摄 OV5693 定焦、没有 VCM，这里只让后摄走 AF 流程。
    // 前摄就算 framework 误发 AF 请求也不再空跑对比度扫描（每次扫描要独占
    // V4L2 若干帧，会让前摄预览卡顿）。
    if (dev->pipeline && dev->camera_id == 0) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        if (dev->af_mode == ANDROID_CONTROL_AF_MODE_AUTO ||
            dev->af_mode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE) {
            if (dev->af_trigger == ANDROID_CONTROL_AF_TRIGGER_START && !dev->af_trigger_handled) {
                ALOGI("AF TRIGGER START received");
                p->startAfScan();
                dev->af_trigger_handled = true;
            } else if (dev->af_mode == ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE &&
                       dev->af_trigger == ANDROID_CONTROL_AF_TRIGGER_IDLE &&
                       !dev->af_trigger_handled) {
                /* 【修复 2026-11-17】CONTINUOUS_PICTURE 模式下 framework 只发
                 * mode=4/trigger=0（IDLE），从不发 AF_TRIGGER_START —— 它期望
                 * HAL 自动持续对焦。原代码只在 trigger==START 时扫描，于是
                 * Aperture 全程不对焦、画面一直糊（实测 10:05 日志
                 * "AF request: mode=4 trigger=0" 连续出现，无任何 AF: coarse）。
                 * 这里在连续对焦模式下也主动跑一次扫描（场景变化由后续
                 * 重新打开或 preview 重启再触发），至少让用户拍到合焦画面。 */
                ALOGI("AF: CONTINUOUS_PICTURE, running initial scan");
                p->startAfScan();
                dev->af_trigger_handled = true;
            } else if (dev->af_trigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL) {
                ALOGI("AF TRIGGER CANCEL received");
                p->cancelAf();
                dev->af_trigger_handled = false;
            }
        }
    }

    /* 【录像修复 2026-11-17】优先用 RGBA 的流做采集目标。
     * 录像时两条流里编码器那条是 YUV，如果从它开始采集，fan-out 就得做
     * YUV->RGBA 转换（没实现、也更贵）；反过来以 RGBA 预览流为源、向编码器
     * 流做 RGBA->YUV，两条流都能拿到正确内容，代价也小。 */
    const camera3_stream_buffer_t* captureBuf = &request->output_buffers[0];
    for (uint32_t bi = 0; bi < request->num_output_buffers; bi++) {
        if (request->output_buffers[bi].stream &&
            request->output_buffers[bi].stream->format == HAL_PIXEL_FORMAT_RGBA_8888) {
            captureBuf = &request->output_buffers[bi];
            break;
        }
    }
    const camera3_stream_buffer_t& buf = *captureBuf;
    
    if (!buf.buffer) {
        ALOGE("Invalid buffer");
        if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
        return -EINVAL;
    }

    // Wait for acquire_fence before writing to buffer
    int fence = buf.acquire_fence;
    if (fence >= 0) {
        // Wait up to 2000ms for the buffer to be ready
        struct pollfd pfd;
        pfd.fd = fence;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pollRet = poll(&pfd, 1, 2000);
        if (pollRet > 0 && (pfd.revents & POLLIN)) {
            // Fence signaled, buffer is ready
            ALOGV("Acquire fence signaled for frame %u", frameNum);
        } else if (pollRet == 0) {
            ALOGW("Acquire fence timeout for frame %u, proceeding anyway", frameNum);
        } else {
            ALOGW("Acquire fence poll error for frame %u: %s", frameNum, strerror(errno));
        }
        close(fence);
    }

    ALOGI("Processing capture: frame=%llu stream=%dx%d format=%d pipeline=%dx%d",
          (unsigned long long)request->frame_number,
          buf.stream->width, buf.stream->height, buf.stream->format,
          dev->pipeline_width, dev->pipeline_height);

 
    bool frameCaptured = false;

    // Capture frame from pipeline
    if (dev->pipeline && dev->streams_configured) {
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);

        // Auto-restart pipeline if it was stopped by flush()
        if (pipeline->getState() == mocha::PIPELINE_OPENED) {
            ALOGI("process_capture_request: pipeline in OPENED state, re-starting streaming");
            int restartRet = pipeline->startStreaming();
            if (restartRet != 0) {
                ALOGE("Auto-restart streaming failed: %d", restartRet);
                if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                return -ENODEV;
            }
        }

        buffer_handle_t handle = *buf.buffer;
        void* vaddr = nullptr;

        /* BLOB format = JPEG still capture */
        if (buf.stream->format == HAL_PIXEL_FORMAT_BLOB) {
            /* Allocate temp RGBA buffer at pipeline resolution */
            uint32_t rgbaSize = dev->pipeline_width * dev->pipeline_height * 4;
            if (!dev->temp_rgba || dev->temp_rgba_size < rgbaSize) {
                if (dev->temp_rgba) free(dev->temp_rgba);
                dev->temp_rgba = (uint8_t*)malloc(rgbaSize);
                dev->temp_rgba_size = rgbaSize;
            }
            if (!dev->temp_rgba) {
                ALOGE("Failed to allocate temp RGBA buffer");
            } else {
                int captureRet = pipeline->captureFrameRgba(dev->temp_rgba,
                        dev->pipeline_width, dev->pipeline_height);
                if (captureRet == 0) {
                    /* BLOB 缓冲由 framework 按 (字节容量, 1) 分配，锁的时候必须
                     * 用同样的尺寸，否则部分 gralloc 会按 1280x720 算出别的 stride。 */
                    uint32_t blobW = buf.stream->width;
                    uint32_t blobH = buf.stream->height;
                    const size_t blobCapacity = jpegBufferCapacity(blobW, blobH);
                    const size_t payloadCapacity = blobCapacity - sizeof(camera3_jpeg_blob_t);
                    const android::Rect blobRect((int)blobCapacity, 1);
                    int ret = android::GraphicBufferMapper::get().lock(handle,
                                   GRALLOC_USAGE_SW_WRITE_OFTEN, blobRect, &vaddr);
                    if (ret == 0 && vaddr) {
                        /* Initialize JPEG encoder */
                        if (!dev->jpeg_encoder) {
                            dev->jpeg_encoder = new mocha::JpegEncoder();
                        }
                        /* Encode RGBA to JPEG at pipeline resolution into blob buffer */
                        size_t jpegSize = 0;
                        ret = dev->jpeg_encoder->encodeRGBA(dev->temp_rgba,
                                                              dev->pipeline_width, dev->pipeline_height,
                                                              90, (uint8_t*)vaddr, payloadCapacity, &jpegSize);
                        if (ret == 0 && jpegSize > 0) {
                            /* 尾标必须写在缓冲最后 8 字节：framework 会从
                             * getWidth()-sizeof(camera3_jpeg_blob_t) 处读它，
                             * 再转成 AIDL 的 CameraBlob 头交给 App。 */
                            camera3_jpeg_blob_t blob;
                            blob.jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
                            blob.jpeg_size = (uint32_t)jpegSize;
                            uint8_t* blobPtr = (uint8_t*)vaddr + payloadCapacity;
                            memcpy(blobPtr, &blob, sizeof(blob));
                            frameCaptured = true;
                            ALOGI("JPEG captured: pipeline=%dx%d blob=%dx%d capacity=%zu -> %zu bytes",
                                  dev->pipeline_width, dev->pipeline_height,
                                  blobW, blobH, blobCapacity, jpegSize);
                        } else {
                            ALOGE("JPEG encode failed: %d", ret);
                        }
                        android::GraphicBufferMapper::get().unlock(handle);
                    } else {
                        ALOGE("Failed to lock BLOB buffer: %d", ret);
                    }
                } else if (captureRet == -EAGAIN) {
                    if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                    return -EAGAIN;
                } else {
                    ALOGE("Failed to capture frame for JPEG: %d", captureRet);
                }
            }
        } else if (buf.stream->format == HAL_PIXEL_FORMAT_YCBCR_420_888 ||
                   buf.stream->format == HAL_PIXEL_FORMAT_YV12) {
            struct android_ycbcr ycbcr;
            memset(&ycbcr, 0, sizeof(ycbcr));
            const android::Rect yuvRect((int)buf.stream->width, (int)buf.stream->height);
            int ret = android::GraphicBufferMapper::get().lockYCbCr(handle,
                               GRALLOC_USAGE_SW_WRITE_OFTEN, yuvRect, &ycbcr);
            if (ret == 0 && ycbcr.y) {
                /* Render at the pipeline resolution (with the zoom crop
                 * applied), then convert/scale into the YUV planes. Same
                 * conversion the encoder fan-out always used. */
                uint32_t rgbaSize = dev->pipeline_width * dev->pipeline_height * 4;
                if (!dev->temp_rgba || dev->temp_rgba_size < rgbaSize) {
                    if (dev->temp_rgba) free(dev->temp_rgba);
                    dev->temp_rgba = (uint8_t*)malloc(rgbaSize);
                    dev->temp_rgba_size = rgbaSize;
                }
                int captureRet = -ENOMEM;
                if (dev->temp_rgba) {
                    captureRet = pipeline->captureFrameRgba(dev->temp_rgba,
                            dev->pipeline_width, dev->pipeline_height);
                    if (captureRet == 0) {
                        rgbaToYuv420Scaled(dev->temp_rgba,
                                           (int)dev->pipeline_width, (int)dev->pipeline_height,
                                           (int)buf.stream->width, (int)buf.stream->height,
                                           &ycbcr);
                    }
                }
                if (captureRet == 0) {
                    frameCaptured = true;
                } else if (captureRet == -EAGAIN) {
                    android::GraphicBufferMapper::get().unlock(handle);
                    if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                    return -EAGAIN;
                }
                android::GraphicBufferMapper::get().unlock(handle);
            }
        } else {
            int usage = GRALLOC_USAGE_SW_WRITE_OFTEN;
            const android::Rect rect((int)buf.stream->width, (int)buf.stream->height);
            int ret = android::GraphicBufferMapper::get().lock(handle, usage, rect, &vaddr);
            if (ret == 0 && vaddr) {
                int captureRet = pipeline->captureFrameRgba(static_cast<uint8_t*>(vaddr),
                        buf.stream->width, buf.stream->height);
                if (captureRet == 0) {
                    frameCaptured = true;
                } else if (captureRet == -EAGAIN) {
                    android::GraphicBufferMapper::get().unlock(handle);
                    if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                    return -EAGAIN;
                }
                android::GraphicBufferMapper::get().unlock(handle);
            }
        }

        // Fan the captured frame out to every other output stream (preview + video).
        // The framework configures TWO output streams during recording (preview + video);
        // we only captured into output_buffers[0], so copy it to the rest.
        if (frameCaptured && buf.stream->format != HAL_PIXEL_FORMAT_BLOB) {
            for (uint32_t i = 0; i < request->num_output_buffers; i++) {
                if (&request->output_buffers[i] == captureBuf)
                    continue;
                copyFrameBetweenBuffers(&buf, &request->output_buffers[i]);
            }
        }
    }

    // Calculate processing time
    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    long elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 + (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
    ALOGI("Frame %u processing time: %ld ms", frameNum, elapsed_ms);

    // Determine if frame was flushed during capture
    bool frameFlushed = false;
    if (dev->inflight_tracker) {
        frameFlushed = dev->inflight_tracker->isError(frameNum);
        dev->inflight_tracker->remove(frameNum);
    }

    // Send SHUTTER notify callback BEFORE delivering the buffer
    if (frameCaptured && dev->callback_ops && dev->callback_ops->notify) {
        camera3_notify_msg_t notifyMsg;
        memset(&notifyMsg, 0, sizeof(notifyMsg));
        notifyMsg.type = CAMERA3_MSG_SHUTTER;
        notifyMsg.message.shutter.frame_number = request->frame_number;
        notifyMsg.message.shutter.timestamp = ((int64_t)ts_end.tv_sec * 1000000000LL) + (ts_end.tv_nsec);
        dev->callback_ops->notify(dev->callback_ops, &notifyMsg);
        ALOGI("Sent SHUTTER notify for frame %llu", (unsigned long long)request->frame_number);
    }

    // Determine buffer status: OK only if captured AND not flushed.
    // Build a result buffer for EVERY output stream (preview + video) so the
    // framework releases/uses all of them, not just the first one.
    static constexpr int kMaxOutputBuffers = 16;
    camera3_stream_buffer_t outputBufs[kMaxOutputBuffers];
    uint32_t numOutputBufs = 0;
    if (request->num_output_buffers > 0) {
        uint32_t n = request->num_output_buffers;
        if (n > kMaxOutputBuffers) n = kMaxOutputBuffers;
        for (uint32_t i = 0; i < n; i++) {
            outputBufs[i] = request->output_buffers[i];
            outputBufs[i].acquire_fence = -1;
            outputBufs[i].release_fence = -1;
            outputBufs[i].status = (frameCaptured && !frameFlushed) ?
                CAMERA3_BUFFER_STATUS_OK : CAMERA3_BUFFER_STATUS_ERROR;
        }
        numOutputBufs = n;
    }

    if (frameFlushed) {
        ALOGW("Frame %u was flushed during capture, sending result with ERROR status", frameNum);
    } else if (!frameCaptured) {
        /* Recoverable failure: the first requests after STREAMON race the
         * VI capture thread's lazy sensor start (power + init regs + s_stream
         * takes ~150ms on the first DQBUF), and DQBUF fails immediately
         * during that window. CAMERA3_MSG_ERROR_DEVICE is fatal - the
         * framework tears the camera down and the app exits - so report a
         * per-frame REQUEST error instead, which the framework simply
         * drops and retries on the next request. */
        ALOGW("Frame %u not captured, sending result with REQUEST error", frameNum);
        if (dev->callback_ops && dev->callback_ops->notify) {
            camera3_notify_msg_t notifyMsg;
            memset(&notifyMsg, 0, sizeof(notifyMsg));
            notifyMsg.type = CAMERA3_MSG_ERROR;
            notifyMsg.message.error.frame_number = request->frame_number;
            notifyMsg.message.error.error_code = CAMERA3_MSG_ERROR_REQUEST;
            dev->callback_ops->notify(dev->callback_ops, &notifyMsg);
        }
    }

    camera_metadata_t* resultMetadata = nullptr;
    if (frameCaptured && !frameFlushed) {
        int32_t exposure = 2400, sensitivity = 128;
        int afState = 0;
        int focusPos = 0;
        if (dev->pipeline) {
            mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
            exposure = p->getExposure();
            sensitivity = p->getGain();
            afState = p->getAfState();
            focusPos = p->getFocusPosition();
        }
        int64_t timestamp = ((int64_t)ts_end.tv_sec * 1000000000LL) + (ts_end.tv_nsec);
        resultMetadata = build_result_metadata(frameNum, timestamp, exposure, sensitivity, afState, focusPos);
    }

    camera3_capture_result_t result;
    memset(&result, 0, sizeof(result));
    result.frame_number = request->frame_number;
    result.result = resultMetadata;
    result.num_output_buffers = numOutputBufs;
    result.output_buffers = outputBufs;
    result.partial_result = 1;
    dev->callback_ops->process_capture_result(dev->callback_ops, &result);

    if (resultMetadata) {
        free_camera_metadata(resultMetadata);
    }

    ALOGI("Capture request completed: frame=%llu num_out=%u status=%d",
          (unsigned long long)request->frame_number, numOutputBufs,
          numOutputBufs > 0 ? outputBufs[0].status : -1);
    
    return 0;
}

static void camera_device_dump(const camera3_device_t *device, int fd) {
    ALOGI("camera_device_dump");
    
    if (!device) {
        return;
    }

    dprintf(fd, "Mocha Camera HAL - Device Dump\n");
    dprintf(fd, "================================\n");
    
    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    dprintf(fd, "Camera ID: %d\n", dev->camera_id);
    dprintf(fd, "Initialized: %s\n", dev->is_initialized ? "Yes" : "No");
    dprintf(fd, "Streams configured: %s\n", dev->streams_configured ? "Yes" : "No");
}

static int camera_device_flush(const camera3_device_t *device) {
    ALOGI("camera_device_flush");
    
    if (!device) {
        return -EINVAL;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)device;
    
    // Stop V4L2 streaming IMMEDIATELY to interrupt any threads blocked
    // in captureFrame() (poll/DQBUF). Without this, those threads stay
    // blocked for 1800ms per retry, causing the framework's drain to
    // hit the 30-second timeout, which keeps the camera "in use".
    if (dev->pipeline && dev->streams_configured) {
        mocha::CameraPipeline* p = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        p->stopStreaming();
    }
    
    // Mark any remaining in-flight requests as error and drain. The
    // threads that were in captureFrame() will return -EAGAIN (since
    // V4L2 is stopped), and the framework will handle those.
    if (dev->inflight_tracker) {
        dev->inflight_tracker->markAllAsError();
        std::vector<uint32_t> drained = dev->inflight_tracker->drainAll();
        ALOGI("Flush: drained %zu frames", drained.size());
    }
    
    ALOGI("Flush complete");
    return 0;
}

// Camera device open implementation
int MochaCameraHAL::openCamera(int cameraId, hw_device_t **device) {
    ALOGI("openCamera: cameraId=%d", cameraId);

    if (cameraId < 0 || cameraId >= MochaCameraHAL::kNumCameras) {
        ALOGE("Invalid camera ID: %d", cameraId);
        return -EINVAL;
    }

    int ret = camera_device_init(&::HMI.common, device);
    if (ret != 0) {
        ALOGE("Failed to initialize camera device: %d", ret);
        return ret;
    }

    mocha_camera_device_t *dev = (mocha_camera_device_t *)*device;
    dev->camera_id = cameraId;
    /* 【2026-11-18】result metadata 里要用到本路传感器的有效阵列尺寸（crop region），
     * 而 build_result_metadata() 拿不到 cameraId，这里用一个文件级变量记住当前
     * 打开的相机。以前那里硬编码 3280x2464，前摄（2624x1956）上报的 crop region
     * 比实际传感器还大，CameraX 做裁剪换算会算错。 */
    gActiveCameraId = cameraId;

    ALOGI("Camera %d opened successfully", cameraId);
    return 0;
}

} // namespace mocha

namespace mocha {

// Implementation of MochaCameraHAL
/*static*/ int MochaCameraHAL::getNumberOfCameras() {
    ALOGI("getNumberOfCameras: %d", MochaCameraHAL::kNumCameras);
    return MochaCameraHAL::kNumCameras;
}

/*static*/ int MochaCameraHAL::getCameraInfo(int cameraId, struct camera_info *info) {
    if (cameraId < 0 || cameraId >= MochaCameraHAL::kNumCameras) {
        ALOGE("Invalid camera ID: %d", cameraId);
        return -EINVAL;
    }

    const MochaCameraInfo& cam = MochaCameraHAL::kCameras[cameraId];
    info->facing = cam.facing;
    info->orientation = cam.orientation;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_2;
    info->static_camera_characteristics = init_static_characteristics(cameraId);
    info->resource_cost = 100;
    info->conflicting_devices = nullptr;
    info->conflicting_devices_length = 0;

    ALOGI("Camera info: id=%d, facing=%d, orientation=%d", cameraId, info->facing, info->orientation);

    return 0;
}

} // namespace mocha

// Module entry point
static int get_number_of_cameras() {
    return mocha::MochaCameraHAL::getNumberOfCameras();
}

static int get_camera_info(int camera_id, struct camera_info *info) {
    return mocha::MochaCameraHAL::getCameraInfo(camera_id, info);
}

static int open(const hw_module_t* module, const char* name, hw_device_t** device) {
    ALOGI("Camera HAL open: name=%s module=%p", name, module);

    if (!name) {
        ALOGE("Camera HAL open: null name");
        return -EINVAL;
    }

    int cameraId = atoi(name);
    int ret = mocha::MochaCameraHAL::openCamera(cameraId, device);
    ALOGI("Camera HAL open: returning %d", ret);
    return ret;
}

static hw_module_methods_t methods = {
    .open = open
};

// set_callbacks implementation
static int set_callbacks(const camera_module_callbacks_t *callbacks) {
    ALOGI("set_callbacks called");
    if (callbacks) {
        memcpy(&mocha::gModuleCallbacks, callbacks, sizeof(camera_module_callbacks_t));
    }
    return 0;
}

static int open_legacy(const hw_module_t* module, const char* id, uint32_t halVersion, hw_device_t** device) {
    ALOGI("Camera HAL open_legacy: id=%s halVersion=%u", id, halVersion);

    if (!id) {
        ALOGE("Camera HAL open_legacy: null id");
        return -EINVAL;
    }

    // halVersion=256 = 0x100 = CAMERA_DEVICE_API_VERSION_1_0
    // We reject HAL1 requests because we're HAL3-only.
    // For HAL3 requests (>= 0x30000), accept any version 3.x+.
    if (halVersion >= 0x30000) {
        int cameraId = atoi(id);
        int ret = mocha::MochaCameraHAL::openCamera(cameraId, device);
        ALOGI("Camera HAL open_legacy: returning %d", ret);
        return ret;
    }

    // HAL1 request - must return ENOSYS so CameraProviderManager
    // falls back to HAL3 detection via module version 2.4
    ALOGW("Camera HAL open_legacy: unsupported HAL version %u (we are HAL3 only)", halVersion);
    return -ENOSYS;
}

static int set_torch_mode(const char* camera_id, bool enabled) {
    ALOGI("Camera HAL set_torch_mode: camera_id=%s enabled=%d", camera_id, enabled);
    int cameraId = atoi(camera_id);
    if (cameraId < 0 || cameraId >= mocha::MochaCameraHAL::kNumCameras) {
        return -EINVAL;
    }
    if (cameraId != 0) {
        return -ENOSYS;
    }
    return 0;
}

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = CAMERA_HARDWARE_MODULE_ID,
        .name = "Mocha Camera HAL",
        .author = "Mocha Team",
        .methods = &methods,
    },
    .get_number_of_cameras = get_number_of_cameras,
    .get_camera_info = get_camera_info,
    .set_callbacks = set_callbacks,
    .get_vendor_tag_ops = nullptr,
    .open_legacy = open_legacy,
    .set_torch_mode = set_torch_mode,
    .init = nullptr,
};
