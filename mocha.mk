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
LOCAL_PATH := device/xiaomi/mocha

# Vendor Blob
$(call inherit-product-if-exists, vendor/xiaomi/mocha/mocha-vendor.mk)
$(call inherit-product-if-exists, vendor/xiaomi/mocha/consolemode-blobs.mk)

# API
PRODUCT_PACKAGES += $(PRODUCT_PACKAGES_SHIPPING_API_LEVEL_34)

# APEX
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/ld.config.txt:$(TARGET_COPY_OUT_SYSTEM)/etc/swcodec/ld.config.txt

# Audio
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/audio/audio.mocha.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio.mocha.xml \
    $(LOCAL_PATH)/audio/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml \
    $(LOCAL_PATH)/audio/nvaudio_conf.xml:$(TARGET_COPY_OUT_VENDOR)/etc/nvaudio_conf.xml  \
    $(LOCAL_PATH)/audio/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml

PRODUCT_PACKAGES += \
    android.hardware.audio.service \
    android.hardware.audio@7.1-impl \
    android.hardware.audio.effect@7.0-impl \
    android.hardware.soundtrigger@2.2-impl \
    audio.usb.default \
    audio.r_submix.default \
    audio.primary.tegra \
    libaudiohalcm \
    libaudio-resampler \
    libaudiospdif \
    libtinycompress \
    tinycap_mocha \
    tinymix_mocha \
    tinyplay_mocha \
    tinypcminfo_mocha \
    libtinyalsa

# Audio configuration
PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/config/a2dp_in_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/a2dp_in_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/audio_policy_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_volumes.xml \
    frameworks/av/services/audiopolicy/config/bluetooth_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/bluetooth_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/default_volume_tables.xml:$(TARGET_COPY_OUT_VENDOR)/etc/default_volume_tables.xml \
    frameworks/av/services/audiopolicy/config/r_submix_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/r_submix_audio_policy_configuration.xml \
    frameworks/av/services/audiopolicy/config/usb_audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/usb_audio_policy_configuration.xml

PRODUCT_COPY_FILES += \
    frameworks/av/services/audiopolicy/enginedefault/config/example/phone/audio_policy_engine_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_engine_configuration.xml \
    frameworks/av/services/audiopolicy/enginedefault/config/example/phone/audio_policy_engine_default_stream_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_engine_default_stream_volumes.xml \
    frameworks/av/services/audiopolicy/enginedefault/config/example/phone/audio_policy_engine_product_strategies.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_engine_product_strategies.xml \
    frameworks/av/services/audiopolicy/enginedefault/config/example/phone/audio_policy_engine_stream_volumes.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_engine_stream_volumes.xml

# Bluetooth
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/bluetooth/bt_vendor.conf:system/etc/bluetooth/bt_vendor.conf \
    $(LOCAL_PATH)/initfiles/init.btloader.sh:system/bin/init.btloader.sh

PRODUCT_PACKAGES += \
    libbt-vendor \
    audio.bluetooth.default \
    android.hardware.bluetooth.audio@2.1-impl \
    android.hardware.bluetooth@1.0-impl \
    android.hardware.bluetooth@1.0-service

# TEGRA_BT
PRODUCT_PROPERTY_OVERRIDES += \
    bluetooth.profile.asha.central.enabled=true \
    bluetooth.profile.a2dp.source.enabled=true \
    bluetooth.profile.avrcp.target.enabled=true \
    bluetooth.profile.gatt.enabled=true \
    bluetooth.profile.hid.host.enabled=true \
	persist.bluetooth.turn_off_bt_main_thread=true \
    ro.bluetooth.request.master=true \
    ro.bluetooth.remote.autoconnect=true \
    bluetooth.a2dp.sink.enabled=false

# Camera
# FIX NOTE: the vendor NVIDIA camera blob (camera.vendor.tegra.so) reads at runtime
# /system/etc/nvcamera.conf and /system/etc/model_frontal.xml (absolute paths, see the
# blob source NvCameraHal). They were previously copied to /vendor/etc/, so the blob
# could not read its config and camera init failed. Now copied to system/etc/. Ref: Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/nvcamera.conf:system/etc/nvcamera.conf \
    $(LOCAL_PATH)/camera/model_frontal.xml:system/etc/model_frontal.xml

