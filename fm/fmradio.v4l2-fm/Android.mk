LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# FM fix (2026-11-16):
# This module is the V4L2 backend of FM: it opens the kernel's /dev/radio0
# (Broadcom FM V4L2 driver, CONFIG_V4L2_FM_DRIVER=y, sources in
#  kernel/xiaomi/mocha/drivers/bluetooth/broadcom/v4l2_fm_driver/) and is
# dlopen'ed by libfmjni.
#
# Installed to /system/lib instead of /vendor/lib because:
#   libfmjni runs inside the FMRadio app process (System.loadLibrary("fmjni")).
#   The app process's classloader namespace only searches paths such as
#   /system/lib; a dlopen of an absolute /vendor/lib/xxx path is refused by the
#   linker with
#       "library ... is not accessible for the namespace"
#   Both must sit in a directory the same namespace can see, so /system/lib is
#   used here and matches -DLIBRARY_PATH="/system/lib/" in libfmjni/Android.bp.
LOCAL_MODULE := libfmradio.v4l2-fm
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_SRC_FILES := v4l2_fm.c v4l2_ioctl.c
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := false

include $(BUILD_SHARED_LIBRARY)
