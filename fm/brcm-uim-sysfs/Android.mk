LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# UIM Application
#
# FIX NOTE (FM, 2026-11-16):
# brcm-uim-sysfs switches the Broadcom combo chip (BCM4354) from Bluetooth HCI mode
# to UIM/FM mode before FM is opened: through sysfs nodes exposed by the bcm_ldisc
# driver (install / vendor_params / snoop_enable ...) it downloads patchram parameters
# and controls chip power sequencing; only then can libfmradio.v4l2-fm reach /dev/radio0.
#
# Install path changed from /vendor/bin to /system/bin:
#   it reads /system/etc/bluetooth/bt_vendor.conf (system_file); installing to /vendor/bin
#   would subject it to SELinux vendor domains and require more verbose read rules.
#   On this device /vendor is really /system/vendor (old unpartitioned TN8 layout), so this
#   changes no actual layout and only simplifies SELinux and path handling.
#
# About SYSFS_PREFIX:
#   bcm_ldisc is a platform driver, so its sysfs directory carries a device instance number
#   (bcmbt_ldisc.<N>, where N depends on kernel probe order, not a constant). Upstream Sony
#   devices use .93, Nexus 9 uses another value. This must match the value printed by
#       ls /sys/bus/platform/drivers/bcm_ldisc/
#   otherwise uim fails to open and prints "unable to open .../install".
#   If the measured number differs, override BOARD_HAVE_BCM_FM_SYSFS in BoardConfig.mk
#   (this file already honours that override).
#

LOCAL_C_INCLUDES:= $(LOCAL_PATH)/include

LOCAL_SRC_FILES:= \
    uim.c \
    upio.c \
    brcm_hci_dump.c \
    btsnoop.c \
    utils.c

LOCAL_CFLAGS:= -c -W -Wall -O2 -D_POSIX_SOURCE -DUIM_DEBUG -DBLUEDROID_ENABLE_V4L2
# FIX NOTE (FM, 2026-11-16, measured on device):
# V4L2_SNOOP_ENABLE must be turned off. Its default is TRUE (fallback in
# include/v4l2_cfg.h:35 via #ifndef), and when enabled uim's main() reads a second config file:
#     if (vnd_load_conf(VENDOR_LIB_CONF_FILE, ...)) return UIM_FAIL;   // bt_vendor.conf, present
#     if (vnd_load_conf(STACK_CONF_FILE, ...))    return UIM_FAIL;     // bt_stack.conf
# STACK_CONF_FILE is /etc/bluetooth/bt_stack.conf, which this ROM does not install
# (it belongs to the Bluedroid tree, packages/modules/Bluetooth/system/conf/, not shipped).
# Measured log:
#     I brcm-uim: Attempt to load conf from /etc/bluetooth/bt_stack.conf
#     I brcm-uim: vnd_load_conf file >/etc/bluetooth/bt_stack.conf< not found
#     I init    : Service 'uim' (pid 741) exited with status 255
# So uim exits with UIM_FAIL immediately on every start, init restarts it every 5 seconds and
# it never reaches "open the install sysfs node and poll for the BT chip state".
# snoop (HCI capture dump) does nothing for FM, so turning it off is safe.
LOCAL_CFLAGS += -DV4L2_SNOOP_ENABLE=0
LOCAL_SHARED_LIBRARIES:= libnetutils libcutils liblog

SYSFS_PREFIX := "/sys/bus/platform/drivers/bcm_ldisc/bcmbt_ldisc.93/"
ifneq ($(BOARD_HAVE_BCM_FM_SYSFS),)
SYSFS_PREFIX := $(BOARD_HAVE_BCM_FM_SYSFS)
endif
LOCAL_CFLAGS += -DSYSFS_PREFIX=\"$(SYSFS_PREFIX)\"


ifneq ($(BOARD_BRCM_HCI_NUM),)
LOCAL_CFLAGS += -DBOARD_BRCM_HCI_NUM=$(BOARD_BRCM_HCI_NUM)
endif

# Install path stays /vendor/bin: the uim service in init.comms.rc references
# /vendor/bin/brcm-uim-sysfs (class late_start, user root, group bluetooth net_bt).
# On this device /vendor is actually /system/vendor (old TN8 layout, no separate partition), so
# both paths point at the same file; keeping init unchanged avoids touching the rc file.
LOCAL_MODULE := brcm-uim-sysfs
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)
