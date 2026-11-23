LOCAL_PATH := $(call my-dir)

# FM fix (2026-11-16):
#
# This file used to do exactly one thing: build all three FM components, which
# meant they all hung off BOARD_HAVE_BCM_FM. In
# vendor/lineage/config/BoardConfigSoong.mk that switch means "add
# hardware/broadcom/fm to PRODUCT_SOONG_NAMESPACES" - the upstream LineageOS
# location for the Broadcom FM sources. Ours live in device/xiaomi/mocha/fm, so
# setting it true only referenced a directory that does not exist.
#
# Now:
#   1) libfmjni is a Soong module (libfmjni/Android.bp here) and is no longer
#      built by Make, so all-makefiles-under below only picks up
#      fmradio.v4l2-fm and brcm-uim-sysfs;
#   2) those two build unconditionally; whether they land in the image is decided
#      by PRODUCT_PACKAGES in mocha.mk.
#
# So BOARD_HAVE_BCM_FM stays false (no reference to an empty upstream path) and
# BOARD_HAVE_MTK_FM := true turns off the namespace of FMRadio's own MTK
# libfmjni, avoiding the name clash with our Broadcom build.
include $(call all-makefiles-under,$(LOCAL_PATH))
