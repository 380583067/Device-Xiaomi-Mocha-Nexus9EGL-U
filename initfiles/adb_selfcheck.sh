#!/vendor/bin/sh
#
# adb_selfcheck.sh - 开机后自动检测 adb 是否正常，异常则重启到 recovery(TWRP)
#
# 背景：mocha 是 legacy g_android FunctionFS 设备（3.10 内核），Android 14 的
# adbd(APEX com.android.adbd) 对 legacy transport 支持不完整，偶发 adbd 建立
# functionfs 后 transport 读线程未正常启动，导致 host 端 adb 一直显示 offline。
#
# 此脚本在开机后延迟运行，检测 adbd 是否与 host 成功建立连接。若系统已启动
# 完成但 adbd 始终停留在 "registering usb transport" 阶段（无任何后续进展），
# 则判定 offline，自动重启到 recovery，避免手动按键进 TWRP 拉日志。
#
# 检测原理：
#   正常连接时，adbd 在 "registering usb transport" 之后会继续推进：
#     - 收到 host 的 A_CNXN，transport 从 kCsOffline -> kCsDevice；
#     - 收到 framework 授权连接（adbd_auth: received a new framework connection）；
#     - 收到 host 授权（adb client <id> authorized）。
#   offline 时，adbd 停在 "registering usb transport" 之后便无任何后续日志。
#
# 判断信号：取 logcat 中 adbd 的最后一条日志，若它仍是
# "registering usb transport"（或更早的 functionfs 初始化日志），说明 adbd 在
# transport 注册后毫无进展，判定 offline。

SELFCHECK_DELAY="${SELFCHECK_DELAY:-90}"   # 总延迟（给系统启动+host 枚举留时间）
GRACE_PERIOD="${GRACE_PERIOD:-20}"         # 额外等待窗口

# 【2026-11-17 重大修正】默认不再自动 reboot recovery。
#
# 原逻辑有致命误判：它把"adbd 最后一条日志停在 registering usb transport"当成
# offline，但**只要 host 端没有 adb 连接，adbd 本来就不会打印后续日志**。
# 结果：设备插在没装 adb 的 USB 口上、或用户根本没插线、或 host adb server
# 刚好没跑，110 秒后一律 reboot recovery —— 表现为"开机动画循环 / 莫名进 TWRP"，
# 用户侧看到的就是 adb offline + reboot 无效。
#
# 现在默认只探测并写日志（log -t adb_selfcheck / /cache/adb_selfcheck.log），
# 不改变系统状态。确实需要"自动兜底进 TWRP"时，手动置：
#     setprop persist.mocha.adb_autoreboot 1
# 这样自动重启变成显式选择，不会再自己把设备踢进 recovery。
AUTO_REBOOT="$(getprop persist.mocha.adb_autoreboot)"

do_reboot() {
    if [ "${AUTO_REBOOT}" = "1" ]; then
        log -t adb_selfcheck "$1 -> rebooting recovery (persist.mocha.adb_autoreboot=1)"
        reboot recovery
    else
        log -t adb_selfcheck "$1 -> would reboot recovery, but autoreboot disabled"
    fi
}

sleep "${SELFCHECK_DELAY}"

# 系统是否已启动完成
if [ "$(getprop sys.boot_completed)" != "1" ]; then
    log -t adb_selfcheck "boot not completed, skip"
    exit 0
fi

# adbd 是否存活 —— 这是唯一可靠的"硬故障"信号（进程不存在）
if ! pidof adbd > /dev/null 2>&1; then
    do_reboot "adbd not running"
    exit 1
fi

# functionfs 端点是否就绪 —— adbd 进程活着但 ffs 没挂上，才算真的异常
if [ ! -e /dev/usb-ffs/adb/ep1 ]; then
    log -t adb_selfcheck "WARN: /dev/usb-ffs/adb/ep1 missing (host may not be attached)"
fi

# 额外等待窗口后再检查一次（给 host 端 adb 重新枚举留时间）
sleep "${GRACE_PERIOD}"

# 取 adbd 的最后一条关键日志，只用于诊断记录，不再据此重启。
# 排除 SELinux audit 噪音，只看 adbd 自身的 INFO 日志。
last_adbd_log=$(logcat -d -b all 2>/dev/null \
    | grep -E "^[0-9-]+ [0-9:.]+ +[0-9]+ +[0-9]+ I adbd" \
    | tail -1)

log -t adb_selfcheck "last adbd log: ${last_adbd_log}"

# 若 adbd 最后一条日志仍是 functionfs 初始化/transport 注册阶段，说明还没有
# host 连接（不代表故障），只记录不动作。
if echo "${last_adbd_log}" | grep -qE "initializing functionfs|opening control endpoint|functionfs successfully initialized|registering usb transport"; then
    log -t adb_selfcheck "adbd waiting for host (no post-registration progress) - state $(getprop sys.usb.state)"
    exit 0
fi

# adbd 有后续进展（framework 连接 / host 授权等），判定正常
log -t adb_selfcheck "adbd has progressed beyond transport registration, adb healthy"
exit 0
