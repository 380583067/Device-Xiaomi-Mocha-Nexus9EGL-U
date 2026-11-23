/*
 * libmocha_camera_shim.so
 * ============================================================================
 * ABI compatibility layer for the Mi Pad 1 (mocha / NVIDIA Tegra K1 T124 / TN8)
 * camera vendor blob.
 *
 * Background
 * ---------------------------------------------------------------------------
 * camera.vendor.tegra.so is an NVIDIA binary blob built for Android 5.1 (2015),
 * loaded by CameraWrapper (camera.tegra.so) through
 * hw_get_module_by_class("camera", "vendor"). On Android 14 (LineageOS 21), of the
 * blob's 345 undefined symbols the stable libc / pthread / liblog ABI is fine, but
 * a set of Android C++ ABI symbols changed or vanished, so dlopen itself fails
 * ("cannot locate symbol ...") and the camera count is 0.
 *
 * Missing symbols, measured one by one (2026-11, readelf over all 1056 .so in out/)
 * ---------------------------------------------------------------------------
 *  1) android::CameraMetadata::getAndLock()
 *     android::CameraMetadata::unlock(const camera_metadata*)
 *     - these became const members on Android 14 (mangled with ZNK, not ZN).
 *       libcamera_client.so has both const versions, only the non-const one is gone.
 *       -> forward non-const to const here, zero cost.
 *
 *  2) android::Fence::wait(unsigned int)
 *     - Android 14 changed the signature to wait(int timeout), so the mangling moves
 *       from waitEj to waitEi.
 *       -> forward with a uint32 -> int32 bit reinterpretation (TIMEOUT_NEVER = -1 on
 *          both sides).
 *
 *  3) android::Fence::~Fence()
 *     - Android 14 declares it `virtual ~Fence() = default;` (Fence.h:151), so the
 *       compiler only emits a COMDAT weak symbol that libui does not export, and the
 *       _ZN7android5FenceD1Ev the blob references is missing. Both versions lay Fence
 *       out as { vptr; int mFenceFd; } (Android 14 uses base::unique_fd, which is just
 *       an int), so this shim reads the fd at the same offset and closes it.
 *
 *  4) android::SensorManager::SensorManager()               (no-argument ctor)
 *     android::SensorManager::createEventQueue()            (no-argument)
 *     android::Singleton<SensorManager>::sInstance
 *     android::Singleton<SensorManager>::sLock
 *     - Android 5's SensorManager inherits Singleton<TS> and the blob inlined
 *       getInstance() into "lock; if sInstance is null then new SensorManager();
 *       return *sInstance". Android 14 switched to getInstanceForPackage(String16,
 *       int) and the class is no longer a Singleton. Handling:
 *          a) export sInstance / sLock from .bss under the old names so the blob's
 *             inlined code works;
 *          b) preset sInstance in the ELF constructor to the real system singleton
 *             (getInstanceForPackage), so the blob's "if (!sInstance) new ..." branch
 *             never runs. This matters: the blob news it with the Android 5 sizeof,
 *             while Android 14's SensorManager is larger, so a real new would corrupt
 *             the heap.
 *          c) provide an empty no-argument constructor (link satisfaction only, it is
 *             never called in practice);
 *          d) forward the no-argument createEventQueue() to the new three-argument
 *             version.
 *
 * Out of scope for this shim (verified as needing nothing)
 * ---------------------------------------------------------------------------
 *  - NvSFxFloat2Fixed: exported by libnvisp_v3.so, which is a NEEDED entry of
 *    libnvmm_camera_v3.so (a direct dependency of the blob), so the symbol is
 *    globally visible at run time and resolves normally.
 *  - libcameraservice.so / libpowerservice_client.so: the blob's DT_NEEDED requires
 *    both files to exist, but nothing references a symbol in the first (an empty stub
 *    is enough) and the second only needs PowerServiceClient's constructor plus
 *    sendPowerHint. Both are built from stub_libcameraservice.cpp and
 *    stub_libpowerservice_client.cpp in this directory.
 *  - std::string COW symbols (_ZNSs*): measured count is 0, the blob does not rely on
 *    the GNU libstdc++ std::string ABI.
 *
 * Warning (hard-won, from libbeautify_stub.cpp / mocha_cpp_shim.cpp;
 * mocha_cpp_shim.cpp was deleted when the camera approach changed, see git history)
 * ---------------------------------------------------------------------------
 * This shim is preloaded through TARGET_LD_SHIM_LIBS and joins the process's
 * *shared group*, so every symbol it exports is visible to all libraries inside
 * cameraserver. Only define symbols that really do not exist in the system; never
 * redefine operator new/delete or __throw_* and friends, which the whole process
 * uses - doing that once made libutils segfault during call_constructors.
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
 * camera_metadata_t is deliberately an opaque pointer (void*) here, with no
 * <camera/camera_metadata.h> include:
 *   - the function names below are **hand-written mangled names** (extern "C"), and
 *     the symbol only depends on the type encoding inside the mangled name, not on
 *     the C++ types declared in this translation unit;
 *   - it keeps the libcamera_metadata header dependency out of this shim (that
 *     header lives under system/media/camera/include and Make would need another
 *     include path).
 */

