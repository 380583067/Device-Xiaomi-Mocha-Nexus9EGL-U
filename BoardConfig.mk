#
# Copyright (C) 2014 The CyanogenMod Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Path
LOCAL_PATH := device/xiaomi/mocha
BOARD_VENDOR := Xiaomi

# apex
USE_CXX_STL := libc++_shared
TARGET_USES_LEGACY_ICU := true

# Allow duplicate rules to override them
BUILD_BROKEN_DUP_RULES := true

# Audio
USE_XML_AUDIO_POLICY_CONF:= 1
USE_CUSTOM_AUDIO_POLICY  := 1
BOARD_USES_GENERIC_AUDIO := false
BOARD_USES_ALSA_AUDIO    := true
BOARD_USES_TINYHAL_AUDIO := true

# Architecture
TARGET_CPU_ABI := armeabi-v7a
TARGET_CPU_ABI2 := armeabi
TARGET_ARCH := arm
TARGET_ARCH_VARIANT := armv7-a-neon
TARGET_CPU_VARIANT := cortex-a15
TARGET_CPU_SMP := true
TARGET_NOT_USE_GZIP_RECOVERY_RAMDISK := true

# Bluetooth
BOARD_HAVE_BLUETOOTH := true
BOARD_HAVE_BLUETOOTH_BCM := true
BCM_BLUETOOTH_MANTA_BUG := true
BOARD_BLUETOOTH_BDROID_BUILDCFG_INCLUDE_DIR := $(LOCAL_PATH)/bluetooth
BOARD_BLUETOOTH_HIDL_GD := false

# Board
TARGET_BOARD_PLATFORM := tegra
TARGET_NO_BOOTLOADER := true
TARGET_NO_RADIOIMAGE := true

# Boot animation
TARGET_SCREEN_HEIGHT := 2048
TARGET_SCREEN_WIDTH := 1536
TARGET_BOOTANIMATION_HALF_RES := true

# Camera
#TARGET_HAS_LEGACY_CAMERA_HAL1 := true
#TARGET_NEEDS_PLATFORM_TEXT_RELOCATIONS := true

# Dexpreopt
WITH_DEXPREOPT := true
WITH_DEXPREOPT_BOOT_IMG_AND_SYSTEM_SERVER_ONLY := false

# ELF
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true
BUILD_BROKEN_PREBUILT_ELF_FILES := true

# FM
# 说明：小米平板1 (mocha) 使用 Broadcom BCM4354 组合芯片（WiFi/BT/FM 共用），
# FM 通过蓝牙芯片的 HCI 通道 + V4L2 驱动（kernel: drivers/bluetooth/broadcom/v4l2_fm_driver，
# 设备节点 /dev/radio0）工作，无需额外 FM 硬件。
#
# 修复说明（2026-11-16，FM 已打通）：
# 关键点在于 **不要让 LineageOS 去引用 hardware/broadcom/fm**。那个路径是上游放
# 博通 FM 源码的标准位置，本设备树把源码放在 device/xiaomi/mocha/fm 里。
# vendor/lineage/config/BoardConfigSoong.mk 里 libfmjni 的 namespace 选择是：
#     BOARD_HAVE_QCOM_FM -> vendor/qcom/opensource/libfmjni
#     BOARD_HAVE_BCM_FM  -> hardware/broadcom/fm          <-- 我们这里没有这个目录
#     BOARD_HAVE_SLSI_FM -> hardware/samsung_slsi/fm
#     否则（即 BOARD_HAVE_MTK_FM=true）-> 不加任何 namespace
# 而 FMRadio 的 jni_libs: ["libfmjni"] 需要能解析到我们的博通版 libfmjni，
# 它已经改成 Soong 模块（device/xiaomi/mocha/fm/libfmjni/Android.bp，位于 root
# namespace），所以不需要额外的 namespace 声明。
#
# 同时必须设 BOARD_HAVE_MTK_FM := true：它的作用是**排除**
# packages/apps/FMRadio/jni/fmr（MTK 版 libfmjni，同名、芯片不兼容）。
# 若三个开关全 false，那个 MTK namespace 会被加入，两个 libfmjni 就会撞车。
BOARD_HAVE_BCM_FM := false
BOARD_HAVE_MTK_FM := true
# bcm_ldisc 的 sysfs 目录名。
# 实测确认（2026-11-16，TWRP 下 adb shell 直接查）：
#     /sys/bus/platform/drivers/bcm_ldisc/ 下只有 bind/uevent/unbind，
#     一个名为 bcm_ldisc 的符号链接指向 ../../../../devices/platform/bcm_ldisc
#     /sys/devices/platform/bcm_ldisc/ 下才是真正的设备节点：
#         bdaddr  bt_err  fm_err  fw_patchfile  install  snoop_enable  vendor_params
# 而 brcm-uim-sysfs/Android.mk 里的默认值 /sys/bus/platform/drivers/bcm_ldisc/bcmbt_ldisc.93/
# 是上游 Sony 设备的写法（那边 ldisc 是带实例号的子设备 bcmbt_ldisc.NN），
# 本机内核（drivers/bluetooth/broadcom/line_discipline_driver）是平台驱动、
# 不分实例，所以必须覆盖成 /sys/devices/platform/bcm_ldisc/。
# 不覆盖的后果：uim 每次启动都 open(".../bcmbt_ldisc.93/install") 失败，
# 打印 "unable to open ... install" 后退出，被 init 每 5 秒重启一次，永远无法
# 把 BCM4354 切到 FM 模式。
BOARD_HAVE_BCM_FM_SYSFS := "/sys/devices/platform/bcm_ldisc/"

