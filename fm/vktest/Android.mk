# Vulkan 驱动链路诊断工具（调试用，不打包进 ROM）
#
# 用途：直接加载 libvulkan.so，创建 instance 并枚举物理设备，
#   验证 frameworks/native/vulkan/libvulkan -> vendor/lib/hw/vulkan.tegra.so
#   这条链路是否真的可用。不加入 PRODUCT_PACKAGES。

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := vktest.c
LOCAL_MODULE    := vktest
LOCAL_MODULE_TAGS := optional
LOCAL_SHARED_LIBRARIES := libdl liblog

include $(BUILD_EXECUTABLE)
