LOCAL_PATH := $(call my-dir)

# ============================================================================
# V4L2 camera HAL (Plan B) —— 自研 HAL3 实现
# ----------------------------------------------------------------------------
# 原厂 blob 走 NVIDIA 老式 PCL 通路（/dev/camera.pcl），在 Android 14 上
# dlopen 成功但 I2C 探测全部超时并卡死系统（详见 BoardConfig.mk 说明）。
# 正确路线是内核 V4L2 驱动（CONFIG_VIDEO_TEGRA_VI_V4L2 + IMX179/OV5693/
# AD5823）+ 这里的自研 HAL：
#   MochaCameraHAL.cpp   HAL3 设备、静态元数据、拍照控制
#   CameraPipeline.cpp   /dev/video0 V4L2 mmap 采集 + VCM 自动对焦
#   JpegEncoder.cpp      libjpeg-turbo 把 Bayer 转换后的帧编码成 JPEG
#   isp/*.cpp            NEON demosaic / 色彩转换
# 产物名必须是 camera.<ro.hardware>（= camera.tegra.so），camera provider
# 才会以 passthrough 方式加载它。原 CameraWrapper/Camera3Wrapper 包装方案
# 已从源码树删除（见 git 历史），不要再混回来。
# ============================================================================

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    MochaCameraHAL.cpp \
    CameraPipeline.cpp \
    JpegEncoder.cpp \
    isp/DemosaicNEON.cpp \
    isp/ColorConvNEON.cpp

LOCAL_SHARED_LIBRARIES := \
    libhardware liblog libcamera_metadata libutils libjpeg libdl libc++ libui libcutils

LOCAL_STATIC_LIBRARIES := libbase

LOCAL_C_INCLUDES += \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/isp \
    frameworks/native/include \
    frameworks/native/include/media/openmax \
    frameworks/native/libs/nativebase/include \
    frameworks/native/libs/ui/include \
    system/core/include \
    system/core/base/include \
    system/media/camera/include \
    external/libjpeg-turbo

LOCAL_HEADER_LIBRARIES := libnativebase_headers libcamera_metadata_headers

LOCAL_32_BIT_ONLY := true
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS := optional

BUILD_BROKEN_VNDK_USAGE := true
LOCAL_PRIVATE_PLATFORM_APIS := true
LOCAL_VENDOR_MODULE := false
LOCAL_PROPRIETARY_MODULE := false

include $(BUILD_SHARED_LIBRARY)

# ============================================================================
# 相机 vendor blob（camera.vendor.tegra.so）的 ABI 兼容层
# ----------------------------------------------------------------------------
# 原厂 blob 是 2015 年 Android 5.1 时代编译的，在 Android 14 上 dlopen 会失败：
#   E HAL: dlopen failed: library "libcameraservice.so" not found:
#          needed by /system/lib/hw/camera.vendor.tegra.so in namespace (default)
# 逐符号比对后确认需要补齐 3 个东西，详见各源文件头部注释：
#   1. libcameraservice.so            —— DT_NEEDED 要求文件存在，无符号引用
#   2. libpowerservice_client.so      —— PowerServiceClient 构造 + sendPowerHint
#   3. libmocha_camera_shim.so        —— CameraMetadata / Fence / SensorManager
#                                        / libstdc++ iostream 的 ABI 转发
# 其中 libmocha_camera_shim.so 通过 BoardConfig.mk 的 TARGET_LD_SHIM_LIBS
# 预加载到 camera.vendor.tegra.so，另外两个靠 DT_NEEDED 名字自动加载。
# ============================================================================

# libcameraservice.so —— 空 stub（只为满足 blob 的 DT_NEEDED）
#
# 为什么搞这么绕：
#   1) frameworks/av/services/camera/libcameraservice/Android.bp 里已经有一个
#      cc_library 叫 "libcameraservice"（Soong 模块名全局唯一，不能重名）。
#      好在它是 cameraserver 的 static_libs（静态链接进可执行文件），运行时
#      根本不会去加载 /system/lib/libcameraservice.so，所以造一个同名空库
#      不会劫持任何真实实现。
#   2) 但 build/make/core/shared_library_internal.mk:16 明确禁止给 SHARED_LIBRARY
#      设置 LOCAL_MODULE_STEM，没法"模块名不同、产物名相同"。
#   3) 于是用 SONAME 曲线救国：文件叫 libcameraservice_mocha.so，但 ELF 头里
#      的 SONAME 声明成 libcameraservice.so。bionic 的 linker 在 find_library()
#      里会先查已加载库的 soname 索引（find_loaded_library_by_soname），只要
#      本库在 camera.vendor.tegra.so 之前被加载，blob 的 DT_NEEDED
#      "libcameraservice.so" 就能命中它，而不必真的存在一个叫这个名字的文件。
#      加载时机由 libmocha_camera_shim.so 的 DT_NEEDED 保证（见下面的
#      LOCAL_SHARED_LIBRARIES），而 shim 又由 TARGET_LD_SHIM_LIBS 预加载。
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libcameraservice.cpp
LOCAL_MODULE := libcameraservice_mocha
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# 覆盖 shared_library_internal.mk 自动加的 -Wl,-soname,<模块名>.so
LOCAL_LDFLAGS := -Wl,-soname,libcameraservice.so
include $(BUILD_SHARED_LIBRARY)