# NOTE: the camera wrapper (camera.tegra) is the original device-tree approach
# (CameraWrapper + Camera3Wrapper around the vendor blob).
#
# FIX NOTE (camera reports zero devices):
#   The vendor blob camera.vendor.tegra.so fails to dlopen on Android 14: 2 DT_NEEDED
#   files and 9 Android C++ ABI symbols are missing. See camera/Android.mk and the
#   header of camera/mocha_camera_shim.cpp. Three fallback libraries are shipped:
#     libcameraservice_mocha     -> builds libcameraservice_mocha.so whose SONAME
#                                   declares itself as libcameraservice.so (empty stub)
#     libpowerservice_client     -> PowerServiceClient constructor + sendPowerHint
#     libmocha_camera_shim       -> CameraMetadata/Fence/SensorManager ABI forwarding
#     libbeautify                -> replaces the vendor beautify lib (which depends on the
#                                   TEXTREL libFaceProc.so, refused by Android 14)
# libmocha_camera_shim is preloaded through TARGET_LD_SHIM_LIBS in BoardConfig.mk.
PRODUCT_PACKAGES += \
    camera.device@1.0-impl \
    android.hardware.camera.provider@2.4-impl \
    camera.tegra \
    libcameraservice_mocha \
    libpowerservice_client \
    libmocha_camera_shim \
    libbeautify

# Comm Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.sip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.xml \
    frameworks/native/data/etc/android.software.sip.voip.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.sip.voip.xml

# Configstore HAL
PRODUCT_PACKAGES += \
    android.hardware.configstore@1.1-service

# ChargerTile
PRODUCT_PACKAGES += \
    charger \
    charger_res_images

# Charging LED
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/charger_led.sh:system/bin/charger_led.sh

# DRM HAL
PRODUCT_PACKAGES += \
    android.hardware.drm-service.clearkey

# Display Device Config
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/displayconfig/display_id_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_0.xml

# fastbootd
PRODUCT_PACKAGES += \
    fastbootd

# FM Radio (Broadcom BCM4354)
# Full stack (brought up 2026-11-16):
#   1) Kernel: CONFIG_V4L2_FM_DRIVER=y, drivers/bluetooth/broadcom/v4l2_fm_driver/
#      -> exposes /dev/radio0 (V4L2 radio device)
#   2) libfmradio.v4l2-fm.so - the V4L2 backend, opens /dev/radio0 and is loaded
#      by libfmjni via dlopen (installed to /system/lib so LIBRARY_PATH resolves it)
#   3) libfmjni.so - JNI layer registering com/android/fmradio/FmNative for the
#      FMRadio app's System.loadLibrary("fmjni") (installed to /system/lib)
#   4) brcm-uim-sysfs - switches the BCM4354 from BT HCI mode to UIM/FM mode by
#      writing the bcm_ldisc sysfs node (installed to /system/bin, started by init)
#   5) FMRadio - the FM app shipped with the ROM (packages/apps/FMRadio)
#
# Root causes of the earlier breakage (all resolved):
#   - libfmjni was defined in Android.mk while FMRadio's jni_libs is a Soong property,
#     so the Make module was invisible -> "FMRadio depends on undefined module libfmjni"
#   -> converted to libfmjni/Android.bp (see the header of that file)
#   - the MTK build of libfmjni bundled with FMRadio is incompatible with this chip
#   -> BoardConfig.mk sets BOARD_HAVE_MTK_FM := true to disable that namespace
PRODUCT_PACKAGES += \
    libfmjni \
    libfmradio.v4l2-fm \
    brcm-uim-sysfs \
    FMRadio

# Graphics
PRODUCT_AAPT_CONFIG += xlarge large
TARGET_SCREEN_HEIGHT := 2048
TARGET_SCREEN_WIDTH := 1536
TARGET_TEGRA_VERSION := t124

PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator@2.0-impl \
    android.hardware.graphics.allocator@2.0-service \
    android.hardware.graphics.composer@2.1-service \
    android.hardware.graphics.mapper@2.0-impl-2.1 \
    android.hardware.renderscript@1.0-impl

# Shims
# FIX NOTE: add libnvomxadaptor_shim, which supplies the old-ABI GraphicBuffer symbols
# missing from the NVIDIA hardware OMX decoding adaptor (preloaded via TARGET_LD_SHIM_LIBS).
# libsensors_fastsys_shim: short-circuits the pointless system("rm -rf /sys/...") fork
# inside the sensor blob. Measured: sensorservice readiness drops from 5.7s to about 2s,
# which also shortens the synchronous wait in DeviceStateManagerService (boot-speed work).
PRODUCT_PACKAGES += \
    libs \
    libshim_zw \
    libshim_atomic \
    libshims_ui \
    libnvomxadaptor_shim \
    libsensors_fastsys_shim