# ==== N_BRCM_HCI 的 line discipline 编号（FM，2026-11-16 真机实测）====
#
# 必须显式设成 26，和内核保持一致。
#
# 内核侧：drivers/bluetooth/broadcom/line_discipline_driver/brcm_hci_uart.h
#     #ifndef N_BRCM_HCI
#     #define N_BRCM_HCI  26
#     #endif
# 并且 brcm_sh_ldisc.c 用 tty_register_ldisc(N_BRCM_HCI, &hci_uart_ldisc)
# 把这个 ldisc 注册在 26 号上。
#
# 用户态：brcm-uim-sysfs/include/uim.h
#     #ifdef BOARD_BRCM_HCI_NUM
#     #define N_BRCM_HCI BOARD_BRCM_HCI_NUM
#     #else
#     #define N_BRCM_HCI 25          <-- 上游默认值
#     #endif
#
# 也就是说：只要这里不定义，uim 就会用 25 去做
#     ioctl(dev_fd, TIOCSETD, &ldisc)   // ldisc = 25
# 而 25 在内核里是别的 ldisc（N_GSM0710MUX），TIOCSETD 会**成功**，因为
# 只要那个号注册过就行 —— 但装上去的完全不是博通的 ldisc。紧接着的
#     ioctl(dev_fd, HCIUARTSETPROTO, HCI_UART_H4)
# 就落到错误的 ldisc ioctl 上，必然失败。实测日志：
#     I brcm-uim: baud rate set to 3000000
#     I brcm-uim: fw_patchfile_name = BCM4350C0.hcd
#     E brcm-uim: ERR Can't set hci protocol
#     E brcm-uim: ERR st_uart_config failed
# 而内核侧此时还在等 ldisc 装好的完成量，于是打出：
#     (brcmldisc)ERR:brcm_sh_ldisc_stop timed out waiting for ldisc to be un-installed
#     (v4l2fmdrv):fmc_prepare (fmdrv): Failed to get shared ldisc write func pointer
#     (fmdrv): Unable to prepare FM CORE
# 最终 open("/dev/radio0") 返回 -EAGAIN，FM 打不开。
BOARD_BRCM_HCI_NUM := 26

# FS
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_EXFAT_DRIVER := sdfat
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USES_MKE2FS := true

# Graphics display
TARGET_SCREEN_DENSITY := 320

# Graphics
NUM_FRAMEBUFFER_SURFACE_BUFFERS := 3
SF_VSYNC_EVENT_PHASE_OFFSET_NS := 5000000
VSYNC_EVENT_PHASE_OFFSET_NS := 7500000

# Gralloc
#TARGET_ADDITIONAL_GRALLOC_10_USAGE_BITS := 0x2000U | 0x02000000U

# HIDL Manifest
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true
DEVICE_MANIFEST_FILE := $(LOCAL_PATH)/manifest.xml
DEVICE_MATRIX_FILE := $(LOCAL_PATH)/compatibility_matrix.xml
DEVICE_FRAMEWORK_COMPATIBILITY_MATRIX_FILE := \
    $(LOCAL_PATH)/framework_compatibility_matrix.xml \
    vendor/lineage/config/device_framework_matrix.xml

