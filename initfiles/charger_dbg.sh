#!/system/bin/sh
#
# Charger-mode evidence collector.
#
# Charger mode runs only early-init/init/charger, so `on fs` never happens and
# /data, /cache and logd are all absent: there is no logcat and no boot log to
# read afterwards. init.tn8_common.rc mounts CAC on /cache and runs this script
# from `on charger`; the file it leaves behind is the only record of what the
# charger actually did.
#
# Two stages on purpose: the early one is written before the UI has had a
# chance to fail, so even a charger that dies immediately leaves something.

OUT=/cache/charger_dbg.txt

dump_early() {
    echo "=== charger mode, stage 1 (t+0s) ==="
    echo "date: $(date)"
    echo "bootmode: $(getprop ro.bootmode) / $(getprop ro.boot.mode)"
    echo "hardware: $(getprop ro.boot.hardware) / $(getprop ro.hardware)"
    echo "boot_completed: $(getprop sys.boot_completed)"
    echo
    echo "--- service managers ---"
    for s in servicemanager hwservicemanager vndservicemanager; do
        echo "$s: $(getprop init.svc.$s)"
    done
    echo "health-hal-2-1: $(getprop init.svc.health-hal-2-1)"
    echo "charger: $(getprop init.svc.charger)"
    echo
    echo "--- framebuffer ---"
    ls -l /dev/graphics/ 2>&1
    for f in virtual_size stride bits_per_pixel blank modes name; do
        echo "fb0/$f: $(cat /sys/class/graphics/fb0/$f 2>&1)"
    done
    echo
    echo "--- minui / charger props ---"
    getprop | grep -iE "minui|charger" 2>&1
}

dump_late() {
    echo
    echo "=== charger mode, stage 2 (t+15s) ==="
    echo "date: $(date)"
    echo
    echo "--- processes ---"
    ps -A 2>&1
    echo
    echo "--- health service ---"
    service list 2>&1 | grep -i health
    echo
    echo "--- battery nodes ---"
    for d in /sys/class/power_supply/*; do
        [ -d "$d" ] || continue
        echo "$(basename "$d"): type=$(cat "$d/type" 2>&1) cap=$(cat "$d/capacity" 2>&1) status=$(cat "$d/status" 2>&1)"
    done
    echo
    echo "--- charger log lines from kmsg ---"
    dmesg 2>&1 | grep -iE "charger|minui|healthd|health|fb0|tegra_fb|batt" | tail -80
    echo
    echo "--- full kmsg tail ---"
    dmesg 2>&1 | tail -120
}

{
    dump_early
} > "$OUT" 2>&1

sleep 15

{
    dump_late
} >> "$OUT" 2>&1

# Keep collecting: a charger that only misbehaves after the first half minute
# (blank timeout, suspend) still leaves a trail.
sleep 45
{
    echo
    echo "=== charger mode, stage 3 (t+60s) ==="
    echo "date: $(date)"
    echo "charger: $(getprop init.svc.charger)"
    echo "health-hal-2-1: $(getprop init.svc.health-hal-2-1)"
    echo
    echo "--- kmsg since stage 2 ---"
    dmesg 2>&1 | grep -iE "charger|minui|healthd|health|fb0|batt" | tail -60
} >> "$OUT" 2>&1
