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
# Timing matters more than detail here. The first run of this script only wrote
# its first stage: with ro.charger.enable_suspend=true the charger calls
# autosuspend_enable() as soon as the animation has played once (even if the
# panel never showed it), and every sleeping process freezes with it. So the
# kmsg reads below are packed into the first ten seconds, one every two
# seconds, instead of a single read at +15 s.

OUT=/cache/charger_dbg.txt

dump_kmsg() {
    echo "--- kmsg (charger|minui|health|fb|batt) ---"
    dmesg 2>&1 | grep -iE "charger|minui|healthd|health-|fb0|tegra_fb|batt|drm" | tail -60
}

dump_state() {
    echo "svc: charger=$(getprop init.svc.charger) health=$(getprop init.svc.health-hal-2-1) sm=$(getprop init.svc.servicemanager) hwsm=$(getprop init.svc.hwservicemanager)"
    echo "battery: $(for d in /sys/class/power_supply/*; do [ -d "$d" ] && echo -n "$(basename $d) cap=$(cat $d/capacity 2>&1) st=$(cat $d/status 2>&1); "; done)"
}

# Two questions that answer each other. If fb0 holds drawing, the charger did
# paint and the problem is downstream of the buffer (display controller); if it
# is all zeroes, the charger never painted - and then wchan and the fd list say
# what it was waiting on instead.
dump_probe() {
    pid=$(getprop init.svc_debug_pid.charger)
    echo "--- charger proc (pid=$pid) ---"
    if [ -n "$pid" ]; then
        echo "wchan: $(cat /proc/$pid/wchan 2>&1)"
        echo "state: $(awk '{print $3}' /proc/$pid/stat 2>/dev/null)"
        ls -l /proc/$pid/fd 2>&1 | head -14
    else
        echo "no charger pid"
    fi
    echo "--- fb0 holds drawing? (non-zero bytes in first 32KB) ---"
    dd if=/dev/graphics/fb0 bs=4096 count=8 2>/dev/null | tr -d '\000' | wc -c
    echo "--- fb0 first 48 bytes ---"
    dd if=/dev/graphics/fb0 bs=48 count=1 2>/dev/null | od -An -tx1
}

{
    echo "=== stage 1 (t+0s) $(date) ==="
    dump_state
    echo "--- framebuffer ---"
    ls -l /dev/graphics/ 2>&1
    for f in virtual_size stride bits_per_pixel blank modes name; do
        echo "fb0/$f: $(cat /sys/class/graphics/fb0/$f 2>&1)"
    done
    dump_probe
    echo "--- full kmsg tail ---"
    dmesg 2>&1 | tail -60
    dump_kmsg
} > "$OUT" 2>&1

# Suspend can arrive within a few seconds, so sample early rather than late.
i=0
while [ $i -lt 4 ]; do
    sleep 2
    i=$((i + 1))
    {
        echo
        echo "=== stage 2.$i (t+$((i * 2))s) $(date) ==="
        dump_state
        dump_probe
        dump_kmsg
    } >> "$OUT" 2>&1
done

# Control test, but only if the charger is already gone: if it is alive it may
# well be drawing, and painting over it would destroy the very thing we are
# trying to prove. A dead charger plus noise on screen means the fb path works
# and the fault was upstream of it; a dead charger plus no noise means the DC is
# not scanning this buffer at all.
if [ "$(getprop init.svc.charger)" != "running" ]; then
    {
        echo
        echo "=== stage 3 (fb0 write test, charger is not running) $(date) ==="
        echo "frame_bytes=$((1536 * 2048 * 4))"
        dd if=/dev/urandom of=/dev/graphics/fb0 bs=4096 count=3072 2>&1
        echo "dd rc=$?"
    } >> "$OUT" 2>&1

    sleep 5
fi

# Whatever is still visible now is the tail end: if the noise is gone the
# charger is drawing over it (and the panel works), if it is still there the
# charger is not drawing.
{
    echo
    echo "=== stage 4 (t+~15s, after fb0 test) $(date) ==="
    dump_state
    dump_probe
    dump_kmsg
    echo "--- full kmsg tail ---"
    dmesg 2>&1 | tail -80
} >> "$OUT" 2>&1