# HIDL
PRODUCT_ENFORCE_VINTF_MANIFEST_OVERRIDE := true
PRODUCT_PACKAGES += \
    hwservicemanager \
    android.hidl.allocator@1.0-service \
    android.hidl.base@1.0 \
    android.hidl.manager@1.0 \
    android.hidl.manager-V1.0-java \

# Binder
PRODUCT_PACKAGES += \
    libhidltransport \
    libhwbinder

# Gatekeeper
PRODUCT_PACKAGES += \
    android.hardware.gatekeeper@1.0-service.software

# Healthd
PRODUCT_PACKAGES += \
    android.hardware.health@2.1-impl \
    android.hardware.health@2.1-impl.recovery \
    android.hardware.health@2.1-service

# Key layouts
PRODUCT_PACKAGES += \
    tegra-kbc.kl \
    Vendor_0955_Product_7210.kl

# Keymaster
PRODUCT_PACKAGES += \
    android.hardware.keymaster@4.1-service

# Light
PRODUCT_PACKAGES += \
	android.hardware.light@2.0-impl \
    android.hardware.light@2.0-service

# Media_omx config
PRODUCT_PACKAGES += \
    android.hardware.media.omx@1.0-service \
    libstagefrighthw

# Media nvidia
PRODUCT_COPY_FILES += \
    frameworks/av/media/libstagefright/data/media_codecs_google_audio.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_audio.xml \
    frameworks/av/media/libstagefright/data/media_codecs_google_video.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_google_video.xml \
    $(LOCAL_PATH)/media/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    $(LOCAL_PATH)/media/media_profiles_V1_0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_profiles_V1_0.xml \
    $(LOCAL_PATH)/media/media_codecs_performance.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_performance.xml

# Memtrack
PRODUCT_PACKAGES += \
	android.hardware.memtrack@1.0-impl \
    android.hardware.memtrack@1.0-service

# Modules
PRODUCT_COPY_FILES += \
	$(LOCAL_PATH)/prebuilt/modules/baseband_usb_chr.ko:system/lib/modules/baseband_usb_chr.ko \
	$(LOCAL_PATH)/prebuilt/modules/gspca_main.ko:system/lib/modules/gspca_main.ko \
	$(LOCAL_PATH)/prebuilt/modules/mbt8897.ko:system/lib/modules/mbt8897.ko \
	$(LOCAL_PATH)/prebuilt/modules/raw_ip_net.ko:system/lib/modules/raw_ip_net.ko \
	$(LOCAL_PATH)/prebuilt/modules/sd8897.ko:system/lib/modules/sd8897.ko \
	$(LOCAL_PATH)/prebuilt/modules/sd8897mlan.ko:system/lib/modules/sd8897mlan.ko \
	$(LOCAL_PATH)/prebuilt/modules/tcrypt.ko:system/lib/modules/tcrypt.ko \
	$(LOCAL_PATH)/prebuilt/modules/modules.alias:system/lib/modules/modules.alias \
	$(LOCAL_PATH)/prebuilt/modules/modules.dep:system/lib/modules/modules.dep \
	$(LOCAL_PATH)/prebuilt/modules/modules.softdep:system/lib/modules/modules.softdep

# Net
PRODUCT_PACKAGES += \
    android.system.net.netd@1.0 \
    libandroid_net \
    libnl \
    netutils-wrapper-1.0

# NVIDIA
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/permissions/com.nvidia.blakemanager.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nvidia.blakemanager.xml \
    $(LOCAL_PATH)/permissions/com.nvidia.feature.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nvidia.feature.xml \
    $(LOCAL_PATH)/permissions/com.nvidia.feature.opengl4.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nvidia.feature.opengl4.xml \
    $(LOCAL_PATH)/permissions/com.nvidia.nvsi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nvidia.nvsi.xml

# Enable nvidia framework enhancements if available
-include vendor/lineage/product/nvidia.mk

# OMX(SOFTWARE)
PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.c2-poolmask=0x80000 \
    debug.stagefright.ccodec=0

# Overlay
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay 

