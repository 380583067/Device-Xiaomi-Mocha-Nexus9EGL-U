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
# Fix: libnvomxadaptor.so (NVIDIA's hardware OMX decode adaptor, an Android 5 era
# blob) references the old ABI symbol
# _ZN7android13GraphicBufferC1EP19ANativeWindowBufferb (the
# GraphicBuffer(ANativeWindowBuffer*, bool) constructor), which libui dropped in
# Android 10+, so libnvomxadaptor.so fails to load and hardware video decoding is
# unavailable (log: cannot locate symbol "...").
# This shim reimplements the old symbol on top of the surviving 8-argument
# constructor, plus the old BufferQueue::createBufferQueue signature and
# MediaBuffer::refcount that libnvomxadaptor also references, for LD_SHIM_LIBS to
# preload. Same as Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S
# shims/libnvomxadaptor_shim.cpp.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libnvomxadaptor_shim.cpp
LOCAL_SHARED_LIBRARIES := libui libgui libstagefright_foundation
LOCAL_C_INCLUDES := frameworks/native/include frameworks/av/include frameworks/av/media/ndk/include
LOCAL_CFLAGS := -Wno-unused-private-field
LOCAL_MODULE := libnvomxadaptor_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

# libsensors_fastsys_shim
# Fix (boot time): the sensor HAL blob (libsensors.lsm6db0.so, NVIDIA vendor) runs
# system("rm -rf /sys/bus/iio/devices/...") during init to clean up iio nodes. On
# Android 14 system() is posix_spawn("/system/bin/sh","-c",...) plus waitpid, which
# costs 1.5-2 s here (Tegra K1 + 2 GB), and it deletes sysfs nodes, so it always
# fails. sensorservice therefore took 5.68 s to be ready and DeviceStateManagerService
# waited 6 s for it (the largest single cost in startOtherServices).
# This shim short-circuits that one known-useless call and forwards everything else
# to libc. See the header of libsensors_fastsys_shim.cpp.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libsensors_fastsys_shim.cpp
LOCAL_SHARED_LIBRARIES := liblog libdl
LOCAL_MODULE := libsensors_fastsys_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
