# FM 芯片链路诊断工具（调试用，不打包进 ROM）
#
# 用途：直接对 /dev/radio0 做 V4L2 radio ioctl，验证
#   kernel v4l2_fm_driver -> brcm-uim-sysfs -> BCM4354 这条链路。
# 不加入 PRODUCT_PACKAGES，需要时用 `m fmtest` 单独编译后 push 到设备。

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := fmtest.c
LOCAL_MODULE    := fmtest
LOCAL_MODULE_TAGS := optional

# 静态链接，方便 push 到任何环境跑，不依赖 system 里的 so
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libc

include $(BUILD_EXECUTABLE)
