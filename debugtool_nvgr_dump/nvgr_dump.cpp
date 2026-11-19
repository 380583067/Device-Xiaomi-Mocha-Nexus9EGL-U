/*
 * nvgr_dump -- print the memory layout of a gralloc buffer handle the way
 * libnvgr's surface accessors see it, next to the raw words of the handle.
 *
 * The allocation goes through AHardwareBuffer (the same HIDL allocator path
 * SurfaceFlinger uses), so the handle arrives in this process exactly as a
 * client target arrives in the composer -- after the binder round trip, not
 * as the raw object the allocator handed its own caller.
 *
 * Build: mmm device/xiaomi/mocha/debugtool_nvgr_dump
 * Run:   nvgr_dump            (as root, on the device)
 */

#include <vndk/hardware_buffer.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <cutils/native_handle.h>

/* What the composer reads; see hwc2/bufferinfo/NvGralloc.cpp */
enum SurfaceWord {
    kWidth = 0,
    kHeight = 1,
    kColorFormat = 2,
    kLayout = 4,
    kPitch = 5,
    kOffset = 7,
    kKind = 8,
    kBlockHeightLog2 = 9,
};

struct NvGrApi {
    int (*is_valid)(const buffer_handle_t);
    int (*get_memfd)(const buffer_handle_t);
    int (*get_format)(const buffer_handle_t);
    int (*get_surfaces)(const buffer_handle_t, const void **, size_t *);
};

static void *open_nvgr(NvGrApi *api) {
    void *lib = dlopen("libnvgr.so", RTLD_NOW);
    if (!lib) {
        printf("dlopen libnvgr.so: %s\n", dlerror());
        return nullptr;
    }
    api->is_valid = (int (*)(const buffer_handle_t))dlsym(lib, "nvgr_is_valid");
    api->get_memfd = (int (*)(const buffer_handle_t))dlsym(lib, "nvgr_get_memfd");
    api->get_format = (int (*)(const buffer_handle_t))dlsym(lib, "nvgr_get_format");
    api->get_surfaces = (int (*)(const buffer_handle_t, const void **, size_t *))
            dlsym(lib, "nvgr_get_surfaces");
    printf("nvgr symbols: is_valid=%p memfd=%p format=%p surfaces=%p\n",
           (void *)api->is_valid, (void *)api->get_memfd,
           (void *)api->get_format, (void *)api->get_surfaces);
    return lib;
}

static void dump_words(const char *what, const uint32_t *words, size_t count) {
    printf("%s:\n", what);
    for (size_t i = 0; i < count; ++i) {
        printf("  word[%zu] = %10u (0x%08x)%s\n", i, words[i], words[i],
               i == kPitch ? "   <-- composer's kPitch" : "");
    }
}

int main() {
    NvGrApi api = {};
    open_nvgr(&api);

    /* Panel-sized RGBA client target. */
    AHardwareBuffer_Desc desc = {};
    desc.width = 1536;
    desc.height = 2048;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

    AHardwareBuffer *ahb = nullptr;
    int rc = AHardwareBuffer_allocate(&desc, &ahb);
    printf("AHardwareBuffer_allocate 1536x2048 RGBA -> %d\n", rc);
    if (rc != 0 || !ahb)
        return 1;

    const native_handle_t *in = AHardwareBuffer_getNativeHandle(ahb);
    printf("handle from AHB: %p\n", (void *)in);

    /* The composer sees the handle after a binder round trip; mimic it by
     * cloning, which is what HIDL's handle un-marshalling does. */
    native_handle_t *nh = native_handle_clone(in);
    printf("cloned handle: numFds=%d numInts=%d version=%d\n", nh->numFds,
           nh->numInts, nh->version);
    for (int i = 0; i < nh->numFds && i < 8; ++i)
        printf("  fd[%d] = %d\n", i, nh->data[i]);
    dump_words("handle int words (data[numFds..], as 32-bit runs)",
               reinterpret_cast<const uint32_t *>(nh->data + nh->numFds),
               nh->numInts > 24 ? 24 : nh->numInts);

    if (api.is_valid) {
        printf("nvgr_is_valid(handle) = %d\n", api.is_valid(nh));
        printf("nvgr_get_format(handle) = 0x%x\n", api.get_format(nh));
        printf("nvgr_get_memfd(handle) = %d\n", api.get_memfd(nh));
        const void *surfaces = nullptr;
        size_t count = 0;
        int src = api.get_surfaces(nh, &surfaces, &count);
        printf("nvgr_get_surfaces -> rc=%d count=%zu ptr=%p\n", src, count,
               surfaces);
        if (surfaces && count > 0)
            dump_words("nvgr surface 0 words",
                       static_cast<const uint32_t *>(surfaces), 16);
    } else {
        printf("libnvgr accessors unavailable\n");
    }

    AHardwareBuffer_release(ahb);
    native_handle_delete(nh);
    return 0;
}
