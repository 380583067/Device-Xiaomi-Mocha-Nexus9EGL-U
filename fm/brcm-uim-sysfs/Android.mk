LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

#
# UIM Application
#
# 修复说明（FM，2026-11-16）：
# brcm-uim-sysfs 的作用是在 FM 打开前把 Broadcom 组合芯片（BCM4354）从
# 「蓝牙 HCI 模式」切到「UIM/FM 模式」：它通过 bcm_ldisc 驱动暴露的 sysfs
# 节点（install / vendor_params / snoop_enable …）下发 patchram 参数、控制
# 芯片上电时序，之后 libfmradio.v4l2-fm 才能通过 /dev/radio0 与芯片通信。
#
# 安装位置从 /vendor/bin 改成 /system/bin：
#   它要读 /system/etc/bluetooth/bt_vendor.conf（system_file），装到 /vendor/bin
#   会被 SELinux 的 vendor 域限制、并且给 system 分区文件的读权限配置更啰嗦。
#   本机 /vendor 其实就是 /system/vendor（不分区的老 TN8 布局），所以这个选择
#   不改变任何实际布局，只是简化 SELinux 与路径判断。
#
# 关于 SYSFS_PREFIX：
#   bcm_ldisc 是 platform driver，sysfs 目录名带设备实例编号
#   （bcmbt_ldisc.<N>，N 由内核探测顺序决定，不是固定值）。上游 Sony 设备是
#   .93，Nexus 9 是别的值。这个值必须和真机
#       ls /sys/bus/platform/drivers/bcm_ldisc/
#   的输出一致，否则 uim 会 open 失败并打印 "unable to open .../install"。
#   如果实测发现编号不同，改 BoardConfig.mk 的 BOARD_HAVE_BCM_FM_SYSFS 即可
#   （本文件已支持该覆盖）。
#

LOCAL_C_INCLUDES:= $(LOCAL_PATH)/include

LOCAL_SRC_FILES:= \
    uim.c \
    upio.c \
    brcm_hci_dump.c \
    btsnoop.c \
    utils.c

LOCAL_CFLAGS:= -c -W -Wall -O2 -D_POSIX_SOURCE -DUIM_DEBUG -DBLUEDROID_ENABLE_V4L2
# 修复说明（FM，2026-11-16，真机实测）：
# 必须关掉 V4L2_SNOOP_ENABLE。它的默认值是 TRUE（include/v4l2_cfg.h:35 的
# #ifndef 兜底），打开后 uim 的 main() 会去读第二个配置文件：
#     if (vnd_load_conf(VENDOR_LIB_CONF_FILE, ...)) return UIM_FAIL;   // bt_vendor.conf，存在
#     if (vnd_load_conf(STACK_CONF_FILE, ...))    return UIM_FAIL;     // bt_stack.conf
# 而 STACK_CONF_FILE 是 /etc/bluetooth/bt_stack.conf，本 ROM 并不安装这个文件
# （它属于 Bluedroid 源码树 packages/modules/Bluetooth/system/conf/，不进镜像）。
# 实测日志：
#     I brcm-uim: Attempt to load conf from /etc/bluetooth/bt_stack.conf
#     I brcm-uim: vnd_load_conf file >/etc/bluetooth/bt_stack.conf< not found
#     I init    : Service 'uim' (pid 741) exited with status 255
# 于是 uim 每次启动都立刻以 UIM_FAIL 退出，被 init 每 5 秒重启一次，永远走不到
# "打开 install sysfs、poll 等 BT 芯片状态" 那一步。
# snoop（HCI 抓包转储）对 FM 功能没有任何作用，关掉即可。
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

# 安装位置保持 /vendor/bin：init.comms.rc 里的 uim service 就是按
# /vendor/bin/brcm-uim-sysfs 写的（class late_start, user root, group bluetooth net_bt）。
# 本机 /vendor 实际上是 /system/vendor（TN8 老布局，不单独分区），所以这两个路径
# 指向同一个文件，但按 init 配置保持一致可省去改 rc 的风险。
LOCAL_MODULE := brcm-uim-sysfs
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_OWNER := sony
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)
