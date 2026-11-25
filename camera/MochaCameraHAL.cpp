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
/* [still-capture fix 2026-11-17] The device gralloc is mapper@2.1 (HIDL
 * passthrough): apps lock buffers through the gralloc2.1 GraphicBufferMapper
 * while this HAL used gralloc0's gralloc_module_t->lock(). NVIDIA gralloc's
 * cross-process lock (shared LockState + pthread mutex) requires every process
 * to use the same interface, so the mismatch left the shared LockState at -1
 * after the HAL wrote a JPEG and the app's AcquireBufferLock always timed out
 * (capture hung, nothing was saved). GraphicBufferMapper matches the app. */
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
// [2026-11-19 the saved JPEG was zero-filled to 4 MB] The framework sizes the
// BLOB buffer with width == byte capacity and height == 1 (getJpegBufferSize()
// computes the capacity and setBufferSizeLocked() allocates it through
// native_window_set_buffers_dimensions(mMaxSize, 1)). On recycle,
// fixUpHidlJpegBlobHeader() looks for the camera3_jpeg_blob_t trailer in the
// LAST 8 BYTES of the buffer; when it is missing it logs
// "jpeg blob id 0 is not correct" and falls back to ANDROID_JPEG_MAX_SIZE, so
// the photo is zero-filled to 4 MB. We used to write the trailer at
// blobW*blobH*2 (only 1.8 MB for 1280x720), nowhere near the end, so it never
// matched. Recompute the capacity with the framework formula:
//   capacity(w,h) = scale * (maxSize - kMin) + kMin
//   scale         = (w * h) / (maxJpegW * maxJpegH)
// where kMin = 256 KiB + sizeof(CameraBlob) and maxJpeg* is the largest BLOB
// resolution this HAL advertises (see
// ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS). All three values must stay in
// sync with the static metadata or the trailer lands in the wrong place.
static constexpr int32_t kMaxJpegSize = 4149248;
static constexpr size_t kMinJpegBufferSize = 256 * 1024 + 8;
static constexpr uint32_t kMaxJpegWidth = 1920;
static constexpr uint32_t kMaxJpegHeight = 1080;

// Recompute the BLOB capacity in bytes with the framework algorithm. Evaluate
// the whole expression in float and truncate once; truncating the product first
// would be a few bytes off from the framework.
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
// [2026-11-17] Register the front ov5693: the kernel probes it and binds it to
// channel 1 (tegra-vi-1 -> /dev/video1) and CameraPipeline already picks
// video0/video1 by camera id, but kCameras never listed it, so
// getNumberOfCameras() returned only one entry.
const MochaCameraInfo MochaCameraHAL::kCameras[] = {
    { 0, "IMX179", CAMERA_FACING_BACK, 90, 3280, false },
    { 1, "OV5693", CAMERA_FACING_FRONT, 270, 2624, false },
};

const int MochaCameraHAL::kNumCameras = sizeof(MochaCameraHAL::kCameras) / sizeof(MochaCameraHAL::kCameras[0]);

// Static camera characteristics cache
static camera_metadata_t* gCameraCharacteristics[2] = { nullptr, nullptr };

/* Currently open camera id, used by build_result_metadata() to build the crop
 * region (see where gActiveCameraId is assigned in camera_device_open). 0 is the
 * back camera. */
static int gActiveCameraId = 0;

/* Digital zoom: the crop actually applied to the last frame, in
 * active-array coordinates (what the framework reads back via
 * ANDROID_SCALER_CROP_REGION). Full frame by default. */
static int32_t gAppliedCrop[4] = {0, 0, 0, 0};

/* [2026-11-18 recording plan 1] Marker for the encoder input stream.
 *
 * We used to rewrite that stream's format to YCbCr_420_888 so the copy path would
 * go through YUV, but then the framework and the BufferQueue ACodec had already
 * built disagreed and every dequeue returned
 * `Stream 2: Can't dequeue next output buffer: -22`.
 * Now the format stays IMPLEMENTATION_DEFINED (gralloc picks the layout) and only
 * the pointer is recorded here, with isEncoderStream() choosing the YUV write path. */
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
    /* Second scratch buffer: in a multi-stream request the BLOB size usually differs
     * from the pipeline size, so the pipeline frame must be cropped and scaled to the
     * BLOB size before the JPEG encoder runs. */
    uint8_t* temp_jpeg_rgba;
    uint32_t temp_jpeg_size;
    /* Compact intermediate buffer for the preview write-out (copied into the surface row by row with the real stride) */
    uint8_t* temp_out;
    uint32_t temp_out_size;

    uint8_t af_mode;
    uint8_t af_trigger;
    bool af_trigger_handled;
};

/* Encode one RGBA frame as a JPEG inside the BLOB stream buffer, with the 8-byte
 * camera3_jpeg_blob trailer at the end.
 * [2026-11-22] This is where still capture really broke: when a request carried both
 * a preview and a JPEG stream, the old code filled only the "capture target" (the
 * RGBA preview) and skipped the BLOB as secondary, so Camera2-JpegProcessor found no
 * SOI marker ("Could not find start of JPEG marker") and the app saved the blank 4 MB
 * buffer as the picture. The BLOB is now always encoded separately here. */

/* [2026-11-22 diagnostic] gralloc0's private_handle_t puts ANativeWindowBuffer first
 * in the struct and the framework reads the BLOB capacity from anb->width (where
 * Camera2Client's imgBuffer.width comes from). These few fields confirm how much to
 * lock and where the trailer has to go. */
struct MochaAnbHead {
    int32_t magic;
    int32_t version;
    void*   reserved[4];
    void*   incRef;
    void*   decRef;
    int32_t width;
    int32_t height;
    int32_t stride;
    int32_t format;
};
static bool writeJpegToBlob(mocha_camera_device_t* dev,
                            const camera3_stream_buffer_t& out,
                            const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!dev || !out.buffer || !out.stream || !rgba) return false;

    char skip[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.mocha.cam.skipjpeg", skip, "0");
    if (atoi(skip) != 0) {
        ALOGI("writeJpegToBlob: SKIP write (skipjpeg=1), handing buffer back untouched");
        return true;
    }

    const uint32_t blobW = out.stream->width;
    const uint32_t blobH = out.stream->height;
    /* [2026-11-22 how the BLOB must be locked] Two traps stacked, remember both:
     * 1) Buffer size: the framework allocates the whole ANDROID_JPEG_MAX_SIZE we
     *    advertise (4,149,248 bytes here, independent of the photo resolution) and
     *    the trailer must sit in the last 8 bytes, because the framework reads
     *    camera3_jpeg_blob from bufferSize-8.
     * 2) Lock geometry: NEVER Rect(capacity, 1). gralloc would build a shadow mapping
     *    of "one 4-megapixel row" and unlock corrupts the allocator, so every later
     *    lock (cameraserver's JpegProcessor, or the app reading the Image) gets a
     *    garbage mutex: SEGV in NvGrLock, or AcquireBufferLock timed out /
     *    LockState -1.
     * So lock by IMAGE geometry but extend the height to cover the whole buffer: the
     * JPEG and the trailer then share one mapping and the geometry is a 2D shape
     * gralloc handles. Verified at 8 MP (image 7.99 MB > 4.15 MB buffer, locked
     * 3264x2448) and 1080p (1080 rows extended to 2162). */
    char noTrailer[PROPERTY_VALUE_MAX] = {0};
    property_get("persist.mocha.cam.notrailer", noTrailer, "1");
    const bool writeTrailer = (atoi(noTrailer) == 0);

    const size_t blobCapacity = kMaxJpegSize;
    const size_t payloadCapacity = blobCapacity - sizeof(camera3_jpeg_blob_t);
    uint32_t lockW = blobW, lockH = blobH;
    if (writeTrailer && (uint64_t)lockW * lockH < blobCapacity) {
        /* The trailer goes at the end of the buffer so the mapping must reach there;
         * extending the height is enough (still a 2D image shape, not the pathological
         * (capacity,1) geometry). */
        lockH = (uint32_t)((blobCapacity + lockW - 1) / lockW);
    }
    const android::Rect blobRect((int)lockW, (int)lockH);

    void* vaddr = nullptr;
    if (android::GraphicBufferMapper::get().lock(*out.buffer,
                GRALLOC_USAGE_SW_WRITE_OFTEN, blobRect, &vaddr) != 0 || !vaddr) {
        ALOGE("writeJpegToBlob: BLOB buffer not lockable (lock %ux%u)", lockW, lockH);
        return false;
    }

    if (!dev->jpeg_encoder) dev->jpeg_encoder = new mocha::JpegEncoder();
    size_t jpegSize = 0;
    int ret = dev->jpeg_encoder->encodeRGBA(rgba, (int)w, (int)h, 90,
                                           (uint8_t*)vaddr, payloadCapacity, &jpegSize);
    bool ok = false;
    if (ret == 0 && jpegSize > 0) {
        if (writeTrailer) {
            camera3_jpeg_blob_t blob;
            blob.jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
            blob.jpeg_size = (uint32_t)jpegSize;
            memcpy((uint8_t*)vaddr + payloadCapacity, &blob, sizeof(blob));
        }
        uint8_t* jp = (uint8_t*)vaddr;
        ALOGI("JPEG captured: %ux%u -> blob %ux%u, %zu bytes, hdr %02X%02X, trailer %s",
              w, h, blobW, blobH, jpegSize, jp[0], jp[1],
              writeTrailer ? "yes" : "no");
        ok = true;
    } else {
        ALOGE("writeJpegToBlob: encode failed %d", ret);
    }
    android::GraphicBufferMapper::get().unlock(*out.buffer);
    return ok;
}

