/*
 * libmocha_camera_shim.so
 * ============================================================================
 * 小米平板1（mocha / NVIDIA Tegra K1 T124 / TN8）相机 vendor blob 的 ABI 兼容层。
 *
 * 背景
 * ---------------------------------------------------------------------------
 * camera.vendor.tegra.so 是 Android 5.1（2015 年）时代 NVIDIA 编译的二进制 blob，
 * 由 CameraWrapper（camera.tegra.so）通过 hw_get_module_by_class("camera", "vendor")
 * 加载。在 Android 14（LineageOS 21）上，blob 的 345 个未定义符号里，除了 libc /
 * pthread / liblog 这些稳定 ABI 之外，有一批 Android C++ ABI 符号已经变形或消失，
 * 导致 dlopen 阶段就失败（"cannot locate symbol ..."），相机设备数为 0。
 *
 * 逐项实测的缺失符号（2026-11，用 readelf 对 out/ 下全部 1056 个 .so 做符号级比对）
 * ---------------------------------------------------------------------------
 *  1) android::CameraMetadata::getAndLock()
 *     android::CameraMetadata::unlock(const camera_metadata*)
 *     —— 方法在 Android 14 变成 const 成员（mangle 前缀 ZNK 而非 ZN）。
 *        libcamera_client.so 里两个 const 版本都在，只是非 const 版本没了。
 *        → 这里做「非 const 转发到 const」，零成本。
 *
 *  2) android::Fence::wait(unsigned int)
 *     —— Android 14 的签名改成 wait(int timeout)，mangle 由 waitEj 变 waitEi。
 *        → 转发并做 uint32 -> int32 的位转换（TIMEOUT_NEVER = -1 两边一致）。
 *
 *  3) android::Fence::~Fence()
 *     —— Android 14 里写成 `virtual ~Fence() = default;`（Fence.h:151），
 *        编译器只生成 COMDAT 弱符号且 libui 未导出，blob 引用的 _ZN7android5FenceD1Ev
 *        找不到。Fence 的布局两个版本都是 { vptr; int mFenceFd; }（Android 14 是
 *        base::unique_fd，本质就是一个 int），所以这里按同样偏移取 fd 并 close。
 *
 *  4) android::SensorManager::SensorManager()               （无参构造）
 *     android::SensorManager::createEventQueue()            （无参）
 *     android::Singleton<SensorManager>::sInstance
 *     android::Singleton<SensorManager>::sLock
 *     —— Android 5 的 SensorManager 继承 Singleton<TS>，getInstance() 被 blob 内联成
 *        「加锁；若 sInstance 为空则 new SensorManager()；返回 *sInstance」。
 *        Android 14 已改为 getInstanceForPackage(String16, int)，类里也不再是 Singleton。
 *        处理办法：
 *          a) 在 .bss 里按老名字导出 sInstance / sLock 两个变量，让 blob 的内联代码
 *             能正常工作；
 *          b) 在 ELF constructor 里把 sInstance 预置成系统真实单例
 *             （getInstanceForPackage），于是 blob 那段 "if (!sInstance) new ..." 的
 *             分支永远不会走 —— 这一步很关键：blob 是按 Android 5 的 sizeof 去 new 的，
 *             而 Android 14 的 SensorManager 更大，真让它 new 会踩堆。
 *          c) 无参构造给一个空实现（只为满足链接，正常不会被调用）；
 *          d) 无参 createEventQueue() 转发到新签名的三参版本。
 *
 * 不属于本 shim 范围的（已确认无需处理）
 * ---------------------------------------------------------------------------
 *  - NvSFxFloat2Fixed：由 libnvisp_v3.so 导出，而 libnvisp_v3.so 是 blob 直接依赖的
 *    libnvmm_camera_v3.so 的 NEEDED，运行时符号全局可见，能正常解析。
 *  - libcameraservice.so / libpowerservice_client.so：blob 的 DT_NEEDED 里要求这两个
 *    文件必须存在，但前者无任何符号引用（空 stub 即可），后者只需要
 *    PowerServiceClient 的构造 + sendPowerHint。两者分别由同目录下的
 *    stub_libcameraservice.cpp / stub_libpowerservice_client.cpp 生成。
 *  - std::string 的 COW 符号（_ZNSs*）：实测计数为 0，blob 没有踩 GNU libstdc++ 的
 *    std::string ABI。
 *
 * 注意（血泪教训，摘自 libbeautify_stub.cpp / mocha_cpp_shim.cpp）
 * ---------------------------------------------------------------------------
 * 本 shim 通过 TARGET_LD_SHIM_LIBS 预加载，会进入进程的 *shared group*，也就是说
 * 它导出的符号对 cameraserver 里所有库可见。因此这里只定义"系统里确实不存在"的
 * 符号，绝不能重定义 operator new/delete、__throw_* 这类被全进程使用的符号 ——
 * 之前那么干过，直接让 libutils 在 call_constructors 阶段 segfault。
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
 * camera_metadata_t 这里刻意只用不透明指针（void*）代替，不 include
 * <camera/camera_metadata.h>：
 *   - 下面的函数名是**手工写死的 mangled name**（extern "C"），符号只跟
 *     mangled 名字里的类型编码有关，跟本 TU 里声明的 C++ 类型无关；
 *   - 避免把 libcamera_metadata 的头文件依赖拉进这个 shim（该头文件在
 *     system/media/camera/include 下，Make 里还要多一个 include 路径）。
 */

