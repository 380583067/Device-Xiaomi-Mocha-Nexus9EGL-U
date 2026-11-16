/*
 * libmocha_camera_shim.so
 * ============================================================================
 * ABI compatibility layer for the camera vendor blob on Xiaomi Mi Pad 1
 *
 * Background
 * ---------------------------------------------------------------------------
 * camera.vendor.tegra.so is a binary blob compiled by NVIDIA in the Android 5.1 (2015) era,
 * loaded by CameraWrapper (camera.tegra.so) through hw_get_module_by_class("camera", "vendor").
 * On Android 14 (LineageOS 21) most of the blob's 345 undefined symbols still resolve against
 * stable ABIs such as libc / pthread / liblog, but a batch of Android C++ ABI symbols changed
 * shape or disappeared, so dlopen fails with "cannot locate symbol ..." and no camera exists.
 *
 * Missing symbols, measured one by one (2026-11, symbol-level diff of all 1056 .so in out/)
 * ---------------------------------------------------------------------------
 *  1) android::CameraMetadata::getAndLock()
 *     android::CameraMetadata::unlock(const camera_metadata*)
 *     -- the method became const in Android 14 (mangled prefix ZNK instead of ZN).
 *        libcamera_client.so exports both const variants, only the non-const one is gone.
 *        -> forward the non-const overload to the const one; zero cost.
 *
 *  2) android::Fence::wait(unsigned int)
 *     -- Android 14 changed the signature to wait(int timeout), so mangle waitEj became waitEi.
 *        -> forward and reinterpret uint32 as int32 (TIMEOUT_NEVER is -1 on both sides).
 *
 *  3) android::Fence::~Fence()
 *     -- Android 14 declares `virtual ~Fence() = default;` (Fence.h:151), so the compiler
 *        emits a COMDAT weak symbol that libui does not export, and the blob's reference to
 *        _ZN7android5FenceD1Ev cannot be resolved. Both versions have the same layout
 *        { vptr; int mFenceFd; } (Android 14 uses base::unique_fd, which is an int). We read
 *
 *  4) android::SensorManager::SensorManager()               (no-arg constructor)
 *     android::SensorManager::createEventQueue()            (no-arg)
 *     android::Singleton<SensorManager>::sInstance
 *     android::Singleton<SensorManager>::sLock
 *     -- Android 5 SensorManager inherits Singleton<TS>, and the blob inlined getInstance() as
 *        "lock; if sInstance is null then new SensorManager(); return *sInstance".
 *        Android 14 replaced this with getInstanceForPackage(String16, int) and the class is no
 *        longer a Singleton. Handling:
 *          a) export sInstance / sLock under the old names from .bss so the blob's inlined code
 *             keeps working;
 *          b) preset sInstance in an ELF constructor to the real system singleton
 *             (getInstanceForPackage), so the blob's "if (!sInstance) new ..." branch is never
 *             taken. This matters: the blob allocates using the Android 5 sizeof, and the
 *             Android 14 SensorManager is larger, so a real new would corrupt the heap;
 *          c) give the no-arg constructor an empty body (link-only, never called in practice);
 *          d) forward the no-arg createEventQueue() to the new three-argument version.
 *
 * Out of scope for this shim (verified as needing no work)
 * ---------------------------------------------------------------------------
 *  - NvSFxFloat2Fixed: exported by libnvisp_v3.so, which is a NEEDED of libnvmm_camera_v3.so
 *    (a direct dependency of the blob), so the symbol is globally visible at runtime.
 *  - libcameraservice.so / libpowerservice_client.so: the blob's DT_NEEDED requires both files
 *    to exist, but the former has no symbol references at all (an empty stub suffices) and the
 *    latter only needs the PowerServiceClient constructor plus sendPowerHint. They are produced
 *    by stub_libcameraservice.cpp / stub_libpowerservice_client.cpp in this directory.
 *  - std::string COW symbols (_ZNSs*): measured count is 0, the blob never hits GNU libstdc++.
 *    std::string ABI。
 *
 * Warning (hard-learned, see libbeautify_stub.cpp / mocha_cpp_shim.cpp)
 * ---------------------------------------------------------------------------
 * This shim is preloaded via TARGET_LD_SHIM_LIBS and therefore joins the process *shared group*:
 * every symbol it exports is visible to all libraries in cameraserver. Only define symbols that
 * truly do not exist anywhere else; never redefine operator new/delete or __throw_* style
 * symbols used by the whole process. Doing so previously made libutils segfault in call_constructors.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <dlfcn.h>

#include <ios>
#include <new>

#include <log/log.h>
#include <sensor/SensorEventQueue.h>
#include <sensor/SensorManager.h>
#include <utils/Mutex.h>
#include <utils/String16.h>
#include <utils/String8.h>
#include <utils/threads.h>

/*
 * camera_metadata_t is deliberately kept as an opaque pointer (void*) here, without including
 * <camera/camera_metadata.h>：
 *   - the names below are *hand-written mangled names* (extern "C"), so a symbol only depends
 *     on the type encoding inside the mangled name, not on the C++ types declared in this TU;
 *   - it avoids pulling the libcamera_metadata headers into this shim (that header lives under
 *     system/media/camera/include and would need one more include path in the Make file).
 */

