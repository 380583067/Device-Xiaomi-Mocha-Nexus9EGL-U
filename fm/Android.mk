LOCAL_PATH := $(call my-dir)

# 修复说明（FM，2026-11-16）：
#
# 这个文件原来只做一件事：
#     ifeq ($(BOARD_HAVE_BCM_FM),true)
#     include $(call all-makefiles-under,$(LOCAL_PATH))
#     endif
# 也就是 FM 的三个组件全部挂在 BOARD_HAVE_BCM_FM 开关下。但那个开关在
# vendor/lineage/config/BoardConfigSoong.mk 里的语义是「把 hardware/broadcom/fm
# 加进 PRODUCT_SOONG_NAMESPACES」——那是 LineageOS 上游放博通 FM 源码的标准路径，
# 我们的源码在 device/xiaomi/mocha/fm，所以设 true 反而会去引用一个不存在的目录。
#
# 现在改成：
#   1) libfmjni 已转成 Soong 模块（本目录下的 libfmjni/Android.bp），
#      不再由 Make 编译，所以下面 all-makefiles-under 只会捞到
#      fmradio.v4l2-fm 和 brcm-uim-sysfs 两个 Android.mk；
#   2) 这两个组件无条件编译，由 mocha.mk 的 PRODUCT_PACKAGES 决定是否装进镜像。
#
# 因此 BoardConfig.mk 里 BOARD_HAVE_BCM_FM 保持 false（不去引用上游空路径），
# 改设 BOARD_HAVE_MTK_FM := true 来关掉 FMRadio 自带 MTK 版 libfmjni 的 namespace，
# 避免与我们的博通版重名。
include $(call all-makefiles-under,$(LOCAL_PATH))
