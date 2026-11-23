# 修复 adb offline（关键）：LineageOS 官方提供 WITH_ADB_INSECURE 开关，
# vendor/lineage/config/common.mk 会据此设置 ro.adb.secure=0（免授权）。
# 设备上 /data/misc/adb/adb_keys 为空时，ro.adb.secure=1 会让 adbd 要求授权，
# host 端 adb 卡在授权握手、显示 offline。设为 true 即免授权、允许直接 root 调试。
# 必须在 inherit common.mk 之前定义。
WITH_ADB_INSECURE := true

# Inherit device configuration for mocha.
# Inherit some common lineage stuff.
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/languages_full.mk)
$(call inherit-product, vendor/lineage/config/common_full_tablet_wifionly.mk)

# Inherit from xiaomi device
$(call inherit-product, device/xiaomi/mocha/mocha.mk)

# Product Information
PRODUCT_NAME := lineage_mocha
PRODUCT_DEVICE := mocha
PRODUCT_BRAND := Xiaomi
PRODUCT_MANUFACTURER := Xiaomi
PRODUCT_MODEL := MI PAD

# GMS Client ID
PRODUCT_GMS_CLIENTID_BASE := android-xiaomi

# Include firmware
$(call inherit-product-if-exists, vendor/xiaomi-firmware/mocha/firmware.mk)

# 【2026-11-23】DeviceAsWebcam 已从源头移除，见
# patches/build/make/0002-drop-DeviceAsWebcam-from-handheld_system.patch
#
# 这里**不要**再写 $(filter-out DeviceAsWebcam,$(PRODUCT_PACKAGES))——实测无效。
# 原因（build/make/core/product.mk:495）：
#   $(eval $(v) := $($(v)) $(INHERIT_TAG)$(np))
# inherit-product 往 PRODUCT_PACKAGES 里塞的是 "@inherit:文件路径" 令牌，
# 真正的模块名要到后续 import-nodes 阶段才递归展开。所以在任何 .mk 里
# 对 PRODUCT_PACKAGES 做 filter-out，看到的只有路径令牌，永远匹配不到模块名。
# 本版本 AOSP 没有 PRODUCT_PACKAGES_REMOVE；唯一能"删除"已继承模块的官方机制是
# LOCAL_OVERRIDES_PACKAGES（main.mk:1217 module-overrides，需由另一个已安装模块声明），
# 语义上不如直接改源头清晰，故采用 patch 方式。
