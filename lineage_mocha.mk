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
