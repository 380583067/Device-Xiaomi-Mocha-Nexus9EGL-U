LOCAL_PATH := $(call my-dir)

# ============================================================================
# V4L2 camera HAL (plan B) - an in-house HAL3 implementation
#
# The vendor blob uses the old NVIDIA PCL path (/dev/camera.pcl): it dlopens on
# Android 14 but every I2C probe times out and hangs the system (see BoardConfig.mk).
# The right path is the kernel V4L2 driver (CONFIG_VIDEO_TEGRA_VI_V4L2 + IMX179/
# OV5693/AD5823) plus this HAL:
#   MochaCameraHAL.cpp   HAL3 device, static metadata, still capture control
#   CameraPipeline.cpp   /dev/video0 V4L2 mmap capture + VCM autofocus
#   JpegEncoder.cpp      libjpeg-turbo, encodes the converted Bayer frame as JPEG
#   isp/*.cpp            NEON demosaic / colour conversion
# The output must be named camera.<ro.hardware> (= camera.tegra.so) for the camera
# provider to load it in passthrough mode. The old CameraWrapper/Camera3Wrapper
# approach was removed from the tree (see git history); do not mix it back in.
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
# ABI compatibility layer for the camera vendor blob (camera.vendor.tegra.so)
# ----------------------------------------------------------------------------
# The vendor blob was built for Android 5.1 in 2015 and fails to dlopen on
# Android 14:
#   E HAL: dlopen failed: library "libcameraservice.so" not found:
#          needed by /system/lib/hw/camera.vendor.tegra.so in namespace (default)
# A symbol-by-symbol comparison shows three things are needed, each documented in
# the header of its source file:
#   1. libcameraservice.so        - DT_NEEDED requires the file, no symbol use
#   2. libpowerservice_client.so  - PowerServiceClient ctor + sendPowerHint
#   3. libmocha_camera_shim.so    - CameraMetadata/Fence/SensorManager and
#                                   libstdc++ iostream ABI forwarding
# libmocha_camera_shim.so is preloaded into camera.vendor.tegra.so through
# TARGET_LD_SHIM_LIBS in BoardConfig.mk; the other two load by their DT_NEEDED name.
# ============================================================================

# libcameraservice.so - an empty stub, only there to satisfy the blob's DT_NEEDED
#
# Why it has to be this convoluted:
#   1) frameworks/av/services/camera/libcameraservice/Android.bp already defines a
#      cc_library named "libcameraservice" (Soong module names are global, so it
#      cannot be reused). It is in cameraserver's static_libs though (linked into
#      the executable), so nothing loads /system/lib/libcameraservice.so at run
#      time and a same-named empty library hijacks no real implementation.
#   2) But build/make/core/shared_library_internal.mk:16 forbids setting
#      LOCAL_MODULE_STEM on a SHARED_LIBRARY, so "different module name, same
#      output name" is not available.
#   3) So the SONAME does the work: the file is libcameraservice_mocha.so but its
#      ELF SONAME says libcameraservice.so. bionic's find_library() looks up the
#      soname index of loaded libraries first (find_loaded_library_by_soname), so
#      as long as this library is loaded before camera.vendor.tegra.so the blob's
#      DT_NEEDED "libcameraservice.so" resolves to it without a file of that name
#      existing. Load order is guaranteed by libmocha_camera_shim.so's DT_NEEDED
#      (see LOCAL_SHARED_LIBRARIES below), and the shim is preloaded through
#      TARGET_LD_SHIM_LIBS.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libcameraservice.cpp
LOCAL_MODULE := libcameraservice_mocha
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# Override the -Wl,-soname,<module name>.so that shared_library_internal.mk adds
LOCAL_LDFLAGS := -Wl,-soname,libcameraservice.so
include $(BUILD_SHARED_LIBRARY)

# libpowerservice_client.so - a minimal implementation of NVIDIA's private PowerServiceClient
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libpowerservice_client.cpp
LOCAL_SHARED_LIBRARIES := libutils
LOCAL_C_INCLUDES := system/core/libutils/include
LOCAL_MODULE := libpowerservice_client
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_SHARED_LIBRARY)

# libmocha_camera_shim.so - CameraMetadata / Fence / SensorManager ABI forwarding
include $(CLEAR_VARS)
LOCAL_SRC_FILES := mocha_camera_shim.cpp
# libcamera_client provides the const CameraMetadata members, libui provides
# Fence::wait(int) and libsensor provides the parameterised SensorManager
# constructor and createEventQueue.
# Note: libcameraservice_mocha must NOT be listed in LOCAL_SHARED_LIBRARIES. ld
# would write its SONAME (libcameraservice.so) into this library's NEEDED section,
# and that name has no file, so the shim would fail to load itself. The shim
# dlopens it explicitly from its constructor instead.
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
# A real dependency on libc++ is required (mocha_camera_shim_keepalive references
# std::ios_base::Init), otherwise --as-needed drops libc++ from DT_NEEDED and the
# blob's std:: symbols are unresolved again.
#
# -z global sets DF_1_GLOBAL in DT_FLAGS_1: bionic only lets a library into the
# process's global group (symbols visible to other libraries) when it carries that
# flag. The camera_shim_test RTLD_GLOBAL dlopen check needs it as well.
LOCAL_LDFLAGS := -Wl,--no-as-needed -Wl,-z,global
include $(BUILD_SHARED_LIBRARY)

# libbeautify.so - an ABI stub replacing the vendor beautify library under the same name
#
# Why replace it:
#   The vendor libbeautify.so has libFaceProc.so in DT_NEEDED (a 3.2 MB face
#   detection library with OpenCV 2.4 linked in statically) and libFaceProc.so
#   carries TEXTREL:
#       readelf -d libFaceProc.so -> 0x00000016 (TEXTREL) 0x0
#   the Android 6+ linker refuses it for API>=23:
#       dlopen failed: "/system/lib/libFaceProc.so" has text relocations
#   so camera.vendor.tegra.so fails
#   to load and the camera count is 0.
#
# Why not fix the TEXTREL in libFaceProc:
#   bionic calls phdr_table_unprotect_segments() for libraries with
#   has_text_relocations before relocating them (linker.cpp:3423). Removing the
#   DT_TEXTREL entry clears the flag, the linker no longer unprotects, and the
#   relocations then write into read-only code - an immediate SIGSEGV. Either
#   patch the linker (global risk) or stop using that library.
#
# Why stub libbeautify instead of libFaceProc:
#   Measured symbol cross-references:
#     camera.vendor.tegra.so  -> libbeautify.so   17 symbols needed
#     libbeautify.so          -> libFaceProc.so   73 symbols needed (many cv::)
#   Cutting at libbeautify is far cheaper.
#
# Impact: only beauty mode, face detection and watermarking go through
#   libbeautify. Preview, still capture, 3A, flash, HDR and recording never touch
#   it and are unaffected.
# Implementation details, symbol by symbol, are in the header of libbeautify_stub.cpp.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libbeautify_stub.cpp
LOCAL_MODULE := libbeautify
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# Avoid function-level GC stripping: the stub's symbols are only looked up by name
# by the dynamic linker and have no caller in the translation unit, so besides
# --gc-sections an anchor array keeps them alive.
LOCAL_LDFLAGS := -Wl,--no-gc-sections
include $(BUILD_SHARED_LIBRARY)

# Diagnostic tool for the camera blob load chain (not installed, push it to /data/local/tmp by hand)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := camera_shim_test.cpp
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_MODULE := camera_shim_test
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_EXECUTABLE)
