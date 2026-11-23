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

# [2026-11-18 fix, affects screen recording] vndservicemanager must be listed explicitly.
#
# This product is system-as-root with no separate vendor partition and never
# inherits base_vendor.mk, the only place vndservicemanager lives
# (build/make/target/product/base_vendor.mk:95). /vendor/bin therefore lacked the
# binary, init.rc's `start vndservicemanager` failed on every boot and nothing was
# listening on /dev/vndbinder.
#
# Consequence (measured): any vendor process touching AIDL/vndbinder blocks forever.
# In media.codec GraphicBufferMapper tries the Gralloc5 (AIDL) path first and
# AServiceManager_isDeclared() asks vndservicemanager; without a context object it
# retries every second and never returns ("Waiting 1s on context object on
# /dev/vndbinder"), so BufferQueueProducer::dequeueBuffer hangs, the encoder input
# Surface never gets a frame and screenrecord reports "recorded 0 frames" (camera
# recording likewise produced nothing). With this package the Gralloc5 probe fails
# fast and falls back to the HDFIDL/Gralloc2 path.
PRODUCT_PACKAGES += \
    vndservicemanager

# [2026-11-23] OpenCamera and the KernelSU manager are not preset; install them
# after boot. The original-signature APKs collide with the build's preset pipeline
# (re-signing breaks the KernelSU check, PRESIGNED strips v2, do-not-alter trips the
# compressed-dex check - see vendor proprietary/Android.mk). The APKs live in
# vendor/xiaomi/mocha/proprietary/app/.

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
    bluetooth.profile.hid.device.enabled=true \
    bluetooth.profile.hfp.ag.enabled=true \
    bluetooth.profile.opp.enabled=true \
    bluetooth.profile.pan.nap.enabled=true \
    bluetooth.profile.pan.panu.enabled=true \
    bluetooth.profile.pbap.server.enabled=true \
    bluetooth.profile.map.server.enabled=true \
    bluetooth.profile.bap.broadcast.assist.enabled=false \
    bluetooth.profile.bap.unicast.client.enabled=false \
    bluetooth.profile.bas.client.enabled=false \
    bluetooth.profile.ccp.server.enabled=false \
    bluetooth.profile.hap.client.enabled=false \
    bluetooth.profile.csip.set_coordinator.enabled=false \
    bluetooth.profile.vcp.controller.enabled=false \
	persist.bluetooth.turn_off_bt_main_thread=true \
    ro.bluetooth.request.master=true \
    ro.bluetooth.remote.autoconnect=true \
    bluetooth.a2dp.sink.enabled=false

# Runtime hygiene: two things today's debugging tripped over.
#
# use_freezer=false - the cached-app freezer needs a working freezer cgroup,
# and this kernel cannot provide one: it is 3.10.108, which predates cgroup v2
# entirely (v2 arrived in 3.16), and /dev/freezer is not mounted either, so
# /sys/fs/cgroup/uid_X creation fails on every process start. AMS's
# isFreezerSupported() already fails on the missing /dev/freezer, so freezing
# never really engages - the property makes that state explicit instead of
# accidental. The real fix is the cgroup v2 backport from SmokeR24.1-kernel
# stable (same 3.10.108 base, see the merge plan in .workbuddy notes); until
# that lands, do not let AMS half-freeze apps.
#
# persist.logd.size - the default logd buffer is 256 KiB and gets overwritten
# within minutes, which cost us the scene of a mediaserver lockup today. 4 MiB
# keeps a full boot plus some debugging around.
PRODUCT_PROPERTY_OVERRIDES += \
    persist.device_config.activity_manager_native_boot.use_freezer=false \
    persist.logd.size=4M

# Camera
# Fix: the vendor NVIDIA camera blob (camera.vendor.tegra.so) reads
# /system/etc/nvcamera.conf and /system/etc/model_frontal.xml by absolute path (see
# the blob's NvCameraHal), so copying them to /vendor/etc/ left the blob without
# config and camera init failed. Copy them to system/etc/ instead, same as
# Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/nvcamera.conf:system/etc/nvcamera.conf \
    $(LOCAL_PATH)/camera/model_frontal.xml:system/etc/model_frontal.xml

