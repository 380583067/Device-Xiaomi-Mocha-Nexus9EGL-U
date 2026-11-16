# 低内存链路诊断工具（调试用，不打包进 ROM）
#
# 分配并实际触碰指定大小的匿名内存，保持一段时间后释放，
# 用于观察 zram 换出、kswapd 回收、lmkd vmpressure 是否生效。

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := memstress.c
LOCAL_MODULE    := memstress
LOCAL_MODULE_TAGS := optional
LOCAL_FORCE_STATIC_EXECUTABLE := true
LOCAL_STATIC_LIBRARIES := libc

include $(BUILD_EXECUTABLE)
