LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# UIM Application
#
# FM fix (2026-11-16):
# brcm-uim-sysfs moves the Broadcom combo chip (BCM4354) from "Bluetooth HCI mode"
# to "UIM/FM mode" before FM is opened: through the sysfs nodes of the bcm_ldisc
# driver (install / vendor_params / snoop_enable ...) it pushes patchram parameters
# and drives the chip's power sequence, after which libfmradio.v4l2-fm can talk to
# the chip over /dev/radio0.
#
# Installed to /system/bin instead of /vendor/bin:
#   it reads /system/etc/bluetooth/bt_vendor.conf (system_file), and a /vendor/bin
#   location would be constrained by the vendor SELinux domain and needs wordier read
#   rules for system files. /vendor here is really /system/vendor (the old TN8 layout
#   without a separate partition), so this changes no real layout, it only simplifies
#   SELinux and path checks.
#
# About SYSFS_PREFIX:
#   bcm_ldisc is a platform driver and its sysfs directory carries the device instance
#   number (bcmbt_ldisc.<N>, where N depends on probe order and is not fixed). The
#   upstream Sony device uses .93 and Nexus 9 something else. This value must match
#   what the kernel really creates, otherwise uim fails its open and logs "unable to
#   open .../install". If the measured number differs, override
#   BOARD_HAVE_BCM_FM_SYSFS in BoardConfig.mk (this file already honours it).
#

LOCAL_C_INCLUDES:= $(LOCAL_PATH)/include

LOCAL_SRC_FILES:= \
    uim.c \
    upio.c \
    brcm_hci_dump.c \
    btsnoop.c \
    utils.c

LOCAL_CFLAGS:= -c -W -Wall -O2 -D_POSIX_SOURCE -DUIM_DEBUG -DBLUEDROID_ENABLE_V4L2
# FM fix (2026-11-16, measured on the device):
# V4L2_SNOOP_ENABLE must be off. Its default is TRUE (the #ifndef fallback in
# include/v4l2_cfg.h:35), and with it on uim's main() reads a second config file:
#     if (vnd_load_conf(VENDOR_LIB_CONF_FILE, ...)) return UIM_FAIL;   // bt_vendor.conf, exists
# but STACK_CONF_FILE is /etc/bluetooth/bt_stack.conf and this ROM does not install it
# (it belongs to the Bluedroid tree, packages/modules/Bluetooth/system/conf/, and is
# not shipped). Measured log:
# so uim exited with UIM_FAIL on every start, init restarted it every 5 s and it never
# reached "open the install sysfs, poll for BT chip state". snoop (HCI capture) does
# nothing for FM, so turning it off is right.
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

# Installed to /vendor/bin: the uim service in init.comms.rc is written against
# /vendor/bin/brcm-uim-sysfs (class late_start, user root, group bluetooth net_bt).
# /vendor here is really /system/vendor (old TN8 layout, no separate partition), so
# both paths point at the same file, but matching the init config avoids touching the rc.
LOCAL_MODULE := brcm-uim-sysfs
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)
