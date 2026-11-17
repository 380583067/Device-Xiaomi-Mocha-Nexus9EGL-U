#!/system/bin/sh
# 【开机向导跳过 / 兜底 2026-11-17】让这台平板不弹设置向导、直接进桌面。
#
# 主修复在设备树 overlay 里（SettingsProvider 的 def_device_provisioned /
# def_user_setup_complete 默认值改成 true），首次开机建库时就直接是"已初始化"，
# 没有竞态。
#
# 这个脚本只处理主修复覆盖不到的情况：userdata 里 SettingsProvider 的库已经
# 存在（默认值只在建库时生效），此时把两个标志补写进去。
#
# 顺带停用 com.android.messaging：本机 ro.radio.noril=yes，没有电话/短信能力。
# 原先是 `on property:ro.radio.noril=yes` + `exec pm disable`，那个时机
# system_server 还没起来，pm 必然失败，所以挪到这里。
#
# 由 init.tn8.rc 的 mocha_provision oneshot 服务在 sys.boot_completed=1 时拉起。

TAG="mocha_provision"

i=0
ok=0
while [ "$i" -lt 20 ]; do
    settings put global device_provisioned 1 >/dev/null 2>&1
    settings put secure user_setup_complete 1 >/dev/null 2>&1

    # 不信任 put 的返回码，直接回读确认 SettingsProvider 真的落库了。
    dp=$(settings get global device_provisioned 2>/dev/null)
    usc=$(settings get secure user_setup_complete 2>/dev/null)
    if [ "$dp" = "1" ] && [ "$usc" = "1" ]; then
        echo "$TAG: provisioned (attempt $i)"
        ok=1
        break
    fi

    i=$((i + 1))
    sleep 1
done

if [ "$ok" != "1" ]; then
    echo "$TAG: settings provider never became ready, last=[$dp][$usc]" >&2
fi

# 停用短信应用（无 RIL，装上也用不了）。失败不影响上层逻辑，只留一行日志。
pm disable com.android.messaging >/dev/null 2>&1 ||
    echo "$TAG: pm disable com.android.messaging failed (package missing?)" >&2

exit 0
