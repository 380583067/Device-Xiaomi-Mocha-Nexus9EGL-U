## v21.0-20261126c（终版 ROM，全量 clean 重编）

本版本取代此前全部 Release（v21.0-20261125 / 20261126 / 20261126b），完整升级日志见文末。

### 本版核心更新
1. **短视频 App 崩溃修复（红果短剧等）**：NVIDIA GL 驱动 shader JIT 缓存页被内核
   回收导致 SIGSEGV；cache 目录改为 tmpfs，JIT 页可压入 zram 恢复
   （实测连续刷剧零崩溃，基线 60-90 秒必崩）
2. **SELinux enforcing 全量修复**：odrefresh/相机/FM/USB 共享/TF/电池/温控/BT 扫描
   全链路零拒绝；新增 SYSTEM_EXT 策略目录与 mocha_cache_tmpfs 专用域
3. **FM 扬声器修复**：Mono DAC MIX DAC L1 直入扬声器功放（耳机+扬声器同时出声）
4. **蓝牙扫描列表修复**：发现设备按 MAC 显示（远端不开可发现时的降级显示）
5. 全量 clean 重编（135,249 个目标，3 小时），提交链 9c24c99da20 全部同步 GitHub

### 刷入
TWRP 刷入 lineage_mocha-ota.zip（765,046,466 字节，
md5 68579eb5abc7942ec1dcc8528430e079）

---

## 升级日志（历史版本存档）

LineageOS 21.0 for Xiaomi Mocha (Mi Pad 1) — 2026-11-26b (camera round 2)

Focus / tap-to-focus:
- ANDROID_CONTROL_AF_REGIONS implemented: tap anywhere and the contrast
  sweep only measures the tapped window (MAX_REGIONS AF was declared 0,
  so apps never sent regions at all)
- AF sweep moved to a worker thread: tapping no longer freezes the
  viewfinder for 2-3 s; lens no longer parks at infinity between taps
- fixed a thread race that crashed cameraserver on rapid double taps
- AF_STATE now reported in results

Viewfinder smoothness:
- preview sessions run the capture at 720p and upscale: ~14 fps -> ~30 fps
  measured; stills still use the full 8 MP pipeline, recordings keep their
  stream size
- removed a duplicated warm-up loop that added ~1 s of shutter lag

Colour (front camera green cast):
- AWB now uses the brightest decile (real white objects) as its reference
  in bright scenes too: white objects measure exactly neutral (R/G=1.00)
  instead of G>R by 5%, which read as "the whole picture is green"

EXIF:
- landscape stills now carry EXIF Orientation 1 instead of no tag
- note: OpenCamera's HDR mode rewrites EXIF itself (Orientation 0) - use
  normal mode for correct orientation metadata

Base: v21.0-20261126 (includes all earlier fixes: 720p recording, focus
carry-over, CMA-backed capture buffer).

md5: 9abcae83e5eefe44379215675356a566


---
LineageOS 21.0 for Xiaomi Mocha (Mi Pad 1) — 2026-11-26

Camera bugfix build:
- Still photos: EXIF orientation tag written (photos no longer rotated)
- Still photos: VCM focus carried across the 8 MP pipeline switch (no more blurry shots)
- Recording: 720p clips no longer black (encoder surface written with the real gralloc stride)
- Recording: AF sweep now runs in video mode; tap-to-focus works during recording
- Kernel: the 8 MP capture buffer is CMA-backed — no more TPG checkerboard fallback at early boot
- Charger animation / health HAL / TWRP-in-OTA included as in v21.0-20261125

Known issues:
- Front camera still underexposed in dark scenes (sensor sensitivity, AE already maxed)
- Tap-to-focus area selection (ANDROID_CONTROL_AF_REGIONS) not implemented yet
- Portrait EXIF orientation should be verified with the device held upright

md5: f58599749eafde648027a999b56c63b6


---
## 2026-11-25 构建（已验证可开机）

### 修复
- **前摄曝光极低**：ov5693 驱动把曝光值直接写进 24 位积分时间寄存器，而 OV5693 该字段是 1/16 行单位，导致 HAL 请求 2500 行实际只积分约 156 行。已改为 ×16 并按帧长 VTS-8 钳制（超帧长会让传感器停流，VI 超时后回退 TPG）。
- **前摄偏绿**：demosaic 的 pos_color 表里没有正确的 BGGR 项（pat1/pat3 都是 RGGB），前摄被当成 GRBG 解马赛克，R/B 互换。已重写表并按 SBGGR10 选 BGGR；黑电平按传感器分开（IMX179=16，OV5693=12）。
- **后摄对焦模糊**：AF 对比度扫描只覆盖 AD5823 全行程 0~1023 中的 140~640，近物永远够不到焦点。已改为全量程步长 64 + ±64 精扫；并修复 AD5823 上电后首次写位置必失败（-121）的时序问题。
- **关机充电只有灯没有动画**：health HAL 2.1 上游是 `vendor:true`，本设备无 vendor 镜像，服务从未安装，charger 拿不到电池数据。已改为装进 system 并在 device manifest 中声明。
- **关机充电熄屏后唤不醒**：充电模式持有内核 wakelock、关闭 autosleep，CPU0 单核常驻，熄屏后短按电源键直接回到充电动画。

### 集成
- OTA 包内附带 TWRP 3.7.0_9-0，刷完整包时写入 SOS（recovery）分区，与 TOS/misc/USP 固件一起刷入。

### 刷入
TWRP 中直接安装本 zip；或 `adb sideload`。首次开机约 2~4 分钟（设备较慢，请耐心等待，不要误判为卡死）。

MD5: `40df1d5cd5dc3a4d76ad040856f0ac6a`

