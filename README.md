# Device configuration for XiaoMi MiPad Tablet

## Spec Sheet
| Feature                 | Specification                     |
| :---------------------- | :-------------------------------- |
| CPU                     | Quad Core 2.2GHz                  |
| Chipset                 | NVIDIA® Tegra K1 T124             |
| GPU                     | NVIDIA® GK20A (Kepler)            |
| Memory                  | 2GB RAM                           |
| Shipped Android Version | 4.4.2                             |
| Storage                 | 16/32/64GB                        |
| MicroSD                 | Up to 128GB                       |
| Battery                 | 5197 mAh                          |
| Dimentions              | 221 x 126 x 9.2 mm                |
| Display                 | 1536 x 2048 pixels                |
| Release Date            | July 29, 2014                     |

## Device Picture
![Nvidia SHIELD Tablet ](http://shield.nvidia.co.uk/images/home-page-sections/shield-tablet-controller-header-image.png "Nvidia SHIELD Tablet")

## LineageOS 21 (Android 14) port

This tree is the Android 14 bring-up of the Mi Pad 1 (`mocha`, NVIDIA Tegra K1
T124 / TN8). It needs three repositories, all of them on the
`lineageos-21-test` branch:

| Path in the source tree | Repository | Notes |
| :---------------------- | :--------- | :---- |
| `device/xiaomi/mocha` | https://github.com/380583067/Device-Xiaomi-Mocha-Nexus9EGL-U | this tree: board config, HALs, init scripts, patches |
| `kernel/xiaomi/mocha` | https://github.com/380583067/mocha-R21.8 | 3.10 Tegra kernel; branch is based on `R21.8-A11+` |
| `vendor/xiaomi/mocha` | https://github.com/380583067/vendor_xiaomi_mocha-nexus9EGL | extracted blobs, based on `lineage-21.0-ul` |

`local_manifest.xml` fragment:

```xml
<project name="380583067/Device-Xiaomi-Mocha-Nexus9EGL-U"
         path="device/xiaomi/mocha" revision="lineageos-21-test" />
<project name="380583067/mocha-R21.8"
         path="kernel/xiaomi/mocha" revision="lineageos-21-test" />
<project name="380583067/vendor_xiaomi_mocha-nexus9EGL"
         path="vendor/xiaomi/mocha" revision="lineageos-21-test" />
```

### Repositories that must be patched

LineageOS source changes cannot live in this repository, so every change outside
`device/`, `kernel/` and `vendor/` is carried as a patch under `patches/` and
applied by `patches/install.sh`. The patch series touches these projects, which
therefore have to be present in the source tree at the matching LineageOS 21
revision:

```
build/make
frameworks/av
frameworks/base
frameworks/native
hardware/interfaces
lineage-sdk
packages/apps/Settings
packages/apps/Trebuchet
packages/modules/adb
packages/modules/Connectivity
packages/modules/IntentResolver
packages/modules/Wifi
packages/services/Mms
prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9
system/core
vendor/lineage
```

Check and apply them with:

```bash
device/xiaomi/mocha/patches/check.sh      # validate the whole series
device/xiaomi/mocha/patches/install.sh    # apply it
device/xiaomi/mocha/patches/uninstall.sh  # revert it
```

All 23 patches apply cleanly, in sorted order, to a pristine checkout of the
LineageOS 21 tree this device targets. See `patches/README` for the list of
patches that were dropped because their change is already upstream.

### Build

```bash
source build/envsetup.sh
lunch lineage_mocha-ap2a-userdebug
mka bacon -j16
```

The `system` partition has been resized to 2.86 GB (`mmcblk0p26`). The bootloader
still hardcodes the original 640 MB size and rejects large images through
`fastboot`, so flash the OTA package from TWRP instead of using `fastboot flash
system`.

## Copyright

```
#
# Copyright (C) 2015 The CyanogenMod Project
# Copyright (C) 2017 HighwayStar
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
```
