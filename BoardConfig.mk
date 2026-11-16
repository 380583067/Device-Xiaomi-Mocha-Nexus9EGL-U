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
# NOTE: Xiaomi Mi Pad 1 (mocha) uses a Broadcom BCM4354 combo chip (WiFi/BT/FM shared),
# FM works through the HCI channel of the Bluetooth chip plus a V4L2 driver (kernel:
# drivers/bluetooth/broadcom/v4l2_fm_driver, device node /dev/radio0). No extra FM hardware.
#
# FIX NOTE (2026-11-16, FM brought up successfully):
# The key point is: do NOT let LineageOS reference hardware/broadcom/fm. That path is where
# upstream keeps the Broadcom FM sources; this device tree keeps them in device/xiaomi/mocha/fm.
# In vendor/lineage/config/BoardConfigSoong.mk the libfmjni namespace selection is:
#     BOARD_HAVE_QCOM_FM -> vendor/qcom/opensource/libfmjni
#     BOARD_HAVE_BCM_FM  -> hardware/broadcom/fm          <-- this directory does not exist here
#     BOARD_HAVE_SLSI_FM -> hardware/samsung_slsi/fm
#     otherwise (i.e. BOARD_HAVE_MTK_FM=true) -> no namespace is added at all
# FMRadio declares jni_libs: ["libfmjni"], which must resolve to our Broadcom flavor,
# already converted to a Soong module (device/xiaomi/mocha/fm/libfmjni/Android.bp,
# which lives in the root namespace), so no extra namespace declaration is needed.
#
# BOARD_HAVE_MTK_FM must also be true: its purpose is to *exclude*
# packages/apps/FMRadio/jni/fmr (the MTK build of libfmjni: same name, incompatible chip).
# If all three switches are false the MTK namespace is added and the two libfmjni collide.
BOARD_HAVE_BCM_FM := false
BOARD_HAVE_MTK_FM := true
# sysfs directory name of bcm_ldisc.
# Verified on the device (2026-11-16, queried via adb shell from TWRP):
#     /sys/bus/platform/drivers/bcm_ldisc/ only holds bind/uevent/unbind, plus
#     a symlink named bcm_ldisc pointing to ../../../../devices/platform/bcm_ldisc
#     /sys/devices/platform/bcm_ldisc/ is where the real device nodes live:
#         bdaddr  bt_err  fm_err  fw_patchfile  install  snoop_enable  vendor_params
# The default in brcm-uim-sysfs/Android.mk, /sys/bus/platform/drivers/bcm_ldisc/bcmbt_ldisc.93/,
# comes from an upstream Sony device (there the ldisc is a sub-device with an instance, bcmbt_ldisc.NN).
# The kernel used here (drivers/bluetooth/broadcom/line_discipline_driver) is a platform driver
# without instances, so it must be overridden to /sys/devices/platform/bcm_ldisc/.
# Consequence of not overriding: uim fails to open(".../bcmbt_ldisc.93/install") on every start,
# prints "unable to open ... install" and exits; init restarts it every 5 seconds and it can never
# switch the BCM4354 into FM mode.
BOARD_HAVE_BCM_FM_SYSFS := "/sys/devices/platform/bcm_ldisc/"

# ==== Line discipline number for N_BRCM_HCI (FM, measured on device 2026-11-16) ====
#
# It must be set explicitly to 26 to stay consistent with the kernel.
#
# Kernel side: drivers/bluetooth/broadcom/line_discipline_driver/brcm_hci_uart.h
#     #ifndef N_BRCM_HCI
#     #define N_BRCM_HCI  26
#     #endif
# and brcm_sh_ldisc.c calls tty_register_ldisc(N_BRCM_HCI, &hci_uart_ldisc)
# to register this ldisc on slot 26.
#
# Userspace: brcm-uim-sysfs/include/uim.h
#     #ifdef BOARD_BRCM_HCI_NUM
#     #define N_BRCM_HCI BOARD_BRCM_HCI_NUM
#     #else
#     #define N_BRCM_HCI 25          <-- upstream default
#     #endif
#
# In other words: unless it is defined here, uim will use 25 to
#     ioctl(dev_fd, TIOCSETD, &ldisc)   // ldisc = 25
# Slot 25 is a different ldisc in this kernel (N_GSM0710MUX). TIOCSETD will *succeed*, because
# any registered slot is accepted - but what gets installed is not the Broadcom ldisc. The
#     ioctl(dev_fd, HCIUARTSETPROTO, HCI_UART_H4)
# following calls then land on the wrong ldisc ioctl and fail. Measured log:
#     I brcm-uim: baud rate set to 3000000
#     I brcm-uim: fw_patchfile_name = BCM4350C0.hcd
#     E brcm-uim: ERR Can't set hci protocol
#     E brcm-uim: ERR st_uart_config failed
# Meanwhile the kernel is still waiting on the ldisc-installed completion, so it prints:
#     (brcmldisc)ERR:brcm_sh_ldisc_stop timed out waiting for ldisc to be un-installed
#     (v4l2fmdrv):fmc_prepare (fmdrv): Failed to get shared ldisc write func pointer
#     (fmdrv): Unable to prepare FM CORE
# Eventually open("/dev/radio0") returns -EAGAIN and FM cannot be opened.
BOARD_BRCM_HCI_NUM := 26