/* ==========================================================================
 * 1) CameraMetadata: non-const -> const forwarding
 * ========================================================================== */

// Const variant that actually exists in libcamera_client.so.
extern "C" void* _ZNK7android14CameraMetadata10getAndLockEv(const void* thiz);
extern "C" int _ZNK7android14CameraMetadata6unlockEPK15camera_metadata(const void* thiz,
                                                                     const void* md);

// Non-const variant referenced by the blob (Android 5 ABI).
extern "C" void* _ZN7android14CameraMetadata10getAndLockEv(void* thiz) {
    return _ZNK7android14CameraMetadata10getAndLockEv(thiz);
}

extern "C" int _ZN7android14CameraMetadata6unlockEPK15camera_metadata(void* thiz,
                                                                     const void* md) {
    return _ZNK7android14CameraMetadata6unlockEPK15camera_metadata(thiz, md);
}

/* ==========================================================================
 * 2) Fence
 * ========================================================================== */

// New signature: status_t Fence::wait(int timeout)
extern "C" int _ZN7android5Fence4waitEi(void* thiz, int timeout);

// Old signature: status_t Fence::wait(unsigned int timeout)
// A bit-cast is enough: old code passes Fence::TIMEOUT_NEVER as 0xFFFFFFFF, which reads as -1
// as an int, and Android 14 TIMEOUT_NEVER is also -1.
extern "C" int _ZN7android5Fence4waitEj(void* thiz, unsigned int timeout) {
    return _ZN7android5Fence4waitEi(thiz, static_cast<int>(timeout));
}

/*
 * Fence object layout (identical in both versions):
 *   offset 0 : vptr
 *   offset 4 : int mFenceFd      (Android 14 uses base::unique_fd, internally an int)
 * 8 bytes in total.
 *
 * The destructor only has to close the fd. libui's `virtual ~Fence() = default;` does not
 * export a symbol, so this logic has to be supplied here.
 */
static inline void mocha_fence_destroy(void* thiz) {
    if (thiz == nullptr) return;
    int* fd = reinterpret_cast<int*>(reinterpret_cast<char*>(thiz) + sizeof(void*));
    if (*fd >= 0) {
        ::close(*fd);
        *fd = -1;
    }
}

extern "C" void _ZN7android5FenceD1Ev(void* thiz) { mocha_fence_destroy(thiz); }
extern "C" void _ZN7android5FenceD2Ev(void* thiz) { mocha_fence_destroy(thiz); }

extern "C" void _ZN7android5FenceD0Ev(void* thiz) {
    mocha_fence_destroy(thiz);
    ::operator delete(thiz);
}

/* ==========================================================================
 * 3) SensorManager / Singleton<SensorManager>
 * ========================================================================== */

/*
 * The inlined getInstance() in the blob accesses these two legacy symbols directly:
 *   static Mutex sLock;             -> Singleton<SensorManager>::sLock
 *   static SensorManager* sInstance;-> Singleton<SensorManager>::sInstance
 *
 * sLock is large zero-initialised memory: bionic's PTHREAD_MUTEX_INITIALIZER is all zeros, so
 * pthread_mutex_lock() on this storage is legal and needs no explicit construction.
 */
extern "C" {
void* _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = nullptr;
__attribute__((aligned(16))) unsigned char _ZN7android9SingletonINS_13SensorManagerEE5sLockE[128] = {};
}

/*
 * No-arg constructor: only there so the dynamic linker resolves.
 * The blob never reaches it in the normal path (sInstance is preset in the constructor),
 * and even if it did, a placement-new of a real SensorManager is not allowed here:
 * the blob allocated using Android 5 sizeof while the new object is larger, corrupting the heap.
 */
extern "C" void _ZN7android13SensorManagerC2Ev(void* /*thiz*/) {}
extern "C" void _ZN7android13SensorManagerC1Ev(void* thiz) { _ZN7android13SensorManagerC2Ev(thiz); }