# Partitions
$(call inherit-product, $(SRC_TARGET_DIR)/product/non_ab_device.mk)

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.audio.low_latency.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.low_latency.xml \
    frameworks/native/data/etc/android.hardware.audio.output.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.output.xml \
    frameworks/native/data/etc/android.hardware.audio.pro.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.audio.pro.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml \
    frameworks/native/data/etc/android.hardware.camera.autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.camera.full.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.full.xml \
    frameworks/native/data/etc/android.hardware.camera.raw.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.raw.xml \
    frameworks/native/data/etc/android.hardware.ethernet.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.ethernet.xml \
    frameworks/native/data/etc/android.hardware.faketouch.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.faketouch.xml \
    frameworks/native/data/etc/android.hardware.location.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.xml \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    frameworks/native/data/etc/android.hardware.opengles.aep.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.opengles.aep.xml \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
    frameworks/native/data/etc/android.software.vulkan.deqp.level-2020-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.vulkan.deqp.level.xml \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.distinct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.distinct.xml \
    frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/handheld_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/handheld_core_hardware.xml \
    frameworks/native/data/etc/tablet_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/tablet_core_hardware.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepcounter.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.stepcounter.xml \
    frameworks/native/data/etc/android.hardware.sensor.stepdetector.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.stepdetector.xml \
    frameworks/native/data/etc/android.software.freeform_window_management.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.freeform_window_management.xml \
    $(LOCAL_PATH)/permissions/privapp-permissions-google.xml:system/etc/permissions/privapp-permissions-google.xml \
    $(LOCAL_PATH)/permissions/GoogleExtServices_permissions.xml:system/etc/permissions/GoogleExtServices_permissions.xml

# Product
PRODUCT_CHARACTERISTICS := tablet

# PHS
PRODUCT_PACKAGES += \
    nvphsd.conf

# Power
PRODUCT_PACKAGES += \
    android.hardware.power@1.0-impl \
    android.hardware.power@1.0-service \
    android.hardware.power.stats@1.0-service.mock

# Ramdisk
PRODUCT_PACKAGES += \
    fstab.tn8 \
    init.cal.rc \
    init.comms.rc \
    init.hdcp.rc \
    init.t124.rc \
    init.tegra.rc \
    init.tlk.rc \
    init.tn8.rc \
    init.tn8.usb.rc \
    init.tn8_common.rc \
    init.ussrd.rc \
    power.tn8.rc \
    ueventd.tn8.rc

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.usb.accessory.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.accessory.xml \
    frameworks/native/data/etc/android.hardware.usb.host.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.usb.host.xml \
    frameworks/native/data/etc/android.software.app_widgets.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.app_widgets.xml

# Seccomp
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/init.renderer.sh:$(TARGET_COPY_OUT_VENDOR)/bin/init.renderer.sh \
    $(LOCAL_PATH)/initfiles/ussr_setup.sh:/system/bin/ussr_setup.sh

# adb self-check script: after boot, verifies the adbd USB transport and reboots into
# recovery (TWRP) on failure, so a failed fix does not require manual key presses to get logs.
# Triggered by the adb_selfcheck service in init.tegra.rc once sys.boot_completed=1.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/adb_selfcheck.sh:$(TARGET_COPY_OUT_VENDOR)/bin/adb_selfcheck.sh

# Sensors
PRODUCT_PACKAGES += \
    android.hardware.sensors@1.0-impl \
    sensors.tegra

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/sensors/_hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/_hals.conf

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += device/xiaomi/mocha

# Thermal
PRODUCT_PACKAGES += \
    android.hardware.thermal@1.0-impl \
    android.hardware.thermal@1.0-service \
    thermalhal.tn8.xml

# TimeKeep
PRODUCT_PACKAGES += \
    timekeep \
    TimeKeep

# USB HAL
PRODUCT_PACKAGES += \
    android.hardware.usb@1.3-service.basic

# Use legacy ADB USB support
PRODUCT_PROPERTY_OVERRIDES += \
    ro.adb.nonblocking_ffs=false

# Vibrator
PRODUCT_PACKAGES += \
	android.hardware.vibrator@1.0-impl \
    android.hardware.vibrator@1.0-service \
    android.hardware.vibrator@1.3

# Vendor seccomp policy files for media components:
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/seccomp/mediacodec-seccomp.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediacodec-seccomp.policy \
    $(LOCAL_PATH)/seccomp/mediaextractor-seccomp.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediaextractor-seccomp.policy

# VNDK
PRODUCT_TARGET_VNDK_VERSION := 34
VNDK_SP_LIBRARIES := \
    com.android.vndk.current

