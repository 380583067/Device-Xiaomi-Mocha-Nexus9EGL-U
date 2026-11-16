/*
 * yv12locktest — mocha (Tegra K1 / TN8) gralloc YUV lock 能力诊断工具
 *
 * 背景：
 *   Android 14 的 SoftwareRenderer 在渲染软件解码的 YUV420 帧时，会把
 *   Surface buffer 配置成 HAL_PIXEL_FORMAT_YV12，然后调用
 *   GraphicBufferMapper::lockYCbCr() 拿到 Y/Cb/Cr 三个平面指针。
 *   在 mocha 上该调用返回 -ENOSYS(-38)，导致 SoftwareRenderer 的
 *   CHECK_EQ(0, ...) 直接 abort，图库一播视频就闪退。
 *
 * 本工具用来确认：
 *   1) 该 gralloc 的 lockYCbCr 对哪些 YUV 格式可用；
 *   2) 若 lockYCbCr 不可用，单平面 lock() 是否能拿到 YV12 的基址
 *      （决定 SoftwareRenderer 兜底方案是"手工算平面"还是"强制 RGB565 转换"）。
 *
 * 仅用于调试，不随正式版本发布。
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

    // 1) 走 lockYCbCr（SoftwareRenderer 当前用的路径）
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

    // 2) 走单平面 lock（兜底方案要用它拿基址，再自己算 YV12 平面）
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
