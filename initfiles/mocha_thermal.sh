#!/system/bin/sh
#
# Userspace thermal guard for mocha (Tegra K1).
#
# Why this exists: the kernel only starts passive throttling at a junction
# temperature of 84 C (CPU-therm trip2) and the SoC happily sits at 2.2 GHz until
# then - measured 66-69 C on all four cores under full load. On a tablet that
# means the whole back gets warm before anything slows down, which is exactly the
# "a big app makes the device hot" complaint. There is no fan and the skin
# thermal model (therm_est) is not useful here, so cap the ceiling a little
# earlier from userspace instead.
#
# This script is the single owner of scaling_max_freq: the profile from
# power.mocha.rc decides the ceiling, the temperature decides whether to clamp it
# further. Only writes when the value actually changes, so it does not spam the
# governor.
#
# Tunables (all optional):
#   persist.mocha.thermal.threshold  clamp above this (millidegrees, default 72000)
#   persist.mocha.thermal.resume     release below this (default 66000)
#   persist.mocha.thermal.cap_freq   clamped ceiling (default 1428000)

LOG_TAG=mocha_thermal
SYS_CPU=/sys/devices/system/cpu
ZONE=/sys/class/thermal/thermal_zone0/temp      # CPU-therm

log_msg() {
    log -p i -t "$LOG_TAG" "$1"
}

profile_ceiling() {
    case "$1" in
        0) echo 1224000 ;;   # power save
        3) echo 1530000 ;;   # bias power save
        2) echo 2218500 ;;   # performance
        *) echo 1836000 ;;   # balanced (default)
    esac
}

threshold=$(getprop persist.mocha.thermal.threshold)
resume=$(getprop persist.mocha.thermal.resume)
cap_freq=$(getprop persist.mocha.thermal.cap_freq)
[ -n "$threshold" ] || threshold=72000
[ -n "$resume" ] || resume=66000
[ -n "$cap_freq" ] || cap_freq=1428000

current=""

while true; do
    # The property name must match the one power.mocha.rc and the power HAL
    # use (persist.vendor.mocha.perf.profile); the earlier
    # persist.mocha.perf_profile was a name that nobody ever sets, so the
    # profile here always read empty and the guard kept pushing the balanced
    # ceiling even in power save.
    profile=$(getprop persist.vendor.mocha.perf.profile)
    ceiling=$(profile_ceiling "$profile")

    temp=$(cat "$ZONE" 2>/dev/null)
    [ -n "$temp" ] || temp=0

    if [ "$temp" -ge "$threshold" ]; then
        target=$cap_freq
        [ "$target" -le "$ceiling" ] || target=$ceiling
    elif [ "$temp" -le "$resume" ] && [ "$current" = "$cap_freq" ]; then
        target=$ceiling
    else
        # Between resume and threshold: keep whatever is applied, but follow a
        # profile change immediately.
        if [ "$current" = "$cap_freq" ] && [ "$temp" -gt "$resume" ]; then
            target=$cap_freq
        else
            target=$ceiling
        fi
    fi

    if [ "$target" != "$current" ]; then
        echo "$target" > "$SYS_CPU/cpu0/cpufreq/scaling_max_freq" 2>/dev/null &&
            log_msg "max_freq ${current:-unset} -> ${target} (temp=${temp}, profile=${profile})" &&
            current="$target"
    fi

    sleep 10
done