# Note: the camera wrapper (camera.tegra) was the original approach (CameraWrapper +
# Camera3Wrapper around the vendor blob). Its sources were removed when the in-house
# V4L2 HAL3 landed (see git history).
#
# Fix (camera count = 0): the vendor blob camera.vendor.tegra.so fails to dlopen on
# Android 14, missing 2 DT_NEEDED files and 9 Android C++ ABI symbols. The shim
# approach is described in camera/Android.mk and the header of
# camera/mocha_camera_shim.cpp. These fallback libraries go into the image:
#     libcameraservice_mocha     -> builds libcameraservice_mocha.so with SONAME
#                                   libcameraservice.so (empty stub)
#     libpowerservice_client     -> PowerServiceClient ctor + sendPowerHint
#     libmocha_camera_shim       -> CameraMetadata/Fence/SensorManager ABI forwarding
#     libbeautify                -> replaces the vendor beautify library (that one
#                                   depends on libFaceProc.so with TEXTREL, which
#                                   Android 14 refuses to load)
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

# Charger-mode evidence collector: charger mode has no logcat and no /data, so
# what it writes to /cache/charger_dbg.txt is the only record (see the file).
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/charger_dbg.sh:system/bin/charger_dbg.sh

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
# Full chain (working since 2026-11-16):
#   1) kernel: CONFIG_V4L2_FM_DRIVER=y, drivers/bluetooth/broadcom/v4l2_fm_driver/
#      -> provides /dev/radio0 (V4L2 radio device)
#   2) libfmradio.v4l2-fm.so - the V4L2 backend, open("/dev/radio0"), dlopen'ed by
#      libfmjni (installed in /system/lib to match LIBRARY_PATH)
#   3) libfmjni.so - JNI layer registering com/android/fmradio/FmNative for the
#      FMRadio app's System.loadLibrary("fmjni") (/system/lib)
#   4) brcm-uim-sysfs - switches the BCM4354 from BT HCI to UIM/FM mode by writing
#      the bcm_ldisc sysfs nodes (/system/bin, started by init)
#   5) FMRadio - the ROM's own FM app (packages/apps/FMRadio)
#
# Why it did not work before (solved):
#   - libfmjni was an Android.mk module while FMRadio's jni_libs is a Soong
#     property, so it did not resolve: "FMRadio depends on undefined module libfmjni"
#   -> converted to libfmjni/Android.bp (see that file's header)
#   - FMRadio ships an MTK libfmjni that is incompatible with this chip
#   -> BoardConfig.mk sets BOARD_HAVE_MTK_FM := true to drop its namespace
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
    android.hardware.renderscript@1.0-impl \
    gralloc.tegra

# Shims
# Fix: add libnvomxadaptor_shim, providing the old GraphicBuffer ABI symbols the
# NVIDIA hardware OMX adaptor needs (preloaded via TARGET_LD_SHIM_LIBS).
# libsensors_fastsys_shim short-circuits the sensor blob's pointless
# system("rm -rf /sys/...") fork; measured, that cuts sensorservice readiness from
# 5.7 s to a bit over 2 s and shortens DeviceStateManagerService's wait accordingly.
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

