LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
    CameraWrapper.cpp \
    Camera3Wrapper.cpp

LOCAL_STATIC_LIBRARIES := libbase libarect
LOCAL_SHARED_LIBRARIES := \
    libhardware liblog  libcamera_client libutils libgui libhidltransport libcamera_metadata libsensor android.hidl.token@1.0-utils android.hardware.graphics.bufferqueue@1.0

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