# FS
BOARD_CACHEIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_USERDATAIMAGE_FILE_SYSTEM_TYPE := ext4
TARGET_EXFAT_DRIVER := sdfat
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USES_MKE2FS := true

# Graphics display
TARGET_SCREEN_DENSITY := 256

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
# FIX NOTE: add a libnvomxadaptor.so -> libnvomxadaptor_shim.so mapping.
# libnvomxadaptor.so (the NVIDIA hardware OMX decoding adaptor) references an old-ABI symbol
# GraphicBuffer(ANativeWindowBuffer*, bool) which was removed in Android 10+. A preloaded shim
# supplies the symbol, otherwise hardware video decoding is unavailable.
TARGET_LD_SHIM_LIBS := \
    /system/vendor/lib/hw/hwcomposer.tegra.so|libshims_ui.so \
    /system/vendor/lib/libnvcap_video.so|libshims_ui.so \
    /system/vendor/lib/libnvgr.so|libshim_atomic.so \
    /system/vendor/lib/libnvomxadaptor.so|libnvomxadaptor_shim.so \
    /system/vendor/lib/libsensors.lsm6db0.so|libsensors_fastsys_shim.so
# FIX NOTE (boot speed, 2026-11-16): add a shim mapping for the sensor HAL blob.
# libsensors.lsm6db0.so calls system("rm -rf /sys/bus/iio/devices/...") during init,
# which costs 1.5-2 seconds per call on Android 14 (posix_spawn of sh + waitpid). What it
# tries to delete are sysfs nodes, so it always fails. sensorservice therefore needs 5.68s
# to register, which makes DeviceStateManagerService block for 6 seconds (see first-boot log
# "Waiting for service 'sensorservice'"，14.600 → 20.614）。
# The shim short-circuits this useless call while forwarding other commands to libc; see
# the header comment in shims/libsensors_fastsys_shim.cpp.

# ============================================================================
# Camera blob shim mapping - implemented but *disabled by default*; reasons follow (important)
# ----------------------------------------------------------------------------
# What is already done (all code is in place and can be re-enabled at any time):
#   camera/mocha_camera_shim.cpp      ABI forwarding for CameraMetadata/Fence/SensorManager
#                                     + libstdc++ iostream + NvPHSSetThrottle
#                                     plus the new NvRmSync API
#   camera/stub_libcameraservice.cpp  empty stub whose SONAME pretends to be libcameraservice.so
#   camera/stub_libpowerservice_client.cpp  minimal PowerServiceClient implementation
#   camera/libbeautify_stub.cpp       stand-in for the vendor beautify lib depending on TEXTREL libFaceProc
#   camera/camera_shim_test.cpp       diagnostic tool; all 345 undefined symbols verified resolvable
#
# Measured result (verified on the device, 2026-11):
#   With all of the above in place camera.vendor.tegra.so does dlopen successfully (ALL OK)
#   and cameraserver really does enter the blob init flow. But this path cannot work on the *current kernel*:
#
#   1) The blob uses NVIDIA legacy PCL and hardcodes open("/dev/camera"). The original device tree
#      created it as a directory via mkdir in init.tegra.rc, and the blob reported
#      "NvcImager_Open: Can not open camera device /dev/camera: Is a directory"。
#      (fixed: changed to symlink /dev/camera -> /dev/camera.pcl)
#
#   2) After that fix the blob probes PCL chips one by one; each probe fails and times out after 4s:
#        PCLDeviceRead: pcl_OV5693 1 - -1
#        PCLDeviceRead: pcl_IMX135 2 - -1
#        PCLDeviceRead: pcl_AR0261 3 - -1   ... 7 in total
#      because the camera device tree in this kernel was rewritten around the *V4L2 subdev framework*,
#      （tegra124-mocha-camera-mc.dtsi：imx179@10 / ov5693@36 / ad5823@0c），
#      so there is no PCL node configuration the blob expects; only an empty pcl-generic exists.
#
#   3) Worst of all, these I2C probe timeouts hold the i2c bus lock for a long time and *freeze the system*
#      (measured: adb shell blocks completely, even adb reboot does not respond, long power press only).
#
# The right approach is therefore the in-house V4L2 HAL matching the device tree (camera/MochaCameraHAL.cpp +
#   CameraPipeline.cpp + isp/ + v4l2/ + tuning/) instead of continuing to patch the blob.
#   Until the HAL switch is complete the shim mapping must stay off, otherwise boot freezes.
#
# To restore: uncomment the two lines below (and set the getprop switch in CameraWrapper.cpp,
# persist.mocha.camera.blob_preload, to 1).
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