# Codec selection: debug.stagefright.ccodec=0 disables Codec2 entirely, so all
# software decoding runs through OMX.google.* inside media.codec, using ACodec's
# SoftwareRenderer (patched for this board's YV12 layout). Both this and the
# Codec2 path played 1080p H.264 cleanly once patched; Codec2 must stay OFF for
# a harder reason:
#
#   media.swcodec hosts c2.android.* and its decoder threads map the output
#   gralloc buffers through tegra's NvGrLock. When a player's process dies
#   mid-playback - MX Player does this by design, racing two engines in
#   short-lived child processes - NvGrLock ends up waiting on an internal
#   mutex that no live thread owns. Every later decoder in that process then
#   blocks in NvGrLock forever (17 stuck component loopers measured), the
#   components' release() never returns, MediaPlayerService::getCodecList
#   stops answering, and every NEW media process - MX itself, Glimpse, VLC,
#   anything - hangs at MediaCodecList init until media.swcodec and
#   mediaserver are killed. Killing both restores a working stack with no
#   further damage, but with ccodec=4 the wedge returns on the next player
#   that exits mid-decode. Verified across repeated runs 2026-11-24.
#
# History of the Codec2 era (kept because the c2-poolmask property below is
# still honoured if ccodec is ever raised again):
#
# c2-poolmask must include BUFFERQUEUE: 0xF0000 = ION|GRALLOC|BUFFERQUEUE|BLOB.
# BLOB-only (0x80000) left the decoder output blocks without a bqId, so under BLAST
# attachBuffer failed every frame with INVALID_OPERATION(-38) and playback had sound
# but no picture - it also wedged mediaserver's NPDecoder, taking every MediaPlayer
# down with it.
#
# The ccodec=4 ranking era used two properties OmxInfoBuilder reads directly
# (frameworks/av/media/libstagefright/OmxInfoBuilder.cpp:141):
#
#   omx_default_rank           default for hardware OMX, 0x100
#   omx_default_rank.sw-other  default for OMX.google video, 0x210
#
# and by the Codec2 software rank, 0x200. Raising the hardware OMX rank to 768
# puts the NVIDIA decoders behind Codec2, and dropping the OMX.google video rank
# to 100 puts them in front of it:
#
#   OMX.google video (100) -> c2.android (512) -> OMX.Nvidia (768)
#   OMX.google audio (16, its own property) stays first for audio.
#
# A warning if the ranking is ever "fixed" through media_codecs.xml instead: the
# rank attribute there is parsed with sscanf("%u%c"), i.e. decimal only.
# rank="0x300" silently fails to parse and the component keeps its default rank -
# exactly how the first attempt at this ended up changing nothing.
PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.c2-poolmask=0xF0000 \
    debug.stagefright.ccodec=0 \
    debug.stagefright.omx_default_rank=768 \
    debug.stagefright.omx_default_rank.sw-other=100
PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.c2-poolmask=0xF0000 \
    debug.stagefright.ccodec=4 \
    debug.stagefright.omx_default_rank=768 \
    debug.stagefright.omx_default_rank.sw-other=100

# [mocha 2026-11-21] 4 = every component keeps its normal rank, so OMX (0x100)
# outranks Codec2 software (0x200) and OMX is tried first - which is what the
# device is meant to ship with: OMX first, Codec2 second, and both always
# present so a stream that one of them chokes on still plays on the other.
#
# That only works because the NVIDIA OMX decoders are pushed to rank 0x300 in
# media/media_codecs.xml. They cannot initialise on this kernel (no
# /dev/nvhost-msdec, and the 2014 blob predates the Android 14 ACodec buffer
# ABI), and at their default 0x100 they would win the race and leave the
# SurfaceView black with CodecException 0x80001000 - the bug fixed on the 13:10
# build by forcing ccodec=2. Ranking them last keeps them listed (some streams
# may still want them, and hiding them hides real failures) while making
# OMX.google.* the first choice.
#
# Resulting order per MIME: OMX.google.* (0x100) -> c2.android.* (0x200) ->
# OMX.Nvidia.* (0x300).

