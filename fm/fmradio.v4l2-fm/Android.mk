LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# FIX NOTE (FM, 2026-11-16):
# This module is the V4L2 backend of FM: it opens the kernel's /dev/radio0 device
# (Broadcom FM V4L2 driver, CONFIG_V4L2_FM_DRIVER=y, sources under
#  kernel/xiaomi/mocha/drivers/bluetooth/broadcom/v4l2_fm_driver/），
# and is loaded by libfmjni via dlopen.
#
# Install path changed from /vendor/lib to /system/lib because:
#   libfmjni runs inside the FMRadio app process (System.loadLibrary("fmjni")).
#   The app process classloader-namespace only searches paths such as /system/lib, so a
#   dlopen of an absolute /vendor/lib/xxx path by libfmjni is rejected by the linker with
#       "library ... is not accessible for the namespace"
#   Both must live in a directory the same namespace can see, hence /system/lib here, kept
#   consistent with -DLIBRARY_PATH="/system/lib/" in libfmjni/Android.bp.
LOCAL_MODULE := libfmradio.v4l2-fm
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_SRC_FILES := v4l2_fm.c v4l2_ioctl.c
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := false

include $(BUILD_SHARED_LIBRARY)
