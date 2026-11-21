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

# 【2026-11-18 修复，影响录屏/录像】必须显式带上 vndservicemanager。
#
# 本 product 是 system-as-root、没有独立 vendor 分区，从来没有 inherit
# base_vendor.mk，而 vndservicemanager 只在 base_vendor 里（见
# build/make/target/product/base_vendor.mk:95）。结果 /vendor/bin 下没有这个
# 二进制，init.rc 的 `start vndservicemanager` 每次都失败，/dev/vndbinder 上
# 没有任何 service manager 在监听。
#
# 后果（真机实证）：任何 vendor 进程只要走了 AIDL/vndbinder 就会永久阻塞。
#   media.codec 里 GraphicBufferMapper 先试 Gralloc5（AIDL）路径，
#   AServiceManager_isDeclared() 这一句就要去问 vndservicemanager，
#   拿不到 context object 就每秒重试一次、永不返回
#   （日志：Waiting 1s on context object on /dev/vndbinder），
#   于是 BufferQueueProducer::dequeueBuffer 卡死 —— 编码器输入 Surface 一帧
#   都拿不到，screenrecord 显示 "recorded 0 frames"，相机录像同样 0 帧。
# 补上这个包后 Gralloc5 探测会立刻失败并回退到 HDFIDL/Gralloc2 老路径。
PRODUCT_PACKAGES += \
    vndservicemanager

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
# 修复说明：原厂 NVIDIA 相机 blob（camera.vendor.tegra.so）在运行时读取
# /system/etc/nvcamera.conf 和 /system/etc/model_frontal.xml（绝对路径，见 blob 源码
# NvCameraHal），原先复制到 /vendor/etc/ 导致 blob 读不到配置、相机初始化失败。
# 改为复制到 system/etc/。参考 Dargons10/Device-Xiaomi-Mocha-Nexus9EGL-S。
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/camera/nvcamera.conf:system/etc/nvcamera.conf \
    $(LOCAL_PATH)/camera/model_frontal.xml:system/etc/model_frontal.xml

# 注：相机 wrapper（camera.tegra）为设备树原始方案（CameraWrapper + Camera3Wrapper
# 包装原厂 blob），该方案已随自研 V4L2 HAL3 上线删除源码（见 git 历史）。
#
# 修复说明（相机设备数 = 0）：
#   原厂 blob camera.vendor.tegra.so 在 Android 14 上 dlopen 失败，缺 2 个 DT_NEEDED
#   文件 + 9 个 Android C++ ABI 符号。补齐方案见 camera/Android.mk 与
#   camera/mocha_camera_shim.cpp 头部注释。这里把 3 个兜底库打进镜像：
#     libcameraservice_mocha     -> 生成 libcameraservice_mocha.so，
#                                   SONAME 声明为 libcameraservice.so（空 stub）
#     libpowerservice_client     -> PowerServiceClient 构造 + sendPowerHint
#     libmocha_camera_shim       -> CameraMetadata/Fence/SensorManager ABI 转发
#     libbeautify                -> 顶替原厂美颜库（原厂库依赖带 TEXTREL 的
#                                   libFaceProc.so，Android 14 拒绝加载）
# 其中 libmocha_camera_shim 由 BoardConfig.mk 的 TARGET_LD_SHIM_LIBS 预加载。
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
# 完整链路（2026-11-16 打通）：
#   1) 内核：CONFIG_V4L2_FM_DRIVER=y，drivers/bluetooth/broadcom/v4l2_fm_driver/
#      → 提供 /dev/radio0（V4L2 radio 设备）
#   2) libfmradio.v4l2-fm.so —— V4L2 底层实现，open("/dev/radio0")，
#      由 libfmjni 通过 dlopen 加载（装 /system/lib，与 LIBRARY_PATH 一致）
#   3) libfmjni.so —— JNI 层，注册 com/android/fmradio/FmNative，
#      供 FMRadio app 的 System.loadLibrary("fmjni") 使用（装 /system/lib）
#   4) brcm-uim-sysfs —— 把 BCM4354 从 BT HCI 模式切到 UIM/FM 模式，
#      写 bcm_ldisc 的 sysfs 节点（装 /system/bin，由 init 启动）
#   5) FMRadio —— ROM 自带的 FM 应用（packages/apps/FMRadio）
#
# 之前不可用的根因（已解决）：
#   - libfmjni 原先用 Android.mk 定义，而 FMRadio 的 jni_libs 是 Soong 属性，
#     解析不到 Make 模块 → "FMRadio depends on undefined module libfmjni"
#   → 已转成 libfmjni/Android.bp（见该文件头部说明）
#   - FMRadio 自带的 MTK 版 libfmjni 与本芯片不兼容
#   → BoardConfig.mk 设 BOARD_HAVE_MTK_FM := true 关闭其 namespace
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
# 修复说明：新增 libnvomxadaptor_shim，为 NVIDIA 硬件 OMX 解码 adaptor 提供缺失的
# GraphicBuffer 旧 ABI 符号（配合 BoardConfig TARGET_LD_SHIM_LIBS 预加载）。
# libsensors_fastsys_shim：短路传感器 blob 里 system("rm -rf /sys/...") 的
# 无意义 fork，实测可把 sensorservice 就绪时间从 5.7 秒压到 2 秒多，
# 连带缩短 DeviceStateManagerService 的同步等待（开机速度专项）。
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