/* ==========================================================================
 * 1) CameraMetadata：非 const -> const 转发
 * ========================================================================== */

// libcamera_client.so 里真实存在的 const 版本。
extern "C" void* _ZNK7android14CameraMetadata10getAndLockEv(const void* thiz);
extern "C" int _ZNK7android14CameraMetadata6unlockEPK15camera_metadata(const void* thiz,
                                                                     const void* md);

// blob 引用的非 const 版本（Android 5 ABI）。
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

// 新签名：status_t Fence::wait(int timeout)
extern "C" int _ZN7android5Fence4waitEi(void* thiz, int timeout);

// 老签名：status_t Fence::wait(unsigned int timeout)
// 位转换即可：老代码传 Fence::TIMEOUT_NEVER 时是 0xFFFFFFFF，解释成 int 就是 -1，
// 而 Android 14 的 TIMEOUT_NEVER 同样是 -1。
extern "C" int _ZN7android5Fence4waitEj(void* thiz, unsigned int timeout) {
    return _ZN7android5Fence4waitEi(thiz, static_cast<int>(timeout));
}

/*
 * Fence 对象布局（两个版本一致）：
 *   offset 0 : vptr
 *   offset 4 : int mFenceFd      (Android 14 是 base::unique_fd，内部就是一个 int)
 * 整体 8 字节。
 *
 * 析构只需要把 fd 关掉。libui 里 `virtual ~Fence() = default;` 未导出符号，
 * 所以这段逻辑由我们自己补。
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
 * blob 内联的 getInstance() 会直接访问这两个老符号：
 *   static Mutex sLock;             -> Singleton<SensorManager>::sLock
 *   static SensorManager* sInstance;-> Singleton<SensorManager>::sInstance
 *
 * sLock 用一块足够大的零初始化内存：bionic 的 PTHREAD_MUTEX_INITIALIZER 就是全零，
 * 所以 pthread_mutex_lock() 对这块内存完全合法，不需要（也无法）显式构造。
 */
extern "C" {
void* _ZN7android9SingletonINS_13SensorManagerEE9sInstanceE = nullptr;
__attribute__((aligned(16))) unsigned char _ZN7android9SingletonINS_13SensorManagerEE5sLockE[128] = {};
}

/*
 * 无参构造：只为满足动态链接器解析。
 * 正常路径下 blob 不会走到这里（sInstance 在 constructor 阶段就已被预置），
 * 而即便走到了，我们也不能在这里 placement-new 真正的 SensorManager ——
 * blob 分配的大小是 Android 5 的 sizeof，新版对象更大，会踩堆。
 */