/*
 * No-arg createEventQueue(): returns sp<SensorEventQueue>, a non-trivial return type, so it
 * uses the sret hidden parameter per AAPCS and effectively has the signature
 *     void f(sp<SensorEventQueue>* ret, SensorManager* this)
 * This forwards to the Android 14 three-argument version (all three have defaults).
 */
extern "C" void _ZN7android13SensorManager16createEventQueueEv(void* ret, void* thiz) {
    android::SensorManager* mgr = static_cast<android::SensorManager*>(thiz);
    android::sp<android::SensorEventQueue> queue;
    if (mgr != nullptr) {
        queue = mgr->createEventQueue(android::String8("mocha.camera"), 0,
                                      android::String16(""));
    }
    new (ret) android::sp<android::SensorEventQueue>(queue);
}

/*
 * Preset sInstance to the real system SensorManager singleton.
 *
 * Timing: this library is preloaded by TARGET_LD_SHIM_LIBS at process start, so the constructor
 * runs before cameraserver's main(). Binder is already available at that point
 * (libsensor's getInstanceForPackage asserts state and retries internally).
 *
 * It has to happen here rather than on first call: the blob's inlined getInstance() logic is
 * "if sInstance is null, new it myself". Once that runs we lose the chance to take over, and it
 * would allocate an object of the wrong size.
 */
__attribute__((constructor)) static void mocha_camera_shim_init() {
    /*
    * Load libcameraservice_mocha.so first.
     *
    * Reason: camera.vendor.tegra.so lists "libcameraservice.so" in DT_NEEDED, but that file
    * does not exist on Android 14 (libcameraservice is a static_libs of cameraserver, purely
    * statically linked, producing no shared library). We build an empty library as a stand-in, but
    *   - the module cannot be named libcameraservice (clashes with the same-named cc_library),
    *   - SHARED_LIBRARY does not allow LOCAL_MODULE_STEM,
    * so the artifact is libcameraservice_mocha.so and only its ELF header declares the SONAME
    * as libcameraservice.so.
     *
    * bionic's linker checks the soname index of already-loaded libraries first in find_library()
    * (linker.cpp:1468 find_loaded_library_by_soname), so loading libcameraservice_mocha.so here
    * makes the blob's DT_NEEDED "libcameraservice.so" resolve to its SONAME without a real file.
     *
     *
    * Note: do not rely on the shim's own DT_NEEDED for this dependency - ld would write the
    * linked library's SONAME (libcameraservice.so) into the NEEDED section, and that name has no
    * matching file, which would make the shim itself fail to load. Hence an explicit dlopen.
     *
    * BoardConfig.mk also lists libcameraservice_mocha.so as the second shim for
    * camera.vendor.tegra.so in TARGET_LD_SHIM_LIBS, so either path works; dlopen is idempotent.
     *
     */
    if (dlopen("libcameraservice_mocha.so", RTLD_NOW | RTLD_GLOBAL) == nullptr) {
        ALOGW("mocha_camera_shim: dlopen libcameraservice_mocha.so failed: %s", dlerror());
    }

    if (_ZN7android9SingletonINS_13SensorManagerEE9sInstanceE == nullptr) {
        android::SensorManager& mgr =
                android::SensorManager::getInstanceForPackage(android::String16("mocha.camera"));
        _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = &mgr;
    }
}

/* ==========================================================================
 * 4) Legacy API of NVIDIA libphs: NvPHSSetThrottle
 * ========================================================================== */

/*
 * This one is not requested by the blob itself but by its dependency libcuda.so - checking
 * camera.vendor.tegra.so alone does not reveal it, the whole dependency tree must be walked.
 *
 * Symptom:
 *   dlopen("/system/lib/hw/camera.vendor.tegra.so") failed:
 *     cannot locate symbol "NvPHSSetThrottle" referenced by
 *     "/system/vendor/lib/libcuda.so"
 *
 * Cause:
 *   libcuda.so (libphs.so in DT_NEEDED) expects libphs to provide two APIs:
 *     NvPHSSetThrottle         (old interface)
 *     NvPHSSendThroughputHints (new interface)
 *   but libphs.so shipped in this ROM only has the latter:
 *     NvPHSSendThroughputHints / NvPHSCancelThroughputHints
 *     NvVaSendThroughputHints  / NvCancelThroughputHints
 *   NVIDIA folded SetThrottle into SendThroughputHints in later versions.
 *
 * Handling:
 *   NvPHSSetThrottle means "apply a frequency cap to GPU/EMC" (a performance hint) and is
 *   unrelated to camera functionality. It is a no-op returning 0 (NvSuccess); the worst case is
 *   that the GPU is not downclocked under heavy camera load - viewfinder, photo, video unaffected.
 *
 * Visibility: this shim is built with DF_1_GLOBAL (see -Wl,-z,global in Android.mk) and loads
 * before libcuda.so, so the symbol is visible to libcuda.
 */
