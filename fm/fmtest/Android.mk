# FM chip chain diagnostic tool (debug only, not packaged into the ROM)
#
# Purpose: issue V4L2 radio ioctls straight to /dev/radio0 to verify the chain
#   kernel v4l2_fm_driver -> brcm-uim-sysfs -> BCM4354.
# Not listed in PRODUCT_PACKAGES; build with `m fmtest` and push to the device when needed.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := fmtest.c
LOCAL_MODULE    := fmtest
LOCAL_MODULE_TAGS := optional

# Statically linked so it can be pushed anywhere without depending on system .so files
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libc

include $(BUILD_EXECUTABLE)