# libpowerservice_client.so —— NVIDIA 私有 PowerServiceClient 的最小实现
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libpowerservice_client.cpp
LOCAL_SHARED_LIBRARIES := libutils
LOCAL_C_INCLUDES := system/core/libutils/include
LOCAL_MODULE := libpowerservice_client
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_SHARED_LIBRARY)

# libmocha_camera_shim.so —— CameraMetadata / Fence / SensorManager ABI 转发
include $(CLEAR_VARS)
LOCAL_SRC_FILES := mocha_camera_shim.cpp
# libcamera_client 提供 CameraMetadata 的 const 版本；libui 提供 Fence::wait(int)；
# libsensor 提供带参数的 SensorManager 构造与 createEventQueue。
# 注意：这里**不能**把 libcameraservice_mocha 写进 LOCAL_SHARED_LIBRARIES ——
# ld 会把它的 SONAME（libcameraservice.so）写进本库的 NEEDED 段，而那个名字没有
# 对应文件，反而会让 shim 自己加载失败。改由 constructor 里显式 dlopen 完成。
LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libui \
    libcamera_client \
    libsensor \
    libdl
LOCAL_C_INCLUDES := \
    frameworks/native/libs/sensor/include \
    system/core/libutils/include \
    system/core/libcutils/include
LOCAL_CFLAGS := -Wno-unused-parameter
LOCAL_MODULE := libmocha_camera_shim
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# 必须真实依赖 libc++（mocha_camera_shim_keepalive 里引用了 std::ios_base::Init），
# 否则 --as-needed 会把 libc++ 从 DT_NEEDED 里删掉，blob 的 std:: 符号又会找不到。
#
# -z global 设置 DT_FLAGS_1 的 DF_1_GLOBAL：bionic 只有在库带这个标志时才允许它
# 进入进程的 global group（符号对其它库可见）。手工用 RTLD_GLOBAL dlopen 本库做
# 验证时（camera_shim_test）同样需要它。
LOCAL_LDFLAGS := -Wl,--no-as-needed -Wl,-z,global
include $(BUILD_SHARED_LIBRARY)

# libbeautify.so —— 原厂美颜库的 ABI stub（同名替换）
#
# 为什么要替换：
#   原厂 libbeautify.so 的 DT_NEEDED 里有 libFaceProc.so（3.2MB 人脸识别库，
#   静态链了 OpenCV 2.4），而 libFaceProc.so 带 TEXTREL：
#       readelf -d libFaceProc.so -> 0x00000016 (TEXTREL) 0x0
#   Android 6+ 的 linker 对 API>=23 直接拒绝：
#       dlopen failed: "/system/lib/libFaceProc.so" has text relocations
#   于是 camera.vendor.tegra.so 加载失败，相机设备数为 0。
#
# 为什么不去修 libFaceProc 的 TEXTREL：
#   bionic 在 linker.cpp:3423 里对 has_text_relocations 的库会先
#   phdr_table_unprotect_segments() 把段改成可写再重定位。只要把 DT_TEXTREL
#   条目删掉，这个标志就没了，linker 也就不会 unprotect，重定位会写只读的
#   代码段 —— 直接 SIGSEGV。要么改 linker（全局风险），要么就别用这个库。
#
# 为什么不用 stub 顶掉 libFaceProc 而是顶掉 libbeautify：
#   实测符号交叉引用：
#     camera.vendor.tegra.so  -> libbeautify.so   需要 17 个符号
#     libbeautify.so          -> libFaceProc.so   需要 73 个符号（含一大堆 cv::）
#   显然从 libbeautify 这一侧切断代价小得多。
#
# 功能影响：只影响美颜/人脸检测/水印三件事，它们都走 libbeautify；
#   取景、拍照、3A、闪光灯、HDR、录像完全不经这条路径，不受影响。
# 具体实现见 libbeautify_stub.cpp 头部注释（那里有逐符号的说明）。
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libbeautify_stub.cpp
LOCAL_MODULE := libbeautify
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# 关闭函数级 GC 的裁剪风险：stub 里的符号只被动态链接器按名字查找，
# 编译单元内没有调用者，保留 --gc-sections 之外再靠锚点数组兜底。
LOCAL_LDFLAGS := -Wl,--no-gc-sections
include $(BUILD_SHARED_LIBRARY)

# 相机 blob 加载链路诊断工具（不装进镜像，手工 push 到 /data/local/tmp 使用）
include $(CLEAR_VARS)
LOCAL_SRC_FILES := camera_shim_test.cpp
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_MODULE := camera_shim_test
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_EXECUTABLE)