extern "C" int NvPHSSetThrottle() {
    return 0;  // NvSuccess
}

/* ==========================================================================
 * 5) New NvRmSync APIs (libnvrm.so in this ROM is old and lacks them)
 * ========================================================================== */

/*
 * Again not requested by the blob directly but by libnvrm_gpu.so:
 *   camera.vendor.tegra.so -> libcuda.so -> libnvrm_gpu.so
 *
 * Symptom:
 *   dlopen failed: cannot locate symbol
 *     "NvRmSyncCalculateSizeForInplaceSyncpoints" referenced by
 *     "/system/vendor/lib/libnvrm_gpu.so"
 *
 * Cause:
 *   libnvrm_gpu.so in this blob set is newer than libnvrm.so. The old libnvrm.so only exports:
 *     NvRmSyncCreateFromFd / NvRmSyncCreateFromSyncpoints
 *   while libnvrm_gpu.so also looks for the "WithAttr" and "CalculateSizeForInplace" variants
 *   that only newer libnvrm.so provides. No library in the whole blob set exports these 4 symbols.
 *
 * Strategy (important):
 *   These APIs belong to the sync path that turns a GPU fence into an NvRmSync handle and are
 *   only used from the CUDA-accelerated branches (EDR/HDR merge, face acceleration, JPEG).
 *   Real semantics cannot be implemented, so *fail explicitly* instead of pretending success:
 *     - CalculateSizeForInplace* returns 0 (no extra space needed);
 *     - Create*WithAttr first writes NULL to the output handle, then returns NvError_NotImplemented,
 *       so even a caller that ignores the return code cannot pass a wild pointer to NvRmSyncClose
 *       (which is where a real crash would happen).
 *   As long as the CUDA branches are never taken, none of these run: zero impact.
 */
#define MOCHA_NVERROR_NOT_IMPLEMENTED 0x000C0000u

extern "C" unsigned int NvRmSyncCalculateSizeForInplaceSyncpoints() {
    return 0;
}

extern "C" unsigned int NvRmSyncCalculateSizeForInplaceFd() {
    return 0;
}

// NvError NvRmSyncCreateFromFdWithAttr(NvRmSyncHandle* sync, NvRmHandle hRm,
//                                      NvRmSyncAttr* attr, int fd)
extern "C" unsigned int NvRmSyncCreateFromFdWithAttr(void** sync) {
    if (sync != nullptr) *sync = nullptr;
    return MOCHA_NVERROR_NOT_IMPLEMENTED;
}

// NvError NvRmSyncCreateFromSyncpointsWithAttr(NvRmSyncHandle* sync, NvRmHandle hRm,
//                                              NvRmSyncAttr* attr, NvU32 num,
//                                              NvRmSyncPoint* syncpts)
extern "C" unsigned int NvRmSyncCreateFromSyncpointsWithAttr(void** sync) {
    if (sync != nullptr) *sync = nullptr;
    return MOCHA_NVERROR_NOT_IMPLEMENTED;
}

/* ==========================================================================
 * 6) GNU libstdc++ iostream symbols (same fix as in mocha_cpp_shim.cpp)
 * ========================================================================== */

/*
 * Blobs from 2015 such as libbeautify.so include <iostream> in some TU and therefore reference
 * the *GNU libstdc++* ABI of std::ios_base::Init::Init()/~Init() (namespace std rather than
 * std::__1). Android 14 only has libc++, so nobody provides them (/system/lib/libstdc++.so
 * has degenerated into a 10KB stub).
 *
 * These two functions only initialise iostream globals; the blob does not depend on their
 * behaviour, so empty bodies are enough.
 */
extern "C" void _ZNSt8ios_base4InitC1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitC2Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD2Ev(void) {}

/*
 * Keep a real reference to libc++.so so that libc++ is pulled into the shim's shared group,
 * which lets the blob's other libc++ symbols resolve. Without a real reference --as-needed
 * drops the dependency.
 */
extern "C" void mocha_camera_shim_keepalive(void) {
    static std::ios_base::Init io_init;
    (void)io_init;
}
