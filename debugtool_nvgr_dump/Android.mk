LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := nvgr_dump
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

LOCAL_SRC_FILES := nvgr_dump.cpp

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libhardware \
    libcutils \
    libnativewindow

include $(BUILD_EXECUTABLE)
