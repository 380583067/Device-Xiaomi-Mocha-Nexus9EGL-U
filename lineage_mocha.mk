# FIX for adb offline (critical): LineageOS provides a WITH_ADB_INSECURE switch,
# and vendor/lineage/config/common.mk sets ro.adb.secure=0 accordingly (no
# authorisation required). When /data/misc/adb/adb_keys is empty on the device,
# ro.adb.secure=1 makes adbd demand authorisation, so the host side hangs in the
# handshake and reports offline. Setting this to true disables authorisation and
# allows direct root debugging.
# Must be defined before inheriting common.mk.
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
