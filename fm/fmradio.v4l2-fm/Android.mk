LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# 修复说明（FM，2026-11-16）：
# 此模块是 FM 的 V4L2 底层实现，打开内核的 /dev/radio0 设备
# （Broadcom FM V4L2 驱动，CONFIG_V4L2_FM_DRIVER=y，源码在
#  kernel/xiaomi/mocha/drivers/bluetooth/broadcom/v4l2_fm_driver/），
# 被 libfmjni 通过 dlopen 加载。
#
# 安装位置从 /vendor/lib 改成 /system/lib，原因：
#   libfmjni 是在 FMRadio app 进程里跑的（System.loadLibrary("fmjni")）。
#   app 进程的 classloader-namespace 只能搜索 /system/lib 这类路径；libfmjni
#   用 dlopen 绝对路径打开 /vendor/lib/xxx 会被 linker 以
#       "library ... is not accessible for the namespace"
#   拒绝。两者必须放在同一个 namespace 能看到的目录，这里统一用 /system/lib，
#   与 libfmjni/Android.bp 里的 -DLIBRARY_PATH="/system/lib/" 保持一致。
LOCAL_MODULE := libfmradio.v4l2-fm
LOCAL_MODULE_SUFFIX := .so
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_SRC_FILES := v4l2_fm.c v4l2_ioctl.c
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := false

include $(BUILD_SHARED_LIBRARY)