static void scaleRgbaNearest(const uint8_t* src, int srcW, int srcH,
                            uint8_t* dst, int dstW, int dstH);

/* [2026-11-22 preview stripes] gralloc's stride is not always the width (this
 * allocator returns an aligned pitch, e.g. a 1440-wide buffer may be 1472 or 1536).
 * We wrote tightly by width, so each row shifted by (stride-width) pixels and the
 * error accumulated into regular stripes - that was the "preview looks wrong" the
 * user saw. Capture into a packed buffer first, then copy row by row with the real
 * stride. */
static int surfaceStride(buffer_handle_t handle, int w) {
    if (!handle) return w;
    const MochaAnbHead* anb = reinterpret_cast<const MochaAnbHead*>(handle);
    const int stride = anb->stride;
    if (stride < w || stride > w + 4096) return w;   /* fall back to a packed write if the layout is unexpected */
    return stride;
}

/* A null srcPacked means "let the pipeline capture straight into the surface" (the
 * stride==width fast path: saves one 1.5 MP copy, 65 ms -> 31 ms per frame in
 * testing). */
static bool writeRgbaToSurface(mocha_camera_device_t* dev,
                              const camera3_stream_buffer_t& out,
                              const uint8_t* src, uint32_t srcW, uint32_t srcH,
                              mocha::CameraPipeline* pipeline) {
    if (!dev || !out.buffer || !out.stream) return false;
    const int w = (int)out.stream->width;
    const int h = (int)out.stream->height;
    if (w <= 0 || h <= 0) return false;

    /* Fast path: stride equals width and no scaling is needed, write the surface directly */
    /* src must not be null on the slow path: if the fast path conditions are not
     * met we have to fail instead of writing an uninitialised buffer out as the
     * picture (in testing that gave full-screen regular stripes). */
    if (pipeline && !src && (int)srcW == w && (int)srcH == h) {
        void* vaddr = nullptr;
        const android::Rect rect(w, h);
        if (android::GraphicBufferMapper::get().lock(*out.buffer,
                    GRALLOC_USAGE_SW_WRITE_OFTEN, rect, &vaddr) != 0 || !vaddr)
            return false;
        const int stride = surfaceStride(*out.buffer, w);
        int ret = -EINVAL;
        if (stride == w) {
            ret = pipeline->captureFrameRgba(static_cast<uint8_t*>(vaddr), w, h);
        }
        android::GraphicBufferMapper::get().unlock(*out.buffer);
        if (stride == w) return ret == 0;
        /* stride mismatch: fall back to the slow path (lock again and write row by row below) */
    }

    const uint32_t need = (uint32_t)w * h * 4;
    if (!dev->temp_out || dev->temp_out_size < need) {
        if (dev->temp_out) free(dev->temp_out);
        dev->temp_out = (uint8_t*)malloc(need);
        dev->temp_out_size = need;
    }
    if (!dev->temp_out) return false;
    if (!src) {
        ALOGE("writeRgbaToSurface: no source frame for %dx%d", w, h);
        return false;
    }
    /* Crop/scale (digital zoom too) into a packed w*h*4 */
    scaleRgbaNearest(src, (int)srcW, (int)srcH, dev->temp_out, w, h);

    void* vaddr = nullptr;
    const android::Rect rect(w, h);
    if (android::GraphicBufferMapper::get().lock(*out.buffer,
                GRALLOC_USAGE_SW_WRITE_OFTEN, rect, &vaddr) != 0 || !vaddr) {
        ALOGE("preview: surface not lockable");
        return false;
    }
    uint8_t* dst = static_cast<uint8_t*>(vaddr);
    const int stride = surfaceStride(*out.buffer, w);
    {
        static int lastW = -1, lastStride = -1;
        if (w != lastW || stride != lastStride) {
            lastW = w; lastStride = stride;
            ALOGI("preview surface: %dx%d stride=%d (packed write=%s)", w, h, stride,
                  stride == w ? "yes" : "no");
        }
    }
    if (stride == w) {
        memcpy(dst, dev->temp_out, (size_t)w * h * 4);
    } else {
        const size_t rowBytes = (size_t)w * 4;
        const size_t padBytes = (size_t)(stride - w) * 4;
        for (int y = 0; y < h; y++) {
            uint8_t* d = dst + (size_t)y * stride * 4;
            memcpy(d, dev->temp_out + (size_t)y * rowBytes, rowBytes);
            if (padBytes) memset(d + rowBytes, 0, padBytes);
        }
    }
    android::GraphicBufferMapper::get().unlock(*out.buffer);
    return true;
}

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
    // [2026-11-18 match A12] That A12 unit (capture and recording work there)
    // advertises only 1920x1080 and 1280x720, only BLOB / IMPLEMENTATION_DEFINED /
    // YCbCr_420_888, and no BLOB above 1080p (jpeg.maxSize is 1920*1080*2). We
    // advertised an 8 MP 3280x2464 BLOB, Aperture always picked it and went into the
    // legacy NVIDIA gralloc 8 MP BLOB lock path (AcquireBufferLock ... LockState -1),
    // so photos never saved. Converge on the A12 sizes and switch 4:3 to 16:9: the
    // sensor only outputs 720p and the extra sizes were nearest-neighbour upscales.
    // Capture side (IMPL_DEF/YCbCr/YV12) keeps 1280x720, the size V4L2 really outputs.
    /* [2026-11-22 full-screen preview] The panel is 1536x2048 (3:4) but the sensor
     * only outputs 16:9, so a rotated 1280x720/1920x1080 preview is a narrow 9:16
     * strip with a big black band on each side. Add 1440x1080 (4:3): exactly 3:4 when
     * rotated, it fills the screen. Capture still runs the full 16:9 1920x1080 and
     * cropScaleRgba() centre-crops to 4:3 (see CameraPipeline.cpp), so nothing is
     * stretched or distorted. */
    /* [2026-11-22 per-camera size table] The two sensors have different mode tables:
     *   back IMX179 (8 MP): 720p/1440x1080(4:3)/1080p/1920x1440/5MP/8MP;
     *   front OV5693 (5 MP): 720p/1080p/5MP (ov5693_s_stream now programs the readout
     *   window for the requested size, see ov5693.c).
     * The front camera does not advertise sizes its sensor lacks (3264x2448 /
     * 1920x1440 / 1440x1080); picking one led to a VI timeout and TPG. */
    static const int32_t rear_configs[] = {
        /* [2026-11-22 restore the 4:3 preview] The 1440x1080 window shows regular
         * wavy stripes (720p/5MP/8MP are clean, only this size is bad) and the user
         * confirmed 720p is the only correct one. Restoring 4:3 as requested: preview
         * goes back to 16:9 (1280x720/1920x1080) and the full-screen look comes from
         * Aperture's fillCenter crop. The 1440x1080 corruption is still unexplained
         * (VI tearing or ISP stride); do not advertise it until that is understood. */
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1440, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                  2592, 1944, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                  3264, 2448, CAMERA3_STREAM_OUTPUT,
    };
    static const int64_t rear_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1440, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                  2592, 1944, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                  3264, 2448, 33333333LL,
    };
    static const int64_t rear_stall_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1920, 1080, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,         1920, 1080, 0,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 0,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 100000000LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1080, 100000000LL,
        HAL_PIXEL_FORMAT_BLOB,                  1920, 1440, 100000000LL,
        HAL_PIXEL_FORMAT_BLOB,                  2592, 1944, 100000000LL,
        HAL_PIXEL_FORMAT_BLOB,                  3264, 2448, 100000000LL,
    };
    static const int32_t rear_processed_sizes[] = {
        1280, 720,
        1920, 1080,
    };
    static const int32_t rear_jpeg_sizes[] = {
        1280, 720,
        1920, 1080,
        1920, 1440,
        2592, 1944,
        3264, 2448,
    };

    /* Front camera: 720p is the only output the OV5693 driver is verified for (the
     * init registers 2x-scale a 2624x1476 readout area and the undocumented
     * scaling/binning registers must not be touched - programming a 1:1 window the
     * IMX179 way on 2026-11-22 made even 720p hit a VI timeout and was reverted).
     * Advertise 720p only; a larger request falls back here instead of a VI timeout. */
    static const int32_t front_configs[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, CAMERA3_STREAM_OUTPUT,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, CAMERA3_STREAM_OUTPUT,
    };
    static const int64_t front_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 33333333LL,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 33333333LL,
    };
    static const int64_t front_stall_durations[] = {
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED, 1280, 720, 0,
        HAL_PIXEL_FORMAT_YCbCr_420_888,          1280, 720, 0,
        HAL_PIXEL_FORMAT_YV12,                   1280, 720, 0,
        HAL_PIXEL_FORMAT_BLOB,                   1280, 720, 100000000LL,
    };
    static const int32_t front_processed_sizes[] = {
        1280, 720,
    };
    static const int32_t front_jpeg_sizes[] = {
        1280, 720,
    };

    const bool isFront = (cameraId == 1);
    const int32_t* configs         = isFront ? front_configs         : rear_configs;
    const int64_t* durations       = isFront ? front_durations       : rear_durations;
    const int64_t* stall_durations = isFront ? front_stall_durations : rear_stall_durations;
    const int32_t* processed_sizes = isFront ? front_processed_sizes : rear_processed_sizes;
    const int32_t* jpeg_sizes      = isFront ? front_jpeg_sizes      : rear_jpeg_sizes;
    const size_t nConfigs   = isFront ? sizeof(front_configs)/sizeof(int32_t)         : sizeof(rear_configs)/sizeof(int32_t);
    const size_t nDurations = isFront ? sizeof(front_durations)/sizeof(int64_t)       : sizeof(rear_durations)/sizeof(int64_t);
    const size_t nStall     = isFront ? sizeof(front_stall_durations)/sizeof(int64_t) : sizeof(rear_stall_durations)/sizeof(int64_t);
    const size_t nProcessed = isFront ? sizeof(front_processed_sizes)/sizeof(int32_t) : sizeof(rear_processed_sizes)/sizeof(int32_t);
    const size_t nJpeg      = isFront ? sizeof(front_jpeg_sizes)/sizeof(int32_t)      : sizeof(rear_jpeg_sizes)/sizeof(int32_t);

    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                              configs, nConfigs);

    int ret = add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                              durations, nDurations);
    ALOGI("DEBUG: Added min frame durations, ret=%d", ret);

    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                              stall_durations, nStall);

    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES,
                              processed_sizes, nProcessed);

    add_camera_metadata_entry(metadata, ANDROID_SCALER_AVAILABLE_JPEG_SIZES,
                              jpeg_sizes, nJpeg);

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
    // [2026-11-17 fix] Front pixelArray corrected from 2592x1944 to 2624x1956.
    // Measured on the Android 12 reference unit (the one with the Shield graphics
    // stack): android.sensor.info.pixelArraySize = 2624x1956 (OV5693).
    // Reporting 2592x1944 broke every aspect ratio except 4:3, so Aperture failed to
    // match a size and fell back to odd resolutions.
    int32_t sensor_width = cam.maxResolution;
    int32_t sensor_height = (cam.maxResolution == 3280) ? 2464 : 1956;
    /* [2026-11-18 match A12] The active array becomes 16:9. A12 reports
     * [20 320 3240 1823] (about 3260x1823, 1.79:1) while pixelArray is still
     * 3280x2464, i.e. it crops the sensor to 16:9. We reported the full 4:3 block, so
     * Aperture's viewfinder came out 4:3 and did not look like A12 (that was the
     * "aspect ratio" complaint). 3280/16*9 = 1845, rounded to 1845. */
    /* [2026-11-22 preview and capture disagreed] The active array used to be reported
     * as 16:9 (3280x1845), but every capture mode is a 1:1 centre crop of the sensor's
     * 4:3 frame (1440x1080 / 2592x1944 / 3264x2448). The framework computed a 4:3 crop
     * window from a 16:9 array, so the preview ended up as a small magnified centre
     * patch (with regular texture patterns) while stills rebuilt the pipeline and came
     * out full-frame. Report the 4:3 active area. */
    int32_t active_width  = (cam.maxResolution == 3280) ? 3264 : 2624;
    int32_t active_height = (cam.maxResolution == 3280) ? 2448 : 1956;
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                              (int32_t[]){0, 0, active_width, active_height}, 4);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, (int32_t[]){sensor_width, sensor_height}, 2);
    add_camera_metadata_entry(metadata, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
                              (int32_t[]){0, 0, active_width, active_height}, 4);

    // Physical sensor size (required for FOV calculation)
    // [2026-11-17 fix] The two entries were swapped: the back camera had 3.676x2.757
    // (that is OV5693 territory) and the front had 2.8x2.1. Corrected to the values
    // measured on the Android 12 reference unit:
    //   IMX179 (back): 4.5920 x 3.4496 mm  (1/3.2", 1.4 um)
    //   OV5693 (front): 3.6736 x 2.7384 mm
    // The physical size feeds the FOV / equivalent focal length calculation, so a swap
    // makes the viewfinder disagree with the "hidden parameters" readout.
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
    // [2026-11-17 fix] The front OV5693 is fixed focus (no VCM, the AD5823 only sits
    // on back channel 0), yet both cameras reported AF, so the front camera showed up
    // as "1-Front:5MP f/2.8, AF" and Aperture sent AF requests that made the HAL sweep
    // for nothing. Measured on the Android 12 reference: the back camera reports
    // afAvailableModes = {OFF, AUTO, CONTINUOUS_PICTURE}, the front one only {OFF}.
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
    // [2026-11-18 match A12] A12's jpeg.maxSize is 4149248 (about 1920x1080x2); we
    // reported 16163840 for 8 MP and the framework allocated a huge BLOB.
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
    // [2026-11-17 fix] Both cameras used to report a placeholder 3.5 mm. Measured on
    // the Android 12 reference: back IMX179 = 3.36 mm, front OV5693 = 2.1 mm. Focal
    // length feeds the FOV calculation, so a wrong value makes the lens data in
    // "hidden parameters" disagree with reality.
    float focal_lengths[1];
    focal_lengths[0] = (cameraId == 0) ? 3.36f : 2.10f;
    add_camera_metadata_entry(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, focal_lengths, 1);

    // Lens aperture (required by some framework paths)
    // [2026-11-17 fix] Used to be a placeholder 2.8. Both modules are actually f/2.0 as
    // measured on the Android 12 reference (availableApertures = {2.0}).
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
    dev->temp_out = nullptr;
    dev->temp_out_size = 0;
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
    if (dev->temp_out) {
        free(dev->temp_out);
        dev->temp_out = nullptr;
        dev->temp_out_size = 0;
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

/* [2026-11-22] Create or rebuild the capture pipeline.
 * configureStreams and high-resolution capture share it: capture briefly switches to
 * 2592x1944 and switches back to the preview size afterwards, so the preview keeps
 * 30 fps while the photo is a real 5 MP frame instead of an upscale. */
static int openPipelineAt(mocha_camera_device_t* dev, uint32_t w, uint32_t h,
                          int inheritExposure = 0, int inheritGain = 0) {
    if (dev->pipeline) {
        mocha::CameraPipeline* old = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        old->close();
        delete old;
        dev->pipeline = nullptr;
    }

    mocha::CameraPipeline* pipeline = new mocha::CameraPipeline();
    if (!pipeline) {
        ALOGE("openPipelineAt: out of memory");
        return -ENOMEM;
    }
    int ret = pipeline->open(dev->camera_id);
    if (ret != 0) {
        ALOGE("openPipelineAt: pipeline->open failed: %d", ret);
        delete pipeline;
        return -ENODEV;
    }

    mocha::PipelineConfig pipelineConfig;
    memset(&pipelineConfig, 0, sizeof(pipelineConfig));   /* new fields must not hold garbage */
    pipelineConfig.width = w;
    pipelineConfig.height = h;
    /* Bayer pattern must match the V4L2 format requested below:
     * camera 0 (IMX179) uses SRGGB10 -> RGGB (pat 3); camera 1 (OV5693)
     * uses SBGGR10 -> BGGR (pat 2). Sharing one value gave the front
     * camera a strong green cast (G/B channels swapped). */
    pipelineConfig.bayerPattern = (dev->camera_id == 0) ? 3 : 2;
    pipelineConfig.offset_x = 0;
    pipelineConfig.offset_y = 0;
    pipelineConfig.flipV = false;  // IMX179 mount normal; framework handles rotation

    pipelineConfig.enableISP = true;

     /* [2026-11-25 per-sensor black level] The two sensors do not share a pedestal.
      * Measured on dark frames (10-bit raw): IMX179 sits at ~64 (=> 16 in the 8-bit
      * scale this table uses), while the OV5693 sub-lattices are B~50 / G~60 / R~60.
      * Subtracting a flat 16 removed G and R completely but left the blue channel
      * over-subtracted, which is what turned the front camera shadows green (blue
      * crushed, green untouched). Stop at the lowest pedestal instead. */
     pipelineConfig.blackLevel = (dev->camera_id == 0) ? 16 : 12;
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
    /* [2026-11-22 overexposure] AE measures luma in the linear domain before gamma,
     * so 0.55 pushes the picture close to saturation (white curtains measured 57% of
     * pixels at 255, mean 231/255). Mid-grey in linear is about 0.18-0.22, so use
     * 0.20. Software gain also drops from 1.5 back to 1.0: that was added to brighten
     * dark scenes, but the sensor now has 2500 lines of exposure headroom and
     * multiplying by 1.5 only washes out the highlights.
     * [2026-11-22 front camera too dark] 0.20 was tuned for the back IMX179 native
     * sensitivity. The front OV5693 is clearly darker at the same target (the user
     * called it "very dark"), so raise it to 0.32: 0.32 in linear becomes about 60/255
     * after gamma(0.55), usable in the dark without clipping the highlights. */
    pipelineConfig.targetLuma = (dev->camera_id == 1) ? 0.32f : 0.20f;
    pipelineConfig.digitalGain = 1.0f;
    /* Rebuilding the pipeline for a high-resolution still inherits the converged exposure/gain (see the PipelineConfig comment). */
    pipelineConfig.initialExposure = inheritExposure;
    pipelineConfig.initialGain = inheritGain;
 
    // Override IMPLEMENTATION_DEFINED to RGBA_8888 (Tegra gralloc allocates
    // RGBA for non-YUV formats). Keep YCbCr_420_888 and BLOB as-is.
    pipelineConfig.pixelFormat =
            (dev->camera_id == 0) ? V4L2_PIX_FMT_SRGGB10 : V4L2_PIX_FMT_SBGGR10;

    ret = pipeline->configure(pipelineConfig);
    if (ret != 0) {
        ALOGE("openPipelineAt: configure(%ux%u) failed: %d", w, h, ret);
        pipeline->close();
        delete pipeline;
        return ret;
    }
    ret = pipeline->startStreaming();
    if (ret != 0) {
        ALOGE("openPipelineAt: startStreaming(%ux%u) failed: %d", w, h, ret);
        pipeline->close();
        delete pipeline;
        return ret;
    }

    dev->pipeline = pipeline;
    dev->pipeline_width = w;
    dev->pipeline_height = h;
    dev->last_config_width = w;
    dev->last_config_height = h;
    ALOGI("openPipelineAt: streaming at %ux%u", w, h);
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
        /* [2026-11-22 high-resolution capture] The BLOB does not compete for the capture
         * size: a photo stream is often 2592x1944 and letting it drive the pipeline
         * drops the preview to 7 fps. process_capture_request switches to it briefly
         * and switches back after the shot. */
        if (s->format == HAL_PIXEL_FORMAT_BLOB) continue;
        uint32_t area = s->width * s->height;
        if (area > pipeArea) { pipeArea = area; pipeW = s->width; pipeH = s->height; }
    }
    /* CSI link bandwidth caps, measured on hardware: the OV5693 streams over
     * a SINGLE lane (CSI-E) - asking it for 1080p makes VI capture time out
     * (-110) and the channel falls back to the TPG test pattern. The IMX179
     * uses 4 lanes (CSI-A) and does 1080p natively. Anything bigger is also
     * capped: 1080p is the sweet spot of the 1:1 readout at 30fps. */
    const uint32_t kMaxArea = (dev->camera_id == 0) ? (2592u * 1944u)
                                                    : (1280u * 720u);
    if (pipeW * pipeH > kMaxArea) { pipeW = 1280; pipeH = 720;
        if (dev->camera_id == 0) { pipeW = 1920; pipeH = 1080; } }
    /* [2026-11-22] Capture resolution follows the largest output stream, capped only
     * by kMaxArea. A 4:3 preview (1440x1080) is therefore captured at 1440x1080: the
     * sensor already delivers a centred 1:1 crop, so the output side need not rescale,
     * which saves one large rescale and looks sharper. */
    /* [2026-11-22] Debug aid: persist.mocha.cam.pipew/pipeh pin the capture size so a
     * sensor mode can be validated for stable frames without a rebuild. */
    {
        char pw[PROPERTY_VALUE_MAX] = {0}, ph[PROPERTY_VALUE_MAX] = {0};
        property_get("persist.mocha.cam.pipew", pw, "0");
        property_get("persist.mocha.cam.pipeh", ph, "0");
        int ow = atoi(pw), oh = atoi(ph);
        if (ow > 0 && oh > 0) {
            ALOGI("Pipeline resolution overridden by property: %dx%d", ow, oh);
            pipeW = (uint32_t)ow; pipeH = (uint32_t)oh;
        }
    }
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

    /* Clear the encoder stream registration before every configure to avoid a stale pointer from the last session. */
    for (int k = 0; k < 4; k++) gEncoderStreams[k] = nullptr;
    /* [2026-11-22 still capture] Every output stream gets
     * GRALLOC_USAGE_SW_WRITE_OFTEN: the BLOB is written by the HAL on the CPU, and
     * without this usage vendor gralloc does not guarantee a writable mapping, so the
     * app's next lock spins forever ("AcquireBufferLock timed out ... LockState -1") and
     * capture fails or ends in an ANR. The Dargons10 lineage-19.1 tree sets this bit on
     * every stream for the same reason. */
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        if (stream->stream_type != CAMERA3_STREAM_OUTPUT) continue;
        stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
        /* Tegra gralloc only accepts flexible YUV for camera allocations; without this
         * bit the allocation gets EINVAL.
         * [2026-11-22 recording produced 0 frames] The encoder input stream was changed
         * to YV12 but HW_CAMERA_WRITE was only added for YCbCr_420_888, so the YV12
         * allocation was refused and the framework's dequeue kept returning -EINVAL
         * (Camera3-OutputStream: Stream 2: Can't dequeue next output buffer: Invalid
         * argument), hence no video keyframe ever arrived. Add it for YV12 too. */
        if (stream->format == HAL_PIXEL_FORMAT_YCbCr_420_888 ||
            stream->format == HAL_PIXEL_FORMAT_YV12) {
            stream->usage |= GRALLOC_USAGE_HW_CAMERA_WRITE;
        }
    }
    for (uint32_t i = 0; i < config->num_streams; i++) {
        camera3_stream_t *stream = config->streams[i];
        if (stream->stream_type == CAMERA3_STREAM_OUTPUT &&
            stream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            /* [recording fix 2026-11-17] While recording the framework configures an extra
             * "encoder input stream" (usage includes GRALLOC_USAGE_HW_VIDEO_ENCODER).
             * MediaCodec's input Surface only accepts YUV, and every PRIV stream used to be
             * rewritten to RGBA_8888, so the encoder got RGBA buffers and produced not a
             * single frame (CameraX reported ERROR_NO_VALID_DATA, screenrecord recorded 0
             * frames). The encoder stream stays YCbCr_420_888, which the pipeline writes
             * natively as YUV; the preview stays RGBA_8888. */
            if ((stream->usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) != 0) {
                /* [2026-11-18] The on-device GraphicBuffer log settled it:
                 *   err=3 (GRALLOC1_ERROR_UNSUPPORTED) fmt=0x23 usage=0x10033 mapperVersion=2
                 * i.e. **vendor gralloc1 does not support YCbCr_420_888 + HW_VIDEO_ENCODER**,
                 * so the encoder input buffer could not be allocated at all. Use YV12, which
                 * the old NVIDIA camera/encoder path always supported and which our fan-out
                 * already handles with a plane-packing fallback. */
                /* [2026-11-22] YV12 is refused by vendor gralloc with any usage (dequeue
                 * -22), so go back to YCbCr_420_888: what failed on 11-18 was the YCbCr +
                 * HW_VIDEO_ENCODER combination, and with HW_VIDEO_ENCODER stripped the
                 * allocation takes the flexible YUV camera-write path (the shim allows it). */
                stream->format = HAL_PIXEL_FORMAT_YCbCr_420_888;
                stream->usage |= GRALLOC_USAGE_SW_WRITE_OFTEN;
                /* [2026-11-22 recording 0 frames] YV12 + HW_VIDEO_ENCODER is refused by vendor
                 * gralloc (dequeue always -EINVAL, MediaRecorder never gets a keyframe and
                 * both OpenCamera and MediaRecorder produce nothing). The bit is only an
                 * allocation hint - the HAL writes with the CPU and the encoder reads with
                 * GPU/hardware - so dropping it lets the allocation through. */
                stream->usage &= ~GRALLOC_USAGE_HW_VIDEO_ENCODER;
                for (int k = 0; k < 4; k++) {
                    if (gEncoderStreams[k] == nullptr) {
                        gEncoderStreams[k] = stream;
                        break;
                    }
                }
                /* [2026-11-18] Back to 4: with max_buffers=2 Camera3's dequeue returned
                 * -EINVAL outright (mocha-dequeue log: max_buffers=2 total=4 handedOut=0),
                 * while 4 slots at least dequeue normally and recording starts. */
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
    /* Pipeline creation and configuration live in openPipelineAt() (high-resolution capture reuses it). */
    int ret = openPipelineAt(dev, pipeW, pipeH);
    if (ret != 0) {
        ALOGE("configureStreams: openPipelineAt(%ux%u) failed: %d", pipeW, pipeH, ret);
        return ret;
    }

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
    /* [2026-11-18] Used to be hard-coded to the 4:3 3280x2464 for both cameras; now it follows the 16:9 active array. */
    int32_t cropRegion[4] = {
        gAppliedCrop[0], gAppliedCrop[1], gAppliedCrop[2], gAppliedCrop[3]
    };
    if (cropRegion[2] <= 0 || cropRegion[3] <= 0) {
        cropRegion[2] = (gActiveCameraId == 0) ? 3264 : 2624;
        cropRegion[3] = (gActiveCameraId == 0) ? 2448 : 1956;
    }
    add_camera_metadata_entry(metadata, ANDROID_SCALER_CROP_REGION, cropRegion, 4);

    /* report the zoom ratio that matches the applied crop */
    {
        float reportedZoom = 1.0f;
        int32_t fullW = (gActiveCameraId == 0) ? 3264 : 2624;
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
// [still-capture fix 2026-11-17] Use GraphicBufferMapper (mapper@2.1) so the lock
// semantics match the app's and no NVIDIA cross-process LockState leaks.
/* [recording fix 2026-11-17] Nearest-neighbour RGBA_8888 scaling.
 * While recording the framework configures two output streams, preview and encoder
 * input, usually at different resolutions (preview 1280x720, recording 1920x1080).
 * copyFrameBetweenBuffers() used to return early when the sizes differed, so
 * **the encoder stream was never written**, MediaCodec never saw a keyframe and
 * CameraX ended with ERROR_NO_VALID_DATA and no mp4 in DCIM.
 * Scale to the target resolution here (column map precomputed, the inner loop is
 * table lookup plus copy; 720p->1080p costs tens of ms on TK1, half the memory
 * bandwidth of a bilinear filter). */
static void scaleRgbaNearest(const uint8_t* src, int srcW, int srcH,
                             uint8_t* dst, int dstW, int dstH) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    /* [2026-11-22] When the aspect ratios differ, centre-crop to the target ratio
     * before scaling (no stretching). A 16:9 capture into a 4:3 preview only loses the
     * sides; with equal ratios the crop window is the whole frame. */
    int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
    if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
        cropW = (int)((int64_t)srcH * dstW / dstH) & ~1;
        if (cropW < 2) cropW = srcW & ~1;
        cropX = (srcW - cropW) / 2;
    } else if ((int64_t)srcW * dstH < (int64_t)srcH * dstW) {
        cropH = (int)((int64_t)srcW * dstH / dstW) & ~1;
        if (cropH < 2) cropH = srcH & ~1;
        cropY = (srcH - cropH) / 2;
    }

    /* Column map dstX -> srcX so the per-pixel loop needs no division */
    std::vector<int> xmap(dstW);
    for (int x = 0; x < dstW; x++)
        xmap[x] = cropX + (int)((int64_t)x * cropW / dstW);

    for (int y = 0; y < dstH; y++) {
        int srcY = cropY + (int)((int64_t)y * cropH / dstH);
        if (srcY >= srcH) srcY = srcH - 1;
        const uint32_t* srow = reinterpret_cast<const uint32_t*>(
                src + (size_t)srcY * srcW * 4);
        uint32_t* drow = reinterpret_cast<uint32_t*>(
                dst + (size_t)y * dstW * 4);
        for (int x = 0; x < dstW; x++)
            drow[x] = srow[xmap[x]];
    }
}

/* [recording fix 2026-11-17] RGBA_8888 -> YUV420 (BT.601, limited range).
 * The encoder input Surface holds YUV while the pipeline produces RGBA, so the
 * fan-out to the encoder stream needs a colour space conversion.
 * gralloc decides the android_ycbcr layout: chroma_step==2 is a semi-planar
 * NV12/NV21 style buffer (interleaved Cb/Cr), ==1 is I420 with separate planes;
 * both are supported here. */
static void rgbaToYuv420Scaled(const uint8_t* rgba, int srcW, int srcH,
                               int dstW, int dstH, struct android_ycbcr* out) {
    if (!rgba || !out || !out->y || !out->cb || !out->cr) return;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    const int w = dstW, h = dstH;

    /* [2026-11-22] Same as scaleRgbaNearest: centre-crop on aspect mismatch so the
     * encoder stream is never stretched or squeezed. */
    int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
    if ((int64_t)srcW * dstH > (int64_t)srcH * dstW) {
        cropW = (int)((int64_t)srcH * dstW / dstH) & ~1;
        if (cropW < 2) cropW = srcW & ~1;
        cropX = (srcW - cropW) / 2;
    } else if ((int64_t)srcW * dstH < (int64_t)srcH * dstW) {
        cropH = (int)((int64_t)srcW * dstH / dstW) & ~1;
        if (cropH < 2) cropH = srcH & ~1;
        cropY = (srcH - cropH) / 2;
    }

    uint8_t* yPlane = static_cast<uint8_t*>(out->y);
    uint8_t* uPlane = static_cast<uint8_t*>(out->cb);
    uint8_t* vPlane = static_cast<uint8_t*>(out->cr);
    const size_t yStride = out->ystride;
    const size_t cStride = out->cstride;
    const size_t cStep = out->chroma_step ? out->chroma_step : 1;

    for (int y = 0; y < h; y++) {
        int sy = cropY + (int)((int64_t)y * cropH / h);
        if (sy >= srcH) sy = srcH - 1;
        const uint8_t* row = rgba + (size_t)sy * srcW * 4;
        uint8_t* yRow = yPlane + (size_t)y * yStride;
        for (int x = 0; x < w; x++) {
            int sx = cropX + (int)((int64_t)x * cropW / w);
            if (sx >= srcW) sx = srcW - 1;
            int r = row[sx * 4 + 0];
            int g = row[sx * 4 + 1];
            int b = row[sx * 4 + 2];
            int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[x] = (uint8_t)(yy < 0 ? 0 : (yy > 255 ? 255 : yy));
        }
    }

    for (int y = 0; y < h; y += 2) {
        int sy0 = cropY + (int)((int64_t)y * cropH / h);
        int sy1 = cropY + (int)((int64_t)(y + 1 < h ? y + 1 : y) * cropH / h);
        if (sy0 >= srcH) sy0 = srcH - 1;
        if (sy1 >= srcH) sy1 = srcH - 1;
        const uint8_t* row0 = rgba + (size_t)sy0 * srcW * 4;
        const uint8_t* row1 = rgba + (size_t)sy1 * srcW * 4;
        uint8_t* uRow = uPlane + (size_t)(y / 2) * cStride;
        uint8_t* vRow = vPlane + (size_t)(y / 2) * cStride;
        for (int x = 0; x < w; x += 2) {
            int x1 = (x + 1 < w) ? x + 1 : x;
            int sx0 = cropX + (int)((int64_t)x * cropW / w);
            int sx1 = cropX + (int)((int64_t)x1 * cropW / w);
            if (sx0 >= srcW) sx0 = srcW - 1;
            if (sx1 >= srcW) sx1 = srcW - 1;
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

/* Write a whole RGBA frame into a YUV output stream: prefer the real plane layout
 * from lockYCbCr, and fall back to lock() with an assumed YV12 (Y + V + U) layout
 * when that is unsupported. [2026-11-22] Shared by multi-stream writes of one frame. */
static bool writeYuvFromRgba(const camera3_stream_buffer_t* dst,
                             const uint8_t* rgba, int sw, int sh) {
    if (!dst || !dst->buffer || !dst->stream || !rgba) return false;
    const android::Rect dstRect((int)dst->stream->width, (int)dst->stream->height);
    struct android_ycbcr ycbcr;
    memset(&ycbcr, 0, sizeof(ycbcr));
    int ret = android::GraphicBufferMapper::get().lockYCbCr(*dst->buffer,
                GRALLOC_USAGE_SW_WRITE_OFTEN, dstRect, &ycbcr);
    if (ret == 0 && ycbcr.y) {
        rgbaToYuv420Scaled(rgba, sw, sh,
                           (int)dst->stream->width, (int)dst->stream->height, &ycbcr);
        android::GraphicBufferMapper::get().unlock(*dst->buffer);
        return true;
    }
    void* vaddr = nullptr;
    int lockRet = android::GraphicBufferMapper::get().lock(*dst->buffer,
                GRALLOC_USAGE_SW_WRITE_OFTEN, dstRect, &vaddr);
    if (lockRet != 0 || !vaddr) {
        ALOGE("writeYuvFromRgba: buffer not lockable: lockYCbCr=%d lock=%d", ret, lockRet);
        return false;
    }
    const int w = (int)dst->stream->width;
    const int h = (int)dst->stream->height;
    const size_t ySize = (size_t)w * h;
    memset(&ycbcr, 0, sizeof(ycbcr));
    ycbcr.ystride = (size_t)w;
    ycbcr.cstride = (size_t)w / 2;
    ycbcr.chroma_step = 1;
    ycbcr.y = static_cast<uint8_t*>(vaddr);
    ycbcr.cr = static_cast<uint8_t*>(vaddr) + ySize;
    ycbcr.cb = static_cast<uint8_t*>(vaddr) + ySize + ySize / 4;
    rgbaToYuv420Scaled(rgba, sw, sh, w, h, &ycbcr);
    android::GraphicBufferMapper::get().unlock(*dst->buffer);
    return true;
}

static void copyFrameBetweenBuffers(
        const camera3_stream_buffer_t* src, const camera3_stream_buffer_t* dst) {
    if (!src || !dst || !src->buffer || !dst->buffer) return;
    const bool srcRgba = (src->stream->format == HAL_PIXEL_FORMAT_RGBA_8888);
    /* [2026-11-18 plan 1] The encoder stream format is IMPLEMENTATION_DEFINED (not
     * rewritten), so the internal marker still routes it to the YUV target and the
     * NV12/YV12 write path. */
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
        /* No longer give up: scale to the target stream size instead (see the
         * scaleRgbaNearest comment). Log once per size combination, not per frame. */
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
        /* The target is the encoder YUV stream: lockYCbCr first for the real plane
         * layout, then convert RGBA to YUV420 while scaling as needed.
         *
         * [recording fix 2026-11-17] The vendor gralloc is a mix of old and new: the
         * README notes that lockYCbCr on YV12 returns -ENOSYS (hit by SoftwareRenderer),
         * but HAL_PIXEL_FORMAT_YCBCR_420_888 takes a different branch and may be
         * supported. So try both: use the ystride/cstride/chroma_step lockYCbCr reports
         * when it works, else fall back to lock() and pack YV12 by hand. Either way log
         * the layout once, so the next on-device run shows at a glance which layout the
         * encoder stream really has. */
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
            /* Fallback: gralloc cannot lockYCbCr this stream. Assume YV12 internally
             * (Y plane + V + U) with stride = width: the 1280/1920 sizes used for
             * recording are multiples of 32 and the vendor YV12 stride is 32-aligned, so
             * stride == width holds for both. */
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
                /* YV12: V plane first, then U, w*h/4 each */
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
    // RGBA_8888: 4 bytes/pixel (configure_streams rewrote every non-BLOB stream)
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

    /* [2026-11-23 fence leak] The HAL3 contract: once process_capture_request is
     * accepted, every output buffer's acquire_fence fd in the request belongs to the
     * HAL and must be closed on every path. Only captureBuf used to be closed, so all
     * the other fences leaked on error and multi-stream paths: cameraserver's fd table
     * grew by one sync_fence per frame, hit the 32768 limit after about 18 minutes, and
     * then binder failed to transfer GraphicBuffers because
     * fcntl(F_DUPFD_CLOEXEC) returned EMFILE (Parcel fds[i]=-1), requestBuffer
     * reported FAILED_TRANSACTION and any camera app died after about 25 minutes of
     * preview (OpenCamera sessions are short and hid it; Aperture blew up first). Wait
     * for and close every fence in one place here. */
    for (uint32_t fi = 0; fi < request->num_output_buffers; fi++) {
        int af = request->output_buffers[fi].acquire_fence;
        if (af >= 0) {
            struct pollfd pfd;
            pfd.fd = af;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 500);
            if (pr <= 0) {
                ALOGW("acquire fence (buf %u) not signaled in 500ms, proceeding", fi);
            }
            close(af);
        }
    }

    // Check if this frame was marked as error by flush()
    uint32_t frameNum = request->frame_number;
    if (dev->inflight_tracker && dev->inflight_tracker->isError(frameNum)) {
        /* [2026-11-22 recording -38] This used to return -EAGAIN directly. HAL3 only
         * allows process_capture_request to fail for malformed requests; the CamDevSession
         * wrapper maps any other error to INTERNAL_ERROR, the framework maps that to
         * INVALID_OPERATION (-38) and Camera2Client tears the session down as fatal
         * (Error 1 -> app exits). Clear the flag and capture normally instead: success
         * reports OK, failure goes through the shared REQUEST error notification plus a
         * buffer ERROR state, which the framework discards and retries. */
        ALOGW("Frame %u was marked by flush, recapturing", frameNum);
        dev->inflight_tracker->remove(frameNum);
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
            const int32_t actW = (dev->camera_id == 0) ? 3264 : 2624;
            const int32_t actH = (dev->camera_id == 0) ? 2448 : 1956;
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

    // [2026-11-17 fix] The front OV5693 is fixed focus with no VCM, so only the back
    // camera runs the AF flow. Even if the framework sends an AF request to the front
    // camera it no longer sweeps (each sweep owns the V4L2 device for several frames and
    // makes the front preview stutter).
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
                /* [fix 2026-11-17] In CONTINUOUS_PICTURE mode the framework only sends
                 * mode=4/trigger=0 (IDLE) and never AF_TRIGGER_START - it expects the HAL
                 * to keep focusing. The old code only scanned on trigger==START, so
                 * Aperture never focused and the picture stayed blurry (the 10:05 log shows
                 * "AF request: mode=4 trigger=0" repeatedly and never an "AF: coarse"). Run
                 * one sweep in continuous mode too, triggered by later session or preview
                 * restarts, so the user at least gets a focused frame. */
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

    /* [recording fix 2026-11-17] Prefer an RGBA stream as the capture source. While
     * recording the encoder stream is YUV, and capturing from it would need a
     * YUV->RGBA conversion in the fan-out (unimplemented and more expensive);
     * sourcing from the RGBA preview and doing RGBA->YUV towards the encoder gives
     * both streams correct content for less work. */
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

    /* acquire_fence is already waited for and closed at function entry (see the fence leak fix above). */

    ALOGI("Processing capture: frame=%llu stream=%dx%d format=%d pipeline=%dx%d",
          (unsigned long long)request->frame_number,
          buf.stream->width, buf.stream->height, buf.stream->format,
          dev->pipeline_width, dev->pipeline_height);

 
    bool frameCaptured = false;

    /* [2026-11-22 full-screen preview] With multiple streams (preview + encoder, or
     * preview + photo YUV), capture one frame at the pipeline resolution into a scratch
     * RGBA buffer, then crop and scale per stream with that stream's own aspect ratio.
     * A 4:3 preview is a centre crop of a 16:9 capture; scaling the preview and then
     * blowing it up to fill a 16:9 encoder stream stretches the picture horizontally.
     * Independent per-stream crops are correct and also give photos and recordings a
     * real 1080p instead of an upscaled 720p preview.
     * Single-stream cases (preview only, BLOB only) keep the zero-copy direct write. */
    uint32_t nonBlobStreams = 0, maxNonBlobArea = 0;
    bool hasBlobStream = false;
    for (uint32_t i = 0; i < request->num_output_buffers; i++) {
        const camera3_stream_t* s = request->output_buffers[i].stream;
        if (!s) continue;
        if (s->format == HAL_PIXEL_FORMAT_BLOB) { hasBlobStream = true; continue; }
        nonBlobStreams++;
        const uint32_t area = s->width * s->height;
        if (area > maxNonBlobArea) maxNonBlobArea = area;
    }
    /* This path is only needed when the streams disagree on aspect ratio: then "shrink
     * into the preview, then blow up into the encoder" would distort the picture. Two
     * streams with the same ratio (the usual recording case) are cheaper through the
     * zero-copy direct write, and avoid touching CameraX's GL output surface
     * (multi-stream software writes triggered libglcore crashes in testing). */
    bool aspectMismatch = false;
    if (!hasBlobStream && nonBlobStreams > 1 && captureBuf->stream) {
        const uint64_t refW = captureBuf->stream->width;
        const uint64_t refH = captureBuf->stream->height;
        for (uint32_t i = 0; i < request->num_output_buffers; i++) {
            const camera3_stream_t* s = request->output_buffers[i].stream;
            if (!s || s->format == HAL_PIXEL_FORMAT_BLOB) continue;
            if ((uint64_t)s->width * refH != refW * (uint64_t)s->height) {
                aspectMismatch = true;
                break;
            }
        }
    }
    /* [2026-11-22 recording without keyframes] Any request with more than one output
     * stream must go through the fan-out: a recording session configures preview
     * (RGBA 4:3) + encoder (YV12) + BLOB, and with equal sizes the aspect ratios match.
     * When the condition was tightened to "only on aspect mismatch", the encoder stream
     * was skipped entirely in the single-stream branch, so MediaRecorder never got a
     * keyframe (OpenCamera and Aperture recordings produced nothing). aspectMismatch now
     * only affects cropping, which each branch handles itself. */
    (void)aspectMismatch;
    if (dev->pipeline && dev->streams_configured &&
        request->num_output_buffers > 1) {
        mocha::CameraPipeline* pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
        if (pipeline->getState() == mocha::PIPELINE_OPENED) {
            ALOGI("process_capture_request: pipeline in OPENED state, re-starting streaming");
            if (pipeline->startStreaming() != 0) {
                ALOGE("Auto-restart streaming failed");
                if (dev->inflight_tracker) dev->inflight_tracker->remove(frameNum);
                return -ENODEV;
            }
        }
        uint32_t pipeW = dev->pipeline_width, pipeH = dev->pipeline_height;
        /* [2026-11-22 high-resolution capture] When the photo stream is larger than the
         * capture size, switch capture to the photo size briefly and switch back right
         * after: the preview keeps its fast 30 fps size while the photo is a real 5 MP
         * frame, not an upscaled 1080p. */
        uint32_t restoreW = 0, restoreH = 0;
        if (hasBlobStream) {
            uint32_t wantW = 0, wantH = 0, best = pipeW * pipeH;
            for (uint32_t i = 0; i < request->num_output_buffers; i++) {
                const camera3_stream_t* bs = request->output_buffers[i].stream;
                if (!bs || bs->format != HAL_PIXEL_FORMAT_BLOB) continue;
                const uint32_t a = bs->width * bs->height;
                if (a > best) { best = a; wantW = bs->width; wantH = bs->height; }
            }
            if (wantW && wantH) {
                const int keepExp = pipeline->getExposure();
                const int keepGain = pipeline->getGain();
                if (openPipelineAt(dev, wantW, wantH, keepExp, keepGain) == 0) {
                    restoreW = pipeW; restoreH = pipeH;
                    pipeW = wantW; pipeH = wantH;
                    /* openPipelineAt() already deleted the old object, so the pointer must be fetched again. */
                    pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
                    ALOGI("still capture: pipeline switched to %ux%u for the shot", pipeW, pipeH);
                } else {
                    ALOGE("still capture: switch to %ux%u failed, upscaling instead",
                          wantW, wantH);
                }
            }
        }
        const uint32_t rgbaSize = pipeW * pipeH * 4;
        if (!dev->temp_rgba || dev->temp_rgba_size < rgbaSize) {
            if (dev->temp_rgba) free(dev->temp_rgba);
            dev->temp_rgba = (uint8_t*)malloc(rgbaSize);
            dev->temp_rgba_size = rgbaSize;
        }
        /* [2026-11-22] Right after a sensor mode switch AE/AWB reconverge from
         * defaults and capturing immediately gives a dark, green-tinted frame (measured
         * at 8 MP). Drop a few frames for 3A before capturing. */
        if (restoreW && dev->temp_rgba) {
            for (int warm = 0; warm < 5; warm++)
                (void)pipeline->captureFrameRgba(dev->temp_rgba, pipeW, pipeH);
        }
        /* [2026-11-22] Right after a sensor mode switch AE/AWB reconverge from
         * defaults and capturing immediately gives a dark, green-tinted frame (measured
         * at 8 MP). Drop a few frames for 3A before capturing. */
        if (restoreW && dev->temp_rgba) {
            for (int warm = 0; warm < 5; warm++)
                (void)pipeline->captureFrameRgba(dev->temp_rgba, pipeW, pipeH);
        }
        int captureRet = -ENOMEM;
        if (dev->temp_rgba)
            captureRet = pipeline->captureFrameRgba(dev->temp_rgba, pipeW, pipeH);
        /* [2026-11-22 recording -38] A failed capture (including -EAGAIN) no longer
         * returns: the CamDevSession wrapper turns any non-BAD_VALUE error into a fatal
         * one and the recording session is torn down. Fall through to the else branch and
         * report a REQUEST error instead. */
        if (captureRet == 0) {
            bool allOk = true;
            for (uint32_t i = 0; i < request->num_output_buffers; i++) {
                const camera3_stream_buffer_t& out = request->output_buffers[i];
                const camera3_stream_t* s = out.stream;
                if (!s || !out.buffer) continue;
                if (s->format == HAL_PIXEL_FORMAT_BLOB) {
                    const uint32_t bw = s->width, bh = s->height;
                    const uint32_t need = bw * bh * 4;
                    if (!dev->temp_jpeg_rgba || dev->temp_jpeg_size < need) {
                        if (dev->temp_jpeg_rgba) free(dev->temp_jpeg_rgba);
                        dev->temp_jpeg_rgba = (uint8_t*)malloc(need);
                        dev->temp_jpeg_size = need;
                    }
                    if (dev->temp_jpeg_rgba) {
                        scaleRgbaNearest(dev->temp_rgba, (int)pipeW, (int)pipeH,
                                         dev->temp_jpeg_rgba, (int)bw, (int)bh);
                        if (!writeJpegToBlob(dev, out, dev->temp_jpeg_rgba, bw, bh))
                            allOk = false;
                    } else {
                        ALOGE("multi-stream: no scratch buffer for JPEG");
                        allOk = false;
                    }
                    continue;
                }
                if (s->format == HAL_PIXEL_FORMAT_YCBCR_420_888 ||
                    s->format == HAL_PIXEL_FORMAT_YV12 || isEncoderStream(s)) {
                    if (!writeYuvFromRgba(&out, dev->temp_rgba, (int)pipeW, (int)pipeH))
                        allOk = false;
                } else {
                    if (!writeRgbaToSurface(dev, out, dev->temp_rgba, pipeW, pipeH, pipeline)) {
                        ALOGE("multi-stream: preview buffer not writable");
                        allOk = false;
                    }
                }
            }
            frameCaptured = allOk;
        } else {
            ALOGE("multi-stream capture failed: %d", captureRet);
        }
        if (restoreW && restoreH) {
            if (openPipelineAt(dev, restoreW, restoreH) != 0)
                ALOGE("still capture: restore to %ux%u failed", restoreW, restoreH);
            else
                ALOGI("still capture: pipeline restored to %ux%u", restoreW, restoreH);
        }
    } else if (dev->pipeline && dev->streams_configured) {
    // Capture a frame from the pipeline (single-stream zero-copy direct write)
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
            /* [2026-11-22] Stills come out at the BLOB stream size: the JPEG width and
             * height must match what the stream declared, otherwise the framework or app
             * throws the picture away (measured with a 4:3 photo stream plus a 16:9
             * encoder: one "JPEG captured" line and no file in DCIM). captureFrameRgba()
             * crops and scales, so capture at the BLOB size and a 4:3 photo becomes a
             * centre crop of the 16:9 capture, matching the preview framing. */
            const uint32_t blobW = buf.stream->width;
            const uint32_t blobH = buf.stream->height;
            const uint32_t rgbaSize = blobW * blobH * 4;
            if (!dev->temp_rgba || dev->temp_rgba_size < rgbaSize) {
                if (dev->temp_rgba) free(dev->temp_rgba);
                dev->temp_rgba = (uint8_t*)malloc(rgbaSize);
                dev->temp_rgba_size = rgbaSize;
            }
            if (!dev->temp_rgba) {
                ALOGE("Failed to allocate temp RGBA buffer");
            } else {
                /* Single-stream still capture (what Aperture/CameraX do: the BLOB gets its
                 * own request): if the capture size is smaller than the BLOB, switch up
                 * briefly, drop a few frames for 3A, capture and switch back.
                 * writeJpegToBlob() locks by image geometry and skips the trailer, same as
                 * the multi-stream path. */
                const uint32_t keepW = dev->pipeline_width, keepH = dev->pipeline_height;
                bool switched = false;
                if (blobW * blobH > keepW * keepH) {
                    const int keepExp0 = pipeline->getExposure();
                    const int keepGain0 = pipeline->getGain();
                    if (openPipelineAt(dev, blobW, blobH, keepExp0, keepGain0) == 0) {
                        switched = true;
                        pipeline = static_cast<mocha::CameraPipeline*>(dev->pipeline);
                        ALOGI("still capture: pipeline switched to %ux%u for the shot", blobW, blobH);
                        for (int warm = 0; warm < 5; warm++)
                            (void)pipeline->captureFrameRgba(dev->temp_rgba, blobW, blobH);
                    } else {
                        ALOGE("still capture: switch to %ux%u failed, upscaling", blobW, blobH);
                    }
                }
                int captureRet = pipeline->captureFrameRgba(dev->temp_rgba, blobW, blobH);
                if (switched) {
                    if (openPipelineAt(dev, keepW, keepH) != 0)
                        ALOGE("still capture: restore to %ux%u failed", keepW, keepH);
                }
                if (captureRet == 0) {
                    camera3_stream_buffer_t blobBuf;
                    memset(&blobBuf, 0, sizeof(blobBuf));
                    blobBuf.buffer = buf.buffer;
                    blobBuf.stream = buf.stream;
                    frameCaptured = writeJpegToBlob(dev, blobBuf, dev->temp_rgba, blobW, blobH);
                } else {
                    /* [2026-11-22 recording -38] -EAGAIN does not return either, for the reason above. */
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
                }
                /* [2026-11-22 recording -38] On captureRet == -EAGAIN do not unlock and return
                 * -EAGAIN (fatal error mapping); use the shared error path at the end. */
                android::GraphicBufferMapper::get().unlock(handle);
            }
        } else {
            /* Preview (RGBA/IMPLEMENTATION_DEFINED): capture into a packed buffer first, then
             * write it out row by row using the surface's real stride (see the
             * writeRgbaToSurface comment). */
            const uint32_t w = buf.stream->width, h = buf.stream->height;
            const uint32_t need = w * h * 4;
            if (!dev->temp_rgba || dev->temp_rgba_size < need) {
                if (dev->temp_rgba) free(dev->temp_rgba);
                dev->temp_rgba = (uint8_t*)malloc(need);
                dev->temp_rgba_size = need;
            }
            frameCaptured = writeRgbaToSurface(dev, buf, nullptr, w, h, pipeline);
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
    /* [2026-11-18] The result metadata needs this sensor's active array size (crop
     * region) and build_result_metadata() has no camera id, so the currently open camera
     * is kept in a file-level variable. It used to be hard-coded to 3280x2464, so the
     * front camera (2624x1956) reported a crop region larger than its sensor and
     * CameraX's crop maths came out wrong. */
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