# Wifi
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/wifi/dhcpcd.conf:$(TARGET_COPY_OUT_VENDOR)/etc/dhcpcd/dhcpcd.conf

# Wifi (All Shield devices xurrently use broadcom wifi / bluetooth modules)
$(call inherit-product-if-exists, hardware/broadcom/wlan/bcmdhd/config/config-bcm.mk)
$(call inherit-product-if-exists, hardware/broadcom/wlan/bcmdhd/firmware/bcm4354/device-bcm.mk)

PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service.legacy \
    hostapd \
    conn_init \
    wificond \
    libwifi-hal-bcm \
    libwpa_client \
    wpa_supplicant \
    wpa_supplicant.conf

# ==== libwifi-hal must ship to the vendor partition as a prebuilt (measured 2026-11-16) ====
# Symptom: the mtime of /system/vendor/lib/libwifi-hal.so always stays at the last full build;
#   after editing sources under frameworks/opt/net/wifi or hardware/broadcom/wlan, mka
#   APEX variant（out/.../apex/com.android.hardware.wifi/lib/libwifi-hal.so），
#   only rebuilds the APEX copy while the file in the vendor partition never changes.
#
# Root cause: on A14 libwifi-hal is APEX-only (the cc_library_shared "libwifi-hal" in
#   frameworks/opt/net/wifi/libwifi_hal/Android.bp only produces an APEX variant), so Soong
#   never generates a vendor variant. The wifi HAL here is the vendor binary extracted
#   from stock, /vendor/bin/hw/android.hardware.wifi@1.0-service.legacy, and ldd clearly
#   shows it loads /system/vendor/lib/libwifi-hal.so - so it always runs that stale copy
#   and source changes never take effect. PRODUCT_PACKAGES += libwifi-hal does not help
#   (measured): it only installs the APEX variant into the APEX, never into vendor.
#
# Hence PRODUCT_COPY_FILES is used here to copy the built .so into the vendor partition.
# [MAINTENANCE] prebuilts/libwifi-hal.so is a snapshot. After changing
#   hardware/broadcom/wlan/bcmdhd/wifi_hal/ (especially sanitize/cflags of libwifi-hal-bcm)
#   or frameworks/opt/net/wifi/libwifi_hal/, you must run:
#       m libwifi-hal -j16
#       cp out/target/product/mocha/apex/com.android.hardware.wifi/lib/libwifi-hal.so \
#          device/xiaomi/mocha/prebuilts/libwifi-hal.so
#   otherwise the vendor partition keeps the old binary.
#
# Why this had to be changed: wifi_cleanup() in the vendor libwifi-hal dereferences a
#   stale callback pointer, so the wifi HAL aborts during stop. Without the guard in
#   WifiHalHidlImpl the framework then throws an NPE on a null IWifi (FATAL EXCEPTION IN SYSTEM PROCESS) and the boot animation replays.
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/libwifi-hal.so:$(TARGET_COPY_OUT_VENDOR)/lib/libwifi-hal.so

# ==== Static single-frame boot image (measured on the device 2026-11-16) ====
# Historically hwcomposer.tegra.so crashed reliably on the first presentDisplay after a
# cold start of SurfaceFlinger (gralloc buffer handle UAF; disassembly showed
# ldr r0,[r5,#0x100] loading the base of an already munmap'ed page). composer died ->
#    on property:init.svc.surfaceflinger=running -> start bootanim
# SF restarted -> init relaunched the boot animation, 2-3 rounds per boot, which the user
#
# saw as an endless boot animation. That crash was a side effect of system_server restarting
# (fixed by guarding null IWifi in WifiHalHidlImpl), but a still image also keeps boot quiet:
# a single-frame animation plus "p 1 0 part0" in desc.txt (play once and hold) means every
# restart of bootanim shows the same still frame instead of jumping back to the start.
# This also covers any future composer restart caused by something else.
#
# [MAINTENANCE] To restore the original animation, drop this PRODUCT_COPY_FILES entry;
#   to use your own boot image, replace prebuilts/bootanimation.zip (must be stored, not deflated).
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/bootanimation.zip:$(TARGET_COPY_OUT_SYSTEM)/media/bootanimation.zip

# webview uses-library
PRODUCT_USES_LIBRARIES += androidx.window.extensions
PRODUCT_OPTIONAL_USES_LIBRARIES += androidx.window.extensions
PRODUCT_PACKAGES += androidx.window.extensions
PRODUCT_BROKEN_VERIFY_USES_LIBRARIES := true