# 软解：默认走 Codec2（c2.android.*），OMX 仅作后备
#   debug.stagefright.ccodec 取值（见 frameworks/av/media/codec2/sfplugin/
#   Codec2InfoBuilder.cpp）：0=完全禁用 C2；1=只优先 c2.android.* 音频；
#   2=c2.android.* 全部可用并排在最前，其它 .avc.decoder/.avc.encoder 可用
#   但排最后；3=c2.android.* 排最前，其它按默认；4=全部按默认。
#   本树不设该属性时默认是 4，而 OMX 默认 rank 0x100 高于 c2.android.* 的
#   0x200，实际会选中 OMX.google.*.avc.decoder。这里取 2：软解默认用 C2，
#   同时保留 OMX 作为末尾后备（万一某个流 C2 软解不吃，OMX 仍能被选中）。
#   前置条件：frameworks/av 的 C2AllocatorGralloc 老 gralloc 适配补丁（
#   lockYCbCr 缺失时回退 lock()、YV12 走 Y/V/U 平面、CPU_WRITE+YUV 补
#   HW_TEXTURE）。没有这些补丁 C2 软解会直接 start() 失败或颜色红绿互换。
#   C2 软解实测 720p 约 29.3~29.5fps，与 OMX 持平，颜色逐帧一致。
#   c2-poolmask：软件 C2 输出 buffer 池的 usage 掩码，沿用原值。
PRODUCT_PROPERTY_OVERRIDES += \
    debug.stagefright.c2-poolmask=0x80000 \
    debug.stagefright.ccodec=1

# 【2026-11-21】ccodec=1 而不是 2：C2 软解的"解码到 Surface 播放"路径还是黑的
# （解码本身正常、帧已提交到 SurfaceView 层（SF frame 计数在走），但 GPU 合成出来
# 全黑 —— C2AllocatorGralloc 的 lock() fallback 在 tegra gralloc0 的 blocklinear
# buffer 上，CPU 写入没落到 GPU 可见的 backing store；OMX 的 SoftwareRenderer
# fallback（0042 补丁）同一 gralloc 路径是好的。所以视频解码先用 OMX 软解，
# 音频继续走 C2；等 C2→Surface 路径修好（对照 0042 的 lock/flush 语义）再切回 2。

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
# Vulkan：采用 Smoke(lineage-19.1) 的配套 blob 隔离方案（2026-11-16）：
#   1) 原厂 vulkan.tegra.so(Android24) 与 libglcore.so(20M) 版本错配，函数表结构
#      不一致导致 CreateInstance 空指针崩溃（SEGV_MAPERR addr=0x4）。
#   2) 改用 Smoke 配套对：vulkan.tegra.so(Android28) + libglcore.so(11M)，并把
#      libglcore.so 复制为 libvkcore.so（SONAME 等长改名），vulkan shim 里
#      dlopen("libglcore.so") 等长改成 dlopen("libvkcore.so")，让 Vulkan 走独立副本，
#      EGL 继续用原 libglcore.so，物理隔离、互不干扰。
#   3) 非 Treble 板 linker namespace 只有 /vendor/lib 不含 /vendor/lib/hw，故在
#      /vendor/lib/ 额外放一份 vulkan.tegra.so（见 mocha-vendor-blobs.mk）。
# 恢复 3 个 Vulkan feature 声明（照抄 Smoke device.mk）。
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
# 换成 AIDL power HAL（arttttt 那棵 19.1 树的实现，Power/Profiles/BoostPulse/
# GpuFloor/CameraHints），顶掉"通用 HIDL 包装 + NVIDIA blob（power.tegra.so）"的
# 组合。blob 没法审计，而且它的 sysfs 写入在被修好 DAC 权限之前一直是静默失败的；
# 新 HAL 同样负责调频，还带性能档位（power.mocha.rc）、GPU floor 和相机提示。
# power.stats@1.0-service.mock 暂时保留，还没有替代实现。
# blob 本体已从 proprietary-files.txt / vendor 仓库删除（那个包装服务是唯一使用者）。
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

