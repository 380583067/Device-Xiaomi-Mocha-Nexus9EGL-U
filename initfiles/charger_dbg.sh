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

{
    echo "=== stage 1 (t+0s) $(date) ==="
    dump_state
    echo "--- framebuffer ---"
    ls -l /dev/graphics/ 2>&1
    for f in virtual_size stride bits_per_pixel blank modes name; do
        echo "fb0/$f: $(cat /sys/class/graphics/fb0/$f 2>&1)"
    done
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
        dump_kmsg
    } >> "$OUT" 2>&1
done

# Decisive test: if the charger is alive but the panel still shows the
# bootloader logo, there are two very different explanations - the charger never
# drew, or it drew and fb0 does not reach the panel. Writing a frame straight
# into fb0 tells them apart: the screen either fills with noise (the fb path
# works, so the problem is in minui/charger) or it does not (the display
# controller is not scanning this buffer out at all).
{
    echo
    echo "=== stage 3 (fb0 write test) $(date) ==="
    echo "charger=$(getprop init.svc.charger) frame_bytes=$((1536 * 2048 * 4))"
    dd if=/dev/urandom of=/dev/graphics/fb0 bs=4096 count=3072 2>&1
    echo "dd rc=$?"
} >> "$OUT" 2>&1

sleep 5

# Whatever is still visible now is the tail end: if the noise is gone the
# charger is drawing over it (and the panel works), if it is still there the
# charger is not drawing.
{
    echo
    echo "=== stage 4 (t+~15s, after fb0 test) $(date) ==="
    dump_state
    dump_kmsg
    echo "--- full kmsg tail ---"
    dmesg 2>&1 | tail -80
} >> "$OUT" 2>&1