# IPv6
BOARD_DISABLE_IPV6 := true

# Include
TARGET_SPECIFIC_HEADER_PATH := $(LOCAL_PATH)/include

# Include an expanded selection of fonts
EXTENDED_FONT_FOOTPRINT := true

# Init
TARGET_INIT_VENDOR_LIB := libinit_mocha
TARGET_RECOVERY_DEVICE_MODULES := libinit_mocha

# Vendor Init
TARGET_LIBINIT_DEFINES_FILE := $(LOCAL_PATH)/init/init_mocha.cpp

# Iptables
TARGET_IPTABLES_BACKEND := legacy

# Netd
BOARD_DISABLE_NETD_ADVANCED_RULES := true
TARGET_USES_LEGACY_NETD := true
BUILD_BROKEN_BPF_KERNEL := true

# Kernel
BOARD_KERNEL_CMDLINE := vpr_resize androidboot.selinux=permissive vmalloc=400M androidboot.hardware=tn8
BOARD_KERNEL_BASE := 0x10000000
BOARD_RAMDISK_OFFSET := 0x02000000
BOARD_KERNEL_PAGESIZE := 2048
BOARD_KERNEL_TAGS_OFFSET := 0x00000100
TARGET_KERNEL_SOURCE := kernel/xiaomi/mocha
TARGET_KERNEL_CONFIG := tegra12_android_defconfig
BOARD_KERNEL_IMAGE_NAME := zImage
BOARD_KERNEL_SEPARATED_DT := true
BOARD_MKBOOTIMG_ARGS := --ramdisk_offset $(BOARD_RAMDISK_OFFSET) --tags_offset $(BOARD_KERNEL_TAGS_OFFSET)
BOARD_CUSTOM_BOOTIMG := true
BOARD_CUSTOM_BOOTIMG_MK := $(LOCAL_PATH)/mkbootimg.mk
#BOARD_SYSTEMIMAGE_PARTITION_SIZE := 671088640 # 640 Mb stock partition table
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 3001024512 # 2.8 Gb
BOARD_USERDATAIMAGE_PARTITION_SIZE := 11196694528
BOARD_CACHEIMAGE_PARTITION_SIZE := 387973120
BOARD_BOOTIMAGE_PARTITION_SIZE := 20971520
BOARD_PERSISTIMAGE_PARTITION_SIZE := 16777216
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 20971520
BOARD_FLASH_BLOCK_SIZE := 131072
TARGET_KERNEL_CLANG_COMPILE := false
TARGET_KERNEL_ADDITIONAL_FLAGS := \
    HOSTCFLAGS="-fuse-ld=lld -Wno-unused-command-line-argument"

