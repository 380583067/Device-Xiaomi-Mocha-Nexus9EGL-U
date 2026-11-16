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
#
# FIX: libnvomxadaptor.so is the NVIDIA hardware OMX decoder adaptor taken from
# an Android 5 era blob. It imports an old-ABI symbol:
#
#     _ZN7android13GraphicBufferC1EP19ANativeWindowBufferb
#
# i.e. GraphicBuffer(ANativeWindowBuffer*, bool). That constructor was removed
# from libui in Android 10+, so libnvomxadaptor.so fails to load and hardware
# media decoding is unavailable. The log shows:
#
#     cannot locate symbol "_ZN7android13GraphicBufferC1EP19ANativeWindowBufferb"
#      referenced by "/system/vendor/lib/libnvomxadaptor.so"
#
# This shim re-implements the legacy symbol on top of the 8-argument constructor
# that still exists, and additionally provides the legacy
# BufferQueue::createBufferQueue signature and MediaBuffer::refcount that
# libnvomxadaptor also references, so the blob can be preloaded via
# TARGET_LD_SHIM_LIBS.
#
# Reference: shims/libnvomxadaptor_shim.cpp from
# Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libnvomxadaptor_shim.cpp
LOCAL_SHARED_LIBRARIES := libui libgui libstagefright_foundation
LOCAL_C_INCLUDES := frameworks/native/include frameworks/av/include frameworks/av/media/ndk/include
LOCAL_CFLAGS := -Wno-unused-private-field
LOCAL_MODULE := libnvomxadaptor_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

# libsensors_fastsys_shim
#
# FIX (boot speed): the sensor HAL blob (libsensors.lsm6db0.so, NVIDIA
# proprietary) calls system("rm -rf /sys/bus/iio/devices/...") while
# initialising, in order to clean up iio nodes. On Android 14 system() expands
# to posix_spawn("/system/bin/sh", "-c", ...) + waitpid, which costs 1.5-2 s per
# call on this device (Tegra K1 + 2 GB), and what it deletes are sysfs nodes, so
# the operation is guaranteed to fail anyway.
#
# As a result sensorservice needed 5.68 s to become ready, which in turn made
# DeviceStateManagerService wait 6 s synchronously - the single largest item in
# startOtherServices.
#
# This shim short-circuits that known-useless call; every other command is
# forwarded to libc unchanged. See the header comment in
# libsensors_fastsys_shim.cpp for the full analysis.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libsensors_fastsys_shim.cpp
LOCAL_SHARED_LIBRARIES := liblog libdl
LOCAL_MODULE := libsensors_fastsys_shim
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)
