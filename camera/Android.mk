LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    CameraWrapper.cpp \
    Camera3Wrapper.cpp

LOCAL_STATIC_LIBRARIES := libbase libarect
LOCAL_SHARED_LIBRARIES := \
    libhardware liblog  libcamera_client libutils libgui libhidltransport libcamera_metadata libsensor android.hidl.token@1.0-utils android.hardware.graphics.bufferqueue@1.0 libdl

LOCAL_C_INCLUDES += \
    frameworks/native/include \
    frameworks/native/include/media/openmax \
    frameworks/native/libs/nativebase/include \
    system/core/include \
    system/core/base/include \
    system/media/camera/include \

LOCAL_HEADER_LIBRARIES := libnativebase_headers

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
# The vendor blob was compiled in the Android 5.1 era (2015) and fails to dlopen on Android 14:
#   E HAL: dlopen failed: library "libcameraservice.so" not found:
#          needed by /system/lib/hw/camera.vendor.tegra.so in namespace (default)
# Symbol-by-symbol comparison shows three things must be supplied; see each source header:
#   1. libcameraservice.so            -- DT_NEEDED requires the file, no symbol references
#   2. libpowerservice_client.so      -- PowerServiceClient constructor + sendPowerHint
#   3. libmocha_camera_shim.so        —— CameraMetadata / Fence / SensorManager
#                                        / ABI forwarding for libstdc++ iostream
# libmocha_camera_shim.so is preloaded into camera.vendor.tegra.so via
# TARGET_LD_SHIM_LIBS in BoardConfig.mk; the other two load by DT_NEEDED name.
# ============================================================================

# libcameraservice.so -- empty stub (only to satisfy the blob's DT_NEEDED)
#
# Why this indirection is needed:
#   1) frameworks/av/services/camera/libcameraservice/Android.bp already defines a
#      cc_library named "libcameraservice" (Soong module names are globally unique).
#      Fortunately it is a static_libs of cameraserver (statically linked into the
#      executable), so /system/lib/libcameraservice.so is never loaded at runtime and
#      an empty library with the same SONAME hijacks no real implementation.
#   2) build/make/core/shared_library_internal.mk:16 forbids setting LOCAL_MODULE_STEM
#      on a SHARED_LIBRARY, so "different module name, same artifact name" is impossible.
#   3) Hence the SONAME trick: the file is libcameraservice_mocha.so but its ELF header
#      declares SONAME libcameraservice.so. bionic's linker looks up the soname index of
#      already-loaded libraries first in find_library() (find_loaded_library_by_soname), so
#      as long as this library is loaded before camera.vendor.tegra.so, the blob's
#      DT_NEEDED "libcameraservice.so" resolves to it without a real file of that name.
#      Load order is guaranteed by libmocha_camera_shim.so's DT_NEEDED (see
#      LOCAL_SHARED_LIBRARIES below), and the shim is preloaded by TARGET_LD_SHIM_LIBS.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libcameraservice.cpp
LOCAL_MODULE := libcameraservice_mocha
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# Override the -Wl,-soname,<module>.so that shared_library_internal.mk adds automatically
LOCAL_LDFLAGS := -Wl,-soname,libcameraservice.so
include $(BUILD_SHARED_LIBRARY)

# libpowerservice_client.so -- minimal implementation of NVIDIA's private PowerServiceClient
include $(CLEAR_VARS)
LOCAL_SRC_FILES := stub_libpowerservice_client.cpp
LOCAL_SHARED_LIBRARIES := libutils
LOCAL_C_INCLUDES := system/core/libutils/include
LOCAL_MODULE := libpowerservice_client
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_SHARED_LIBRARY)

# libmocha_camera_shim.so -- CameraMetadata / Fence / SensorManager ABI forwarding
include $(CLEAR_VARS)
LOCAL_SRC_FILES := mocha_camera_shim.cpp
# libcamera_client provides the const CameraMetadata methods; libui provides Fence::wait(int);
# libsensor provides the parameterised SensorManager constructor and createEventQueue.
# Note: libcameraservice_mocha must NOT be listed in LOCAL_SHARED_LIBRARIES here -
# ld would write its SONAME (libcameraservice.so) into this library's NEEDED section, and
# since no file has that name the shim itself would fail to load. An explicit dlopen in the
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
# A real dependency on libc++ is required (mocha_camera_shim_keepalive references std::ios_base::Init),
# otherwise --as-needed drops libc++ from DT_NEEDED and the blob's std:: symbols break again.
#
# -z global sets DF_1_GLOBAL in DT_FLAGS_1: bionic only lets a library join the process
# global group (symbols visible to other libraries) when this flag is present. It is also
# needed when manually dlopen'ing this library with RTLD_GLOBAL for verification (camera_shim_test).
LOCAL_LDFLAGS := -Wl,--no-as-needed -Wl,-z,global
include $(BUILD_SHARED_LIBRARY)

# libbeautify.so -- ABI stub replacing the vendor beautify library under the same name
#
# Why replace it:
#   The vendor libbeautify.so has libFaceProc.so in DT_NEEDED (a 3.2MB face detection
#   library with OpenCV 2.4 statically linked), and libFaceProc.so carries TEXTREL:
#       readelf -d libFaceProc.so -> 0x00000016 (TEXTREL) 0x0
#   The Android 6+ linker rejects it outright for API>=23:
#       dlopen failed: "/system/lib/libFaceProc.so" has text relocations
#   As a result camera.vendor.tegra.so fails to load and no camera devices exist.
#
# Why not fix the TEXTREL in libFaceProc:
#   bionic at linker.cpp:3423 calls phdr_table_unprotect_segments() for libraries with
#   has_text_relocations, making segments writable before relocating. Removing the
#   DT_TEXTREL entry clears the flag, so the linker no longer unprotects and relocations
#   write into read-only code -> instant SIGSEGV. Either patch the linker (global risk) or
#
# Why a stub replaces libbeautify rather than libFaceProc:
#   Measured cross-references:
#     camera.vendor.tegra.so  -> libbeautify.so   needs 17 symbols
#     libbeautify.so          -> libFaceProc.so   needs 73 symbols (many cv::)
#   Cutting at the libbeautify side is clearly the cheaper option.
#
# Functional impact: only beauty mode, face detection and watermark are affected, all of
#   which go through libbeautify; preview, photo capture, 3A, flash, HDR and video never
#   touch this path. See libbeautify_stub.cpp for the per-symbol implementation notes.
include $(CLEAR_VARS)
LOCAL_SRC_FILES := libbeautify_stub.cpp
LOCAL_MODULE := libbeautify
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
# Guard against function-level GC: the stub symbols are only looked up by name by the
# dynamic linker and have no in-TU caller, so keep an anchor array in addition to --gc-sections.
LOCAL_LDFLAGS := -Wl,--no-gc-sections
include $(BUILD_SHARED_LIBRARY)

# Diagnostic tool for the camera blob load path (not installed, push to /data/local/tmp manually)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := camera_shim_test.cpp
LOCAL_SHARED_LIBRARIES := libdl liblog
LOCAL_MODULE := camera_shim_test
LOCAL_MODULE_TAGS := optional
LOCAL_32_BIT_ONLY := true
LOCAL_PROPRIETARY_MODULE := false
include $(BUILD_EXECUTABLE)