# LINEAGEHW
JAVA_SOURCE_OVERLAYS := org.lineageos.hardware|$(LOCAL_PATH)/lineagehw|**/*.java

# LightHAL
TARGET_LIGHTHAL_VARIANT := tegra

# Malloc
MALLOC_SVELTE := true

# OTA mocha
TARGET_RELEASETOOLS_EXTENSIONS := device/xiaomi/mocha/releasetools

# Memfd
TARGET_HAS_MEMFD_BACKPORT := true

# Offmode Charging
HEALTHD_ENABLE_TRICOLOR_LED := true
BOARD_CHARGER_ENABLE_SUSPEND := true
BOARD_CHARGER_SHOW_PERCENTAGE := true
BOARD_CHARGER_DISABLE_INIT_BLANK := true
BACKLIGHT_PATH := /sys/devices/platform/tegra12-i2c.0/i2c-0/0-002c/backlight/lcd-backlight/brightness
RED_LED_PATH := /sys/class/leds/red/brightness
GREEN_LED_PATH := /sys/class/leds/green/brightness
BLUE_LED_PATH := /sys/class/leds/blue/brightness

# Per-application sizes for shader cache
MAX_EGL_CACHE_SIZE := 4194304
MAX_EGL_CACHE_ENTRY_SIZE := 262144

# PowerHAL
TARGET_POWERHAL_VARIANT := tegra

# Properties
TARGET_SYSTEM_PROP += device/xiaomi/mocha/system.prop
TARGET_PRODUCT_PROP += device/xiaomi/mocha/product.prop

# Recovery
TARGET_RECOVERY_DEVICE_DIRS += device/xiaomi/mocha
TARGET_RECOVERY_FSTAB := device/xiaomi/mocha/initfiles/fstab.tn8
BOARD_NO_SECURE_DISCARD := true

# RenderScript
OVERRIDE_RS_DRIVER := libnvRSDriver.so
BOARD_OVERRIDE_RS_CPU_VARIANT_32 := cortex-a15

# SELinux
SELINUX_IGNORE_NEVERALLOWS := true
# This checkout keeps NVIDIA's common, Lineage common, and Mocha policy rules
# in the device repository itself. The separately synced device/nvidia policy
# project is optional, so an absent checkout must not stop product parsing.
-include device/nvidia/sepolicy/sepolicy.mk
BOARD_SEPOLICY_DIRS += \
    $(LOCAL_PATH)/sepolicy/common \
    $(LOCAL_PATH)/sepolicy/lineage-common \
    $(LOCAL_PATH)/sepolicy/mocha

# SHIMS
# 修复说明：新增 libnvomxadaptor.so -> libnvomxadaptor_shim.so 映射。
# libnvomxadaptor.so（NVIDIA 硬件 OMX 解码 adaptor）引用旧 ABI 符号
# GraphicBuffer(ANativeWindowBuffer*, bool)，Android 10+ 已移除，需预加载 shim
# 补齐符号，否则媒体硬件解码不可用。
# HWC2（hwc2/ 子目录，mocha.mk 有完整说明）：
# VIC 硬件合成引擎禁用 —— mocha 的 libnvddk_vic.so 在 NvDdkVicCreateSession
# 内部直接 SIGSEGV（NvRm 代际断层，同 Vulkan 不可用的根因），错误码都来不及
# 返回。TegraDisplayPipeline 对 mVic==null 有完整降级路径（不合并/不捕获/无
# cursor plane，全部走 window plane 或 client 合成），行为安全。
TARGET_HWC_NO_VIC := true

# 调试开关（2026-11-19 黑屏排查）：HWC2 的 per-frame trace（每帧的 plane 规划、
# 描述符去向、硬件应答）。运行时由 setprop vendor.hwc.trace 1 控制是否输出。
TARGET_HWC_TRACE := true

TARGET_LD_SHIM_LIBS := \
    /system/vendor/lib/libnvcap_video.so|libshims_ui.so \
    /system/vendor/lib/libnvgr.so|libshim_atomic.so \
    /system/vendor/lib/libnvomxadaptor.so|libnvomxadaptor_shim.so \
    /system/vendor/lib/libsensors.lsm6db0.so|libsensors_fastsys_shim.so
# 修复说明（HWC2，2026-11-19）：hwcomposer.tegra.so 现在是源码 HWC2 实现
# （hwc2/ 子目录，mocha.mk 有完整说明），直接链接系统 libui，不再需要
# libshims_ui 的 7 参 GraphicBuffer 桥 —— 该桥只服务于原厂 HWC1 blob。
# 原厂 blob 改名 hwcomposer.tegra_legacy.so 保留（vendor 仓库）；
# 若要切回 blob 路线，除了 TWRP 换文件名，还要把下面这行加回来：
#     /system/vendor/lib/hw/hwcomposer.tegra.so|libshims_ui.so \
# 修复说明（开机速度，2026-11-16）：新增传感器 HAL blob 的 shim 映射。
# libsensors.lsm6db0.so 初始化时调 system("rm -rf /sys/bus/iio/devices/...")，
# 在 Android 14 上一次要 1.5~2 秒（posix_spawn sh + waitpid），而它要删的是
# sysfs 节点、必然失败。sensorservice 因此需要 5.68 秒才注册，连带让
# DeviceStateManagerService 同步等待 6 秒（见首次开机日志
# "Waiting for service 'sensorservice'"，14.600 → 20.614）。
# shim 把这条无效调用短路，其它命令照旧转交 libc，详见
# shims/libsensors_fastsys_shim.cpp 头部注释。

# ============================================================================
# 相机 blob 的 shim 映射 —— 已实现但**默认不启用**，原因如下（重要）
# ----------------------------------------------------------------------------
# 已完成的部分（代码都在，随时可恢复）：
#   camera/mocha_camera_shim.cpp      CameraMetadata/Fence/SensorManager 的 ABI 转发
#                                     + libstdc++ iostream + NvPHSSetThrottle
#                                     + NvRmSync 新 API
#   camera/stub_libcameraservice.cpp  空 stub，SONAME 伪装成 libcameraservice.so
#   camera/stub_libpowerservice_client.cpp  PowerServiceClient 最小实现
#   camera/libbeautify_stub.cpp       顶替依赖带 TEXTREL 的 libFaceProc 的原厂美颜库
#   camera/camera_shim_test.cpp       诊断工具，实测 345 个未定义符号全部可解析
#
# 实测结论（2026-11 在真机上验证）：
#   补完上面这些之后 camera.vendor.tegra.so 确实能 dlopen 成功了（ALL OK），
#   cameraserver 也真的进入了 blob 的初始化流程。但这条路在**当前内核**上走不通：
#
#   1) blob 走的是 NVIDIA 老式 PCL 通路，硬编码打开 "/dev/camera"。原设备树在
#      init.tegra.rc 里 mkdir /dev/camera 把它建成了目录，blob 报
#      "NvcImager_Open: Can not open camera device /dev/camera: Is a directory"。
#      （该问题已修：改成 symlink /dev/camera -> /dev/camera.pcl）
#
#   2) 修掉之后 blob 开始逐个探测 PCL 芯片，全部失败且各超时 4 秒：
#        PCLDeviceRead: pcl_OV5693 1 - -1
#        PCLDeviceRead: pcl_IMX135 2 - -1
#        PCLDeviceRead: pcl_AR0261 3 - -1   ... 共 7 个
#      因为本内核的摄像头设备树是按 **V4L2 subdev 框架**重写的
#      （tegra124-mocha-camera-mc.dtsi：imx179@10 / ov5693@36 / ad5823@0c），
#      根本没有 blob 需要的 PCL 节点配置，内核里只有 pcl-generic 的空壳。
#
#   3) 最严重的是：这些 I2C 探测超时会长时间占住 i2c 总线锁，**把整个系统卡死**
#      （实测 adb shell 全部阻塞、adb reboot 也无响应，只能长按电源键）。
#
#   所以正确的路线是配套设备树的 V4L2 自研 HAL（camera/MochaCameraHAL.cpp +
#   CameraPipeline.cpp + isp/ + v4l2/ + tuning/），而不是继续修 blob。
#   在 HAL 切换完成之前，这里不能打开 shim 映射，否则开机就会卡死。
#
# 恢复方法：把下面两行取消注释，并把 CameraWrapper.cpp 从 git 历史取回
# （commit 937cb6c 之前），同时把其中的 getprop 开关
# persist.mocha.camera.blob_preload 设为 1。
# TARGET_LD_SHIM_LIBS += \
#     /system/lib/hw/camera.vendor.tegra.so|libmocha_camera_shim.so \
#     /system/lib/hw/camera.vendor.tegra.so|libcameraservice_mocha.so
# ============================================================================

# ThermalHAL
TARGET_THERMALHAL_VARIANT := tegra

# WEBGL in WebKit
ENABLE_WEBGL := true

# Wifi related defines
BOARD_WPA_SUPPLICANT_DRIVER      := NL80211
WPA_SUPPLICANT_VERSION           := VER_0_8_X
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_bcmdhd
BOARD_WLAN_DEVICE                := bcmdhd
BOARD_HOSTAPD_DRIVER             := NL80211
BOARD_HOSTAPD_PRIVATE_LIB        := lib_driver_cmd_bcmdhd
WIFI_DRIVER_FW_PATH_STA          := "/vendor/firmware/fw_bcmdhd.bin"
WIFI_DRIVER_FW_PATH_AP           := "/vendor/firmware/fw_bcmdhd_apsta.bin"
WIFI_DRIVER_FW_PATH_PARAM        := "/sys/module/bcmdhd/parameters/firmware_path"
#WIFI_DRIVER_MODULE_ARG           := "iface_name=wlan0"
#WIFI_DRIVER_MODULE_NAME          := "bcmdhd"
WIFI_HIDL_UNIFIED_SUPPLICANT_SERVICE_RC_ENTRY := true
               
# Zygote whitelist extra paths
ZYGOTE_WHITELIST_PATH_EXTRA := \"/dev/nvhost-ctrl\",\"/dev/nvmap\"

# Security patch level
VENDOR_SECURITY_PATCH := 2025-04-05
