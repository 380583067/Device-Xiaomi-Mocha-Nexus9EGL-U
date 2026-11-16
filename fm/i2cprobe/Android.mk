LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := i2cprobe
LOCAL_SRC_FILES := i2cprobe.c
LOCAL_MODULE_TAGS := optional
include $(BUILD_EXECUTABLE)