/* ==========================================================================
 * 1) CameraMetadata: forward non-const to const
 * ========================================================================== */

// The const versions that really exist in libcamera_client.so.
extern "C" void* _ZNK7android14CameraMetadata10getAndLockEv(const void* thiz);
extern "C" int _ZNK7android14CameraMetadata6unlockEPK15camera_metadata(const void* thiz,
                                                                     const void* md);

// The non-const versions the blob references (Android 5 ABI).
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
// A bit reinterpretation is enough: the old code passes 0xFFFFFFFF for
// Fence::TIMEOUT_NEVER, which reads as -1 as an int, and Android 14's
// TIMEOUT_NEVER is -1 as well.
extern "C" int _ZN7android5Fence4waitEj(void* thiz, unsigned int timeout) {
    return _ZN7android5Fence4waitEi(thiz, static_cast<int>(timeout));
}

/*
 * Fence object layout (identical in both versions):
 *   offset 0 : vptr
 *   offset 4 : int mFenceFd      (Android 14 uses base::unique_fd, an int inside)
 * 8 bytes in total.
 *
 * The destructor only has to close the fd. libui does not export a symbol for
 * `virtual ~Fence() = default;`, so this shim supplies that logic itself.
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
 * The blob's inlined getInstance() accesses these two old symbols directly:
 *   static Mutex sLock;             -> Singleton<SensorManager>::sLock
 *   static SensorManager* sInstance;-> Singleton<SensorManager>::sInstance
 *
 * sLock gets a block of zero-initialised memory that is large enough: bionic's
 * PTHREAD_MUTEX_INITIALIZER is all zeroes, so pthread_mutex_lock() on this memory
 * is perfectly valid and no explicit construction is needed (or possible).
 */
extern "C" {
void* _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = nullptr;
__attribute__((aligned(16))) unsigned char _ZN7android9SingletonINS_13SensorManagerEE5sLockE[128] = {};
}

/*
 * The no-argument constructor exists only to satisfy the dynamic linker.
 * The blob never reaches it on the normal path (sInstance is preset in the
 * constructor), and even if it did we must not placement-new a real SensorManager
 * here: the blob allocates the Android 5 sizeof while the new object is larger, so
 * that would corrupt the heap.
 */
extern "C" void _ZN7android13SensorManagerC2Ev(void* /*thiz*/) {}
extern "C" void _ZN7android13SensorManagerC1Ev(void* thiz) { _ZN7android13SensorManagerC2Ev(thiz); }

/*
 * The no-argument createEventQueue() returns sp<SensorEventQueue>, a non-trivial
 * return type, so AAPCS passes a hidden sret argument and the real signature is
 *     void f(sp<SensorEventQueue>* ret, SensorManager* this)
 * It forwards to Android 14's three-argument version (all three have defaults).
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
 * Timing: TARGET_LD_SHIM_LIBS preloads this library at process start, so this
 * constructor runs before cameraserver's main() and binder is already available
 * (libsensor's getInstanceForPackage does its own assertStateLocked and retries).
 *
 * It has to happen here and not on first call: the blob's inlined getInstance()
 * is "if sInstance is null, new one", so once it runs first we have lost the chance
 * to take over - and it would allocate the object with the wrong size.
 */