# History, because both conclusions in it still matter.
#
# ccodec=1 was the original value and left video black: option 1 ranks only the
# c2.android.* *audio* codecs first, so video still landed on the NVIDIA OMX
# decoders. Those cannot initialise on this kernel (no /dev/nvhost-msdec, and
# the 2014 blob predates the Android 14 ACodec buffer ABI), failing with
# CodecException 0x80001000: the SurfaceView stays black while the progress bar
# keeps running, which looked like a compositor bug for a long time.
#
# Forcing ccodec=2 fixed that by ranking all c2.android.* first. A 16-clip matrix
# (H.264 720p-4K, HEVC 8-bit 720p/1080p, VP9, VP8, MPEG-4 Part 2 in mp4/mkv/avi,
# 3GP, interlaced, anamorphic, non-16-aligned, AC3-in-MKV, H.264 in MPEG-TS)
# rendered correctly with it, encoder side included (screenrecord ->
# c2.android.avc.encoder, playable).
#
# It is 4 now, for the ordering described above: the same protection is provided
# by ranking the NVIDIA decoders last in media/media_codecs.xml instead of by
# suppressing Codec2, so OMX can still be tried first.
#
# Still unsupported for other reasons: HEVC Main10 (no 10-bit decoder at all)
# and MPEG-2 (no software decoder in this build, the NVIDIA one fails).
#
# The earlier claim "C2 software video output to a Surface is always black" was
# a misdiagnosis - the black layers were NVIDIA decoders being selected; C2
# decode to a SurfaceView composites fine under the forced GPU composition.

# Packages the inherited product lists ship but this board cannot use; see
# RemovePackages/Android.mk for which one and why.
PRODUCT_PACKAGES += \
    RemovePackages

# Overlay
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

# Partitions
$(call inherit-product, $(SRC_TARGET_DIR)/product/non_ab_device.mk)

# Permissions
# Vulkan: use Smoke's (lineage-19.1) matched blob set with isolation (2026-11-16):
#   1) The stock vulkan.tegra.so (Android24) and libglcore.so (20 MB) are mismatched,
#      their dispatch tables differ and CreateInstance crashes on a null pointer
#      (SEGV_MAPERR addr=0x4).
#   2) Smoke's matched pair - vulkan.tegra.so (Android28) + libglcore.so (11 MB) - is
#      used instead, with libglcore.so copied to libvkcore.so (same-length SONAME
#      rename) and the vulkan shim's dlopen("libglcore.so") rewritten to
#      dlopen("libvkcore.so"), so Vulkan runs on its own copy while EGL keeps the
#      original libglcore.so: physically isolated, no interference.
#   3) On this non-Treble board the linker namespace only has /vendor/lib, not
#      /vendor/lib/hw, so an extra copy of vulkan.tegra.so goes in /vendor/lib/
#      (see mocha-vendor-blobs.mk).
# Restores the three Vulkan feature declarations (copied from Smoke's device.mk).
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute-0.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_0_3.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
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
# Switch to the AIDL power HAL (from arttttt's 19.1 tree: Power/Profiles/BoostPulse/
# GpuFloor/CameraHints), replacing "generic HIDL wrapper + NVIDIA blob
# (power.tegra.so)". The blob cannot be audited and its sysfs writes silently failed
# before the DAC permissions were fixed; the new HAL also does frequency scaling and
# adds performance profiles (power.mocha.rc), a GPU floor and camera hints.
# power.stats@1.0-service.mock stays for now, there is no replacement yet.
# The blob itself is gone from proprietary-files.txt and the vendor repo (that
# wrapper service was its only user).
PRODUCT_PACKAGES += \
    android.hardware.power-service.mocha \
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
    power.mocha.rc \
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

# Setup-wizard skip script: run by the mocha_provision oneshot service in
# init.tn8.rc once sys.boot_completed=1, it sets both device_provisioned and
# user_setup_complete so the device does not sit in the setup wizard.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/mocha_provision.sh:$(TARGET_COPY_OUT_VENDOR)/bin/mocha_provision.sh

# Performance profiles (power.mocha.rc) and the userspace thermal guard.
# The guard owns scaling_max_freq: profile ceiling from persist.mocha.perf_profile,
# clamped further when CPU-therm crosses persist.mocha.thermal.threshold. The
# kernel itself only throttles at 84 C, which is too late for a fanless tablet.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/initfiles/power.mocha.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/power.mocha.rc \
    $(LOCAL_PATH)/initfiles/mocha_thermal.sh:$(TARGET_COPY_OUT_VENDOR)/bin/mocha_thermal.sh

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

# Vibrator: board HAL (HIDL 1.3, DRV2604 over /sys/class/timed_output/vibrator
# and /sys/vibrator/pwmvalue). Replaces the generic 1.0 impl, which only knows
# on/off and cannot play patterns or set strength.
PRODUCT_PACKAGES += \
    android.hardware.vibrator@1.3 \
    android.hardware.vibrator@1.3-service.mocha

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