extern "C" void _ZN7android13SensorManagerC2Ev(void* /*thiz*/) {}
extern "C" void _ZN7android13SensorManagerC1Ev(void* thiz) { _ZN7android13SensorManagerC2Ev(thiz); }

/*
 * 无参 createEventQueue()：返回 sp<SensorEventQueue>，属于非平凡返回类型，
 * 按 AAPCS 走 sret 隐藏参数，实际签名相当于
 *     void f(sp<SensorEventQueue>* ret, SensorManager* this)
 * 这里转发到 Android 14 的三参版本（三个参数都有默认值）。
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
 * 预置 sInstance 为系统真实的 SensorManager 单例。
 *
 * 时机：本库由 TARGET_LD_SHIM_LIBS 在进程启动时预加载，constructor 早于
 * cameraserver 的 main()，此时 binder 已可用（libsensor 的 getInstanceForPackage
 * 内部会自行 assertStateLocked 并重试）。
 *
 * 之所以必须在这里做、不能推迟到首次调用：blob 的内联 getInstance() 逻辑是
 * 「若 sInstance 为空就自己 new」，一旦它先跑，我们就失去了接管的机会，而且它会
 * 按错误的大小分配对象。
 */
__attribute__((constructor)) static void mocha_camera_shim_init() {
    /*
     * 先把 libcameraservice_mocha.so 加载进来。
     *
     * 原因：camera.vendor.tegra.so 的 DT_NEEDED 里写了 "libcameraservice.so"，
     * 但那个文件在 Android 14 里不存在（libcameraservice 是 cameraserver 的
     * static_libs，纯静态链接，不产出共享库）。我们编了一个空库顶上，但
     *   - 模块名不能叫 libcameraservice（与 Soong 里的同名 cc_library 冲突），
     *   - SHARED_LIBRARY 又不允许设置 LOCAL_MODULE_STEM，
     * 所以产物文件名是 libcameraservice_mocha.so，只在 ELF 头里把 SONAME
     * 声明成 libcameraservice.so。
     *
     * bionic 的 linker 在 find_library() 里会先查已加载库的 soname 索引
     * （linker.cpp:1468 find_loaded_library_by_soname），所以只要这里先把
     * libcameraservice_mocha.so 加载进来，后面 blob 的 DT_NEEDED
     * "libcameraservice.so" 就能命中它的 SONAME，不必真的存在同名文件。
     *
     * 注意：不能靠 shim 自己的 DT_NEEDED 来建立这个依赖 —— ld 会把被链接库的
     * SONAME（libcameraservice.so）写进 NEEDED 段，而这个名字反过来又没有对应
     * 文件，会导致 shim 自己加载失败。所以改用显式 dlopen。
     *
     * 另外 BoardConfig.mk 的 TARGET_LD_SHIM_LIBS 里也把
     * libcameraservice_mocha.so 列为 camera.vendor.tegra.so 的第二个 shim，
     * 两条路任意一条生效即可，dlopen 这里是幂等的。
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
 * 4) NVIDIA libphs 的旧 API：NvPHSSetThrottle
 * ========================================================================== */

/*
 * 这一条不是 blob 自己要的，而是它的依赖 libcuda.so 要的 —— 单独检查
 * camera.vendor.tegra.so 是发现不了的，必须把整条依赖树走一遍。
 *
 * 现象：
 *   dlopen("/system/lib/hw/camera.vendor.tegra.so") failed:
 *     cannot locate symbol "NvPHSSetThrottle" referenced by
 *     "/system/vendor/lib/libcuda.so"
 *
 * 原因：
 *   libcuda.so（DT_NEEDED 里有 libphs.so）期望 libphs 提供两个 API：
 *     NvPHSSetThrottle        （老接口）
 *     NvPHSSendThroughputHints（新接口）
 *   而本 ROM 带的 libphs.so 只有后一个：
 *     NvPHSSendThroughputHints / NvPHSCancelThroughputHints
 *     NvVaSendThroughputHints  / NvCancelThroughputHints
 *   NVIDIA 在后期版本里把 SetThrottle 并入了 SendThroughputHints。
 *
 * 处理：
 *   NvPHSSetThrottle 的语义是"给 GPU/EMC 施加一个频率上限"（性能调节提示），
 *   与相机功能本身无关。这里做成 no-op 并返回 0（NvSuccess），最坏后果只是
 *   相机负载高时 GPU 不会被主动降频 —— 不影响取景、拍照、录像。
 *
 * 可见性：本 shim 带 DF_1_GLOBAL（见 Android.mk 的 -Wl,-z,global），且在
 * libcuda.so 之前加载，因此该符号对 libcuda 可见。
 */
extern "C" int NvPHSSetThrottle() {
    return 0;  // NvSuccess
}

/* ==========================================================================
 * 5) NvRmSync 的新 API（本 ROM 的 libnvrm.so 是旧版，缺少这几个）
 * ========================================================================== */

/*
 * 这一组同样不是 blob 直接要的，而是 libnvrm_gpu.so 要的：
 *   camera.vendor.tegra.so -> libcuda.so -> libnvrm_gpu.so
 *
 * 现象：
 *   dlopen failed: cannot locate symbol
 *     "NvRmSyncCalculateSizeForInplaceSyncpoints" referenced by
 *     "/system/vendor/lib/libnvrm_gpu.so"
 *
 * 原因：
 *   这套 blob 里的 libnvrm_gpu.so 比 libnvrm.so 新。旧版 libnvrm.so 只导出：
 *     NvRmSyncCreateFromFd / NvRmSyncCreateFromSyncpoints
 *   而 libnvrm_gpu.so 还在找带 "WithAttr" 和 "CalculateSizeForInplace" 的新变体，
 *   新版 libnvrm.so 才有。全套 blob 里没有任何库导出这 4 个符号，所以只能补。
 *
 * 处理策略（重要）：
 *   这几个 API 属于"把 GPU 的 fence 做成 NvRmSync 句柄"的同步路径，只在
 *   CUDA 加速分支里用（相机侧是 EDR/HDR 合成、人脸加速、JPEG 加速）。
 *   我们没法实现真语义，所以选择**明确失败**而不是假装成功：
 *     - CalculateSizeForInplace* 返回 0（不需要额外空间）；
 *     - Create*WithAttr 先把输出句柄写成 NULL，再返回 NvError_NotImplemented，
 *       这样即使调用方不检查返回码，也不会拿一个野指针去 NvRmSyncClose（那才是
 *       真正会崩的地方）。
 *   只要 CUDA 分支根本没被走到，这些函数一次都不会执行，等于零影响。
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
 * 6) GNU libstdc++ iostream 符号（沿用 mocha_cpp_shim.cpp 的既有修复）
 * ========================================================================== */

/*
 * libbeautify.so 等 2015 年的 blob 在某个 TU 里 include 了 <iostream>，
 * 因而引用 std::ios_base::Init::Init()/~Init() 的 **GNU libstdc++** ABI
 * （namespace std 而非 std::__1）。Android 14 只有 libc++，这些符号没人提供
 * （/system/lib/libstdc++.so 已退化为 10KB 的空壳）。
 *
 * 这两个函数只负责初始化 iostream 的全局对象，blob 并不依赖它们的实际行为，
 * 空实现即可。
 */
extern "C" void _ZNSt8ios_base4InitC1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitC2Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD1Ev(void) {}
extern "C" void _ZNSt8ios_base4InitD2Ev(void) {}

/*
 * 保持对 libc++.so 的真实引用：让 libc++ 被本 shim 拖进进程的 shared group，
 * 这样 blob 里其它 libc++ 符号也能解析。若没有真实引用，--as-needed 会把
 * 依赖丢掉。
 */
extern "C" void mocha_camera_shim_keepalive(void) {
    static std::ios_base::Init io_init;
    (void)io_init;
}