__attribute__((constructor)) static void mocha_camera_shim_init() {
    /*
     * Load libcameraservice_mocha.so first.
     *
     * Why: camera.vendor.tegra.so has "libcameraservice.so" in DT_NEEDED, but that
     * file does not exist on Android 14 (libcameraservice is in cameraserver's
     * static_libs, linked statically, no shared library is produced). We build an
     * empty one to fill the gap, except that
     *   - the module cannot be named libcameraservice (it clashes with the same-named
     *     cc_library in Soong), and
     *   - SHARED_LIBRARY does not allow LOCAL_MODULE_STEM,
     * so the output file is libcameraservice_mocha.so with its ELF SONAME declared as
     * libcameraservice.so.
     *
     * bionic's find_library() consults the soname index of loaded libraries first
     * (linker.cpp:1468 find_loaded_library_by_soname), so loading
     * libcameraservice_mocha.so here means the blob's DT_NEEDED
     * "libcameraservice.so" hits that SONAME and no file of that name has to exist.
     *
     * Note: the dependency cannot come from the shim's own DT_NEEDED - ld would
     * write the linked library's SONAME (libcameraservice.so) into NEEDED, and that
     * name has no file, so the shim would fail to load itself. Hence the explicit
     * dlopen.
     *
     * BoardConfig.mk also lists libcameraservice_mocha.so as the second shim of
     * camera.vendor.tegra.so, so either path works and the dlopen here is idempotent.
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
 * 4) The old NVIDIA libphs API: NvPHSSetThrottle
 * ========================================================================== */

/*
 * The blob does not ask for this one, its dependency libcuda.so does - checking
 * camera.vendor.tegra.so alone would never find it, the whole dependency tree has
 * to be walked.
 *
 * Symptom:
 *   dlopen("/system/lib/hw/camera.vendor.tegra.so") failed:
 *     cannot locate symbol "NvPHSSetThrottle" referenced by
 *     "/system/vendor/lib/libcuda.so"
 *
 * Cause:
 *   libcuda.so (with libphs.so in DT_NEEDED) expects two APIs from libphs:
 *     NvPHSSetThrottle         (old interface)
 *     NvPHSSendThroughputHints (new interface)
 *   while this ROM's libphs.so only has the second one:
 *     NvPHSSendThroughputHints / NvPHSCancelThroughputHints
 *     NvVaSendThroughputHints  / NvCancelThroughputHints
 *   NVIDIA merged SetThrottle into SendThroughputHints in a later release.
 *
 * Handling:
 *   NvPHSSetThrottle means "put a frequency cap on GPU/EMC" (a performance hint) and
 *   has nothing to do with camera functionality. It is a no-op returning 0
 *   (NvSuccess); the worst case is that the GPU is not downclocked under camera load
 *   - preview, stills and recording are unaffected.
 *
 * Visibility: this shim carries DF_1_GLOBAL (see -Wl,-z,global in Android.mk) and
 * loads before libcuda.so, so the symbol is visible to libcuda.
 */
extern "C" int NvPHSSetThrottle() {
    return 0;  // NvSuccess
}

/* ==========================================================================
 * 5) The new NvRmSync API (this ROM's libnvrm.so is older and lacks them)
 * ========================================================================== */

/*
 * These are not requested by the blob either, but by libnvrm_gpu.so:
 *   camera.vendor.tegra.so -> libcuda.so -> libnvrm_gpu.so
 *
 * Symptom:
 *   dlopen failed: cannot locate symbol
 *     "NvRmSyncCalculateSizeForInplaceSyncpoints" referenced by
 *     "/system/vendor/lib/libnvrm_gpu.so"
 *
 * Cause:
 *   libnvrm_gpu.so in this blob set is newer than libnvrm.so. The old libnvrm.so only
 *   exports:
 *     NvRmSyncCreateFromFd / NvRmSyncCreateFromSyncpoints
 *   while libnvrm_gpu.so looks for the "WithAttr" and "CalculateSizeForInplace"
 *   variants that only the new libnvrm.so has. No library in the whole set exports
 *   these four symbols, so they have to be supplied.
 *
 * Strategy (important):
 *   These APIs belong to the synchronisation path that turns a GPU fence into an
 *   NvRmSync handle and are only used by CUDA-accelerated branches (EDR/HDR
 *   composition, face acceleration, JPEG acceleration on the camera side). The real
 *   semantics cannot be implemented, so the choice is to **fail explicitly** instead
 *   of pretending:
 *     - CalculateSizeForInplace* returns 0 (no extra space needed);
 *     - Create*WithAttr writes NULL to the output handle first and then returns
 *       NvError_NotImplemented, so a caller that ignores the return code still
 *       cannot pass a wild pointer to NvRmSyncClose (which is where it would really
 *       crash).
 *   As long as the CUDA branch is never taken, none of this runs at all.
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
 * 6) GNU libstdc++ iostream symbols (carried over from mocha_cpp_shim.cpp)
 * ========================================================================== */

/*
 * 2015-era blobs such as libbeautify.so include <iostream> in one translation unit
 * and therefore reference the **GNU libstdc++** ABI of
 * std::ios_base::Init::Init()/~Init() (namespace std, not std::__1). Android 14
 * only has libc++, so nothing provides those symbols (/system/lib/libstdc++.so has
 * degenerated into a 10 KB shell).
 *
 * Both functions only initialise iostream globals and the blob does not depend on
 * their actual behaviour, so empty implementations are enough.
 */
extern "C" void _ZNSt8ios_base4InitC1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitC2Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD2Ev(void) {}

/*
 * Keep a real reference to libc++.so so libc++ is pulled into the process's shared
 * group by this shim and other libc++ symbols in the blob resolve as well. Without a
 * real reference --as-needed would drop the dependency.
 */
extern "C" void mocha_camera_shim_keepalive(void) {
    static std::ios_base::Init io_init;
    (void)io_init;
}
