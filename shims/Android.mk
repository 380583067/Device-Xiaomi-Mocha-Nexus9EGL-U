LOCAL_PATH:= $(call my-dir)

# libshim_atomic
include $(CLEAR_VARS)
LOCAL_SRC_FILES := atomic.cpp
LOCAL_MODULE := libshim_atomic
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_CFLAGS := -O3 -Wno-unused-variable -Wno-unused-parameter
LOCAL_C_INCLUDES += system/core/libcutils/include

include $(BUILD_SHARED_LIBRARY)

# libs
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stdio_vsnprintf.cpp
LOCAL_C_INCLUDES := \
    bionic/libc \
    bionic/libc/stdio \
    bionic/libc/async_safe/include

LOCAL_SHARED_LIBRARIES := liblog
LOCAL_MODULE := libs
LOCAL_MODULE_TAGS := optional
LOCAL_LDFLAGS_arm += -Wl,--version-script,$(LOCAL_PATH)/stdio_vsnprintf.arm.map

include $(BUILD_SHARED_LIBRARY)

# libshim_zw
include $(CLEAR_VARS)
LOCAL_SRC_FILES := zygote_whitelist.cpp
LOCAL_C_INCLUDES := frameworks/base/core/jni \
                    system/core/base/include
ifneq ($(ZYGOTE_WHITELIST_PATH_EXTRA),)
    LOCAL_CFLAGS += -DPATH_WHITELIST_EXTRA=$(ZYGOTE_WHITELIST_PATH_EXTRA)
endif
LOCAL_SHARED_LIBRARIES := liblog libbase
LOCAL_MODULE := libshim_zw
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

# libnvomxadaptor_shim
# 修复说明：libnvomxadaptor.so（NVIDIA 硬件 OMX 解码 adaptor，Android 5 时代的 blob）
# 引用了旧 ABI 符号 _ZN7android13GraphicBufferC1EP19ANativeWindowBufferb
# （即 GraphicBuffer(ANativeWindowBuffer*, bool) 构造函数），该构造函数在 Android 10+
# 的 libui 中已被移除，导致 libnvomxadaptor.so 加载失败、媒体硬件解码不可用
# （日志：cannot locate symbol "_ZN7android13GraphicBufferC1EP19ANativeWindowBufferb"
#  referenced by "/system/vendor/lib/libnvomxadaptor.so"）。
# 本 shim 用现存的 8 参数构造函数重新实现该旧符号，并补齐 libnvomxadaptor 还引用的
# BufferQueue::createBufferQueue 旧签名与 MediaBuffer::refcount，供 LD_SHIM_LIBS 预加载。
# 参考 Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S 的 shims/libnvomxadaptor_shim.cpp。
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libnvomxadaptor_shim.cpp
LOCAL_SHARED_LIBRARIES := libui libgui libstagefright_foundation
LOCAL_C_INCLUDES := frameworks/native/include frameworks/av/include frameworks/av/media/ndk/include
LOCAL_CFLAGS := -Wno-unused-private-field
LOCAL_MODULE := libnvomxadaptor_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

# libsensors_fastsys_shim
# 修复说明（开机速度）：传感器 HAL blob（libsensors.lsm6db0.so，NVIDIA 原厂）
# 初始化时用 system("rm -rf /sys/bus/iio/devices/...") 清理 iio 节点。在
# Android 14 上 system() = posix_spawn("/system/bin/sh","-c",...) + waitpid，
# 本机（Tegra K1 + 2GB）一次要 1.5~2 秒，而它删的是 sysfs 节点、必然失败。
# sensorservice 因此要 5.68 秒才就绪，进而让 DeviceStateManagerService 同步
# 等待 6 秒（它是 startOtherServices 里最大的单体耗时）。
# 本 shim 短路掉这条已知无效的调用，其它命令照常转交 libc。
# 详见 libsensors_fastsys_shim.cpp 头部注释。
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libsensors_fastsys_shim.cpp
LOCAL_SHARED_LIBRARIES := liblog libdl
LOCAL_MODULE := libsensors_fastsys_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