# ==== libwifi-hal must ship as a prebuilt in the vendor partition (measured 2026-11-16) ====
# Symptom: /system/vendor/lib/libwifi-hal.so always had the mtime of the last full
#   build; after editing frameworks/opt/net/wifi or hardware/broadcom/wlan, mka only
#   rebuilt the APEX variant (out/.../apex/com.android.hardware.wifi/lib/libwifi-hal.so)
#   and the vendor copy never changed.
#
# Root cause: on A14 libwifi-hal is APEX-only (the cc_library_shared "libwifi-hal" in
#   frameworks/opt/net/wifi/libwifi_hal/Android.bp produces just an APEX variant), so
#   Soong never builds a vendor variant. This device's WiFi HAL is the extracted
#   vendor binary /vendor/bin/hw/android.hardware.wifi@1.0-service.legacy, and ldd
#   shows it loads /system/vendor/lib/libwifi-hal.so - so it always ran the stale
#   leftover binary and source changes had zero effect. Adding
#   PRODUCT_PACKAGES += libwifi-hal does not help either (measured): it only installs
#   the APEX variant into the APEX, never into vendor.
#
# So PRODUCT_COPY_FILES copies the built .so into the vendor partition directly.
# [maintenance] prebuilts/libwifi-hal.so is a snapshot. After touching
#   hardware/broadcom/wlan/bcmdhd/wifi_hal/ (especially libwifi-hal-bcm's
#   sanitize/cflags) or frameworks/opt/net/wifi/libwifi_hal/, run:
#       m libwifi-hal -j16
#       cp out/target/product/mocha/apex/com.android.hardware.wifi/lib/libwifi-hal.so \\
#          device/xiaomi/mocha/prebuilts/libwifi-hal.so
#   otherwise vendor keeps the old binary.
#
# Why this device must change it: libwifi-hal-bcm lacks sanitize.cfi, so the CFI check
#   in wifi_cleanup always fails -> the WiFi HAL aborts -> system_server's
#   WifiHalHidlImpl.registerHalCallback gets a null IWifi and throws NPE
#   (FATAL EXCEPTION IN SYSTEM PROCESS) -> the boot animation replays.
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/libwifi-hal.so:$(TARGET_COPY_OUT_VENDOR)/lib/libwifi-hal.so

# ==== Static single-frame boot screen (measured 2026-11-16) ====
# On this device hwcomposer.tegra.so reliably crashes once on SurfaceFlinger's first
# presentDisplay after a cold start (gralloc buffer handle UAF; disassembly shows
# ldr r0,[r5,#0x100] loading the base of an already munmap'ed page and dereferencing
# it). The composer dies -> SF restarts -> init's
#    on property:init.svc.surfaceflinger=running -> start bootanim
# starts the boot animation again, twice or three times per boot, which the user sees
# as "the animation loops forever".
#
# The crash is inside the prebuilt blobs (no sources for gralloc/hwcomposer, and
# NvGrDecompressBuffer is not a dynamic symbol reference so LD_SHIM_LIBS cannot
# hijack it), so it cannot be fixed in user space.
# What can be removed is the visible loop: a single-frame animation with "p 1 0 part0"
# in desc.txt (play once and hold) means every restart shows the same still frame
# instead of jumping back to the start.
#
# [maintenance] To restore the original animation, drop this PRODUCT_COPY_FILES entry;
#   to use your own image, replace prebuilts/bootanimation.zip (store compression).
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/bootanimation.zip:$(TARGET_COPY_OUT_SYSTEM)/media/bootanimation.zip

# webview uses-library
PRODUCT_USES_LIBRARIES += androidx.window.extensions
PRODUCT_OPTIONAL_USES_LIBRARIES += androidx.window.extensions
PRODUCT_PACKAGES += androidx.window.extensions
PRODUCT_BROKEN_VERIFY_USES_LIBRARIES := true
