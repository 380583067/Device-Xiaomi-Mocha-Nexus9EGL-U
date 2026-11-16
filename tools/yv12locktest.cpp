/*
 * yv12locktest - diagnostic tool for gralloc YUV lock on mocha (Tegra K1 / TN8)
 *
 * Background:
 *   When SoftwareRenderer in Android 14 renders a software-decoded YUV420
 *   frame, it configures the Surface buffer as HAL_PIXEL_FORMAT_YV12 and then
 *   calls GraphicBufferMapper::lockYCbCr() to obtain the Y/Cb/Cr plane
 *   pointers. On mocha that call returns -ENOSYS (-38), so SoftwareRenderer's
 *   CHECK_EQ(0, ...) aborts and the Gallery app crashes as soon as any video is
 *   played.
 *
 * This tool is used to determine:
 *   1) which YUV formats this gralloc can actually lock via lockYCbCr;
 *   2) if lockYCbCr is unavailable, whether a plain single-plane lock() still
 *      returns the YV12 base address. This decides whether the SoftwareRenderer
 *      fallback should compute the planes by hand or force an RGB565 conversion.
 *
 * Debug only, never shipped in release builds.
 */

#define LOG_TAG "yv12locktest"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

using namespace android;

namespace {

struct FormatCase {
    const char* name;
    uint32_t format;
};

const FormatCase kCases[] = {
    { "YV12 (HAL_PIXEL_FORMAT_YV12)",            HAL_PIXEL_FORMAT_YV12 },
    { "YCbCr_420_888",                           HAL_PIXEL_FORMAT_YCbCr_420_888 },
    { "NV12 fourcc 0x3231564e",                  0x3231564e },
    { "NV21 fourcc 0x3132564e",                  0x3132564e },
};

void testFormat(const FormatCase& c, int w, int h) {
    printf("\n===== %s  (%dx%d) =====\n", c.name, w, h);

    sp<GraphicBuffer> buffer = new GraphicBuffer(
            w, h, static_cast<PixelFormat>(c.format),
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_RARELY |
            GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP);

    status_t err = buffer->initCheck();
    if (err != NO_ERROR) {
        printf("  alloc FAILED: %d (%s)\n", err, strerror(-err));
        return;
    }
    printf("  alloc ok: stride=%u height=%u format=0x%x\n",
           buffer->getStride(), buffer->getHeight(), buffer->getPixelFormat());

    GraphicBufferMapper& mapper = GraphicBufferMapper::get();
    const Rect bounds(w, h);
    const native_handle_t* handle = buffer->getNativeBuffer()->handle;

    // 1) Via lockYCbCr - the path SoftwareRenderer currently uses.
    android_ycbcr ycbcr;
    memset(&ycbcr, 0, sizeof(ycbcr));
    status_t e1 = mapper.lockYCbCr(handle,
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_RARELY,
            bounds, &ycbcr);
    if (e1 == NO_ERROR) {
        printf("  lockYCbCr OK: y=%p cb=%p cr=%p ystride=%zu cstride=%zu chroma_step=%zu\n",
               ycbcr.y, ycbcr.cb, ycbcr.cr,
               ycbcr.ystride, ycbcr.cstride, ycbcr.chroma_step);
        mapper.unlock(handle);
    } else {
        printf("  lockYCbCr FAILED: %d (%s)\n", e1, strerror(-e1));
    }

    // 2) Via plain single-plane lock - the fallback needs this to get the base
    //    address and then compute the YV12 planes itself.
    void* vaddr = nullptr;
    status_t e2 = mapper.lock(handle,
            GRALLOC_USAGE_SW_READ_NEVER | GRALLOC_USAGE_SW_WRITE_RARELY,
            bounds, &vaddr);
    if (e2 == NO_ERROR) {
        printf("  lock       OK: vaddr=%p\n", vaddr);
        mapper.unlock(handle);
    } else {
        printf("  lock       FAILED: %d (%s)\n", e2, strerror(-e2));
    }
}

}  // namespace

int main(int argc, char** argv) {
    int w = 1280, h = 720;
    if (argc >= 3) {
        w = atoi(argv[1]);
        h = atoi(argv[2]);
    }

    printf("gralloc YUV lock capability test (%dx%d)\n", w, h);
    for (const auto& c : kCases) {
        testFormat(c, w, h);
    }
    printf("\ndone\n");
    return 0;
}