# 开机向导跳过脚本：由 init.tn8.rc 里的 mocha_provision oneshot 服务在
# sys.boot_completed=1 时执行，把 device_provisioned / user_setup_complete
# 两个标志都写上，避免卡在设置向导里。
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

# ==== libwifi-hal 必须以 prebuilt 形式装进 vendor 分区（2026-11-16 真机实测）====
# 现象：/system/vendor/lib/libwifi-hal.so 的 mtime 永远是"上一次完整编译"的时间，
#   改了 frameworks/opt/net/wifi 或 hardware/broadcom/wlan 的源码后，mka 只重编
#   APEX variant（out/.../apex/com.android.hardware.wifi/lib/libwifi-hal.so），
#   而 vendor 分区那份纹丝不动。
#
# 根因：A14 的 libwifi-hal 已经 APEX 化（frameworks/opt/net/wifi/libwifi_hal/Android.bp
#   的 cc_library_shared "libwifi-hal" 只产出 APEX variant），Soong 根本不会为它
#   生成 vendor variant。而本机的 wifi HAL 是原厂提取的 vendor 二进制
#   /vendor/bin/hw/android.hardware.wifi@1.0-service.legacy，ldd 明确显示它加载的是
#   /system/vendor/lib/libwifi-hal.so —— 于是它永远跑那份历史残留的旧代码，
#   源码改动 100% 不生效。加 PRODUCT_PACKAGES += libwifi-hal 也没用（实测无效），
#   因为它只会把 APEX variant 装进 APEX，不碰 vendor。
#
# 因此这里用 PRODUCT_COPY_FILES 直接把编好的 so 复制到 vendor 分区。
# 【维护提示】prebuilts/libwifi-hal.so 是一个"快照"，改动
#   hardware/broadcom/wlan/bcmdhd/wifi_hal/（尤其 libwifi-hal-bcm 的 sanitize/cflags）
#   或 frameworks/opt/net/wifi/libwifi_hal/ 之后，必须执行：
#       m libwifi-hal -j16
#       cp out/target/product/mocha/apex/com.android.hardware.wifi/lib/libwifi-hal.so \
#          device/xiaomi/mocha/prebuilts/libwifi-hal.so
#   否则 vendor 里仍是旧二进制。
#
# 本机为何必须改它：libwifi-hal-bcm 缺 sanitize.cfi 导致 wifi_cleanup 的 CFI 检查
#   必然失败 → wifi HAL abort → system_server 的 WifiHalHidlImpl.registerHalCallback
#   拿到 null IWifi 抛 NPE（FATAL EXCEPTION IN SYSTEM PROCESS）→ 开机动画重放。
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/libwifi-hal.so:$(TARGET_COPY_OUT_VENDOR)/lib/libwifi-hal.so

# ==== 静态单帧开机画面（2026-11-16 真机实测）====
# 本机 hwcomposer.tegra.so 在 SurfaceFlinger 冷启动第一次 presentDisplay 时会稳定崩溃
# 一次（gralloc buffer handle UAF，反汇编确认是 ldr r0,[r5,#0x100] 取出已 munmap 的
# 页基址后解引用）。composer 崩 → SF 重启 → init 的
#    on property:init.svc.surfaceflinger=running -> start bootanim
# 又会把开机动画重新拉起，实测开机阶段重复 2~3 轮，用户看到的是"开机动画一直在循环"。
#
# 崩溃点位于 prebuilt blob 内部（gralloc/hwcomposer 均无源码，且 NvGrDecompressBuffer
# 不是动态符号引用，无法用 LD_SHIM_LIBS 劫持），用户态无法根治。
# 但"观感上的循环"可以消除：换成单帧动画 + desc.txt 的 "p 1 0 part0"（播一次停住），
# 这样即使 bootanim 被 init 重启多次，每一轮显示的都是同一张静止画面，
# 不会出现"动画从头再播一遍"的跳变。
#
# 【维护提示】若要恢复原版动画，删掉本条 PRODUCT_COPY_FILES 即可；
#   若要换成自己的开机画面，直接替换 prebuilts/bootanimation.zip（需 store 压缩）。
PRODUCT_COPY_FILES += \
    device/xiaomi/mocha/prebuilts/bootanimation.zip:$(TARGET_COPY_OUT_SYSTEM)/media/bootanimation.zip

# webview uses-library
PRODUCT_USES_LIBRARIES += androidx.window.extensions
PRODUCT_OPTIONAL_USES_LIBRARIES += androidx.window.extensions
PRODUCT_PACKAGES += androidx.window.extensions
PRODUCT_BROKEN_VERIFY_USES_LIBRARIES := true
