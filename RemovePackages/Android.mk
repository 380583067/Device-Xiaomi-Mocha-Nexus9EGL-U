#
# Copyright (C) 2026 The LineageOS Project
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

LOCAL_PATH := $(call my-dir)

# Packages the inherited product lists ship but this board cannot use.
# Make cannot take a package back out of an inherited PRODUCT_PACKAGES, so this
# module - which installs nothing - overrides them instead.
#
# TeleService: there is no modem on this tablet (ro.radio.noril=yes, no SIM
# slot, no voice/data). generic_no_telephony.mk still pulls TeleService in
# through handheld_system.mk, and the resulting com.android.phone process runs
# as user "radio" for the whole uptime (~80MB RSS and a permanent background
# service) while every telephony caller already takes its no-telephony path.
# Telecom (android.server.telecom) is a separate package, runs inside
# system_server and is kept: in-app calls and the audio route still use it.
include $(CLEAR_VARS)
LOCAL_MODULE := RemovePackages
LOCAL_MODULE_CLASS := APPS
LOCAL_MODULE_TAGS := optional
LOCAL_OVERRIDES_PACKAGES := \
    TeleService
LOCAL_UNINSTALLABLE_MODULE := true
LOCAL_CERTIFICATE := PRESIGNED
LOCAL_SRC_FILES := /dev/null
include $(BUILD_PREBUILT)
