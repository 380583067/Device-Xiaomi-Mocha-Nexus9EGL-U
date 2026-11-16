LOCAL_PATH := $(call my-dir)

# FIX NOTE (FM, 2026-11-16):
#
# This file used to do one thing:
#     ifeq ($(BOARD_HAVE_BCM_FM),true)
#     include $(call all-makefiles-under,$(LOCAL_PATH))
#     endif
#   all three FM components under the BOARD_HAVE_BCM_FM switch. But that switch means
#   "add hardware/broadcom/fm to PRODUCT_SOONG_NAMESPACES" in
#   vendor/lineage/config/BoardConfigSoong.mk - the upstream location of the Broadcom FM
#   sources. Ours live in device/xiaomi/mocha/fm, so setting it true would reference a missing directory.
#
# Now it works as follows:
#   1) libfmjni has been converted to a Soong module (libfmjni/Android.bp in this directory),
#      so it is no longer built by Make and all-makefiles-under below only picks up
#      fmradio.v4l2-fm and brcm-uim-sysfs;
#   2) those two components build unconditionally; PRODUCT_PACKAGES in mocha.mk decides
#
#      whether they are installed. Therefore BoardConfig.mk keeps BOARD_HAVE_BCM_FM false
# path) and sets BOARD_HAVE_MTK_FM := true to disable the namespace of FMRadio's bundled
# MTK libfmjni, avoiding a name clash with our Broadcom build.
include $(call all-makefiles-under,$(LOCAL_PATH))
