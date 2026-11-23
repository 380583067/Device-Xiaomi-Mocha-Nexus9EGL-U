# Fix adb offline (critical): LineageOS provides WITH_ADB_INSECURE and
# vendor/lineage/config/common.mk turns it into ro.adb.secure=0 (no authorisation).
# With an empty /data/misc/adb/adb_keys, ro.adb.secure=1 makes adbd demand
# authorisation and the host stays offline during the handshake. Setting it to true
# allows unauthenticated root debugging. Must be defined before inheriting common.mk.
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

# [2026-11-23] DeviceAsWebcam is gone at the source, see
# patches/build/make/0002-drop-DeviceAsWebcam-from-handheld_system.patch
#
# Do NOT add $(filter-out DeviceAsWebcam,$(PRODUCT_PACKAGES)) - it does not work.
# Why (build/make/core/product.mk:495):
#   $(eval $(v) := $($(v)) $(INHERIT_TAG)$(np))
# inherit-product appends an "@inherit:<path>" token to PRODUCT_PACKAGES and the
# real module names only show up later during import-nodes, so a filter-out in a
# .mk file matches the token and never the module name. This AOSP release has no
# PRODUCT_PACKAGES_REMOVE; the only official way to drop an inherited module is
# LOCAL_OVERRIDES_PACKAGES (main.mk:1217 module-overrides, declared by another
# installed module), which is less obvious than fixing the source, hence the patch.
