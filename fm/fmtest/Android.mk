# FM chip link diagnostic (debug only, not shipped in the ROM)
#
# Purpose: issue V4L2 radio ioctls on /dev/radio0 directly to verify the whole
#   kernel v4l2_fm_driver -> brcm-uim-sysfs -> BCM4354 chain.
# Not in PRODUCT_PACKAGES; build it with `m fmtest` and push it when needed.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := fmtest.c
LOCAL_MODULE    := fmtest
LOCAL_MODULE_TAGS := optional

# Static, so it can be pushed anywhere and run without the system's shared objects
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libc

include $(BUILD_EXECUTABLE)
