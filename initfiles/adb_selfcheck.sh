#!/vendor/bin/sh
#
# adb_selfcheck.sh - after boot, check that adb works; if not, reboot to recovery (TWRP)
#
# Background: mocha is a legacy g_android FunctionFS device (3.10 kernel) and adbd in
# Android 14 (APEX com.android.adbd) has incomplete support for legacy transports, so
# occasionally adbd creates functionfs but its transport read thread never starts
#
# This script runs after boot and checks whether adbd established a connection. If boot has
# completed but adbd stalls at "registering usb transport" with no further progress, it is
# declared offline and the device reboots to recovery, avoiding manual key presses for logs.
#
# Detection logic:
#   With a healthy link adbd keeps progressing after "registering usb transport":
#     - receives A_CNXN from the host, transport goes kCsOffline -> kCsDevice;
#     - receives a framework authorisation (adbd_auth: received a new framework connection);
#     - receives host authorisation (adb client <id> authorized).
#   When offline, adbd stops right after "registering usb transport" with no further logs.
#
# Signal used: take the last adbd line in logcat; if it is still
# "registering usb transport" (or an earlier functionfs init line), adbd made no progress
# after registering the transport and is declared offline.

SELFCHECK_DELAY="${SELFCHECK_DELAY:-90}"   # total delay (boot completion + host enumeration)
GRACE_PERIOD="${GRACE_PERIOD:-20}"         # extra wait window

sleep "${SELFCHECK_DELAY}"

# Has the system finished booting
if [ "$(getprop sys.boot_completed)" != "1" ]; then
    log -t adb_selfcheck "boot not completed, skip"
    exit 0
fi

# Is adbd alive
if ! pidof adbd > /dev/null 2>&1; then
    log -t adb_selfcheck "adbd not running -> reboot recovery"
    reboot recovery
    exit 1
fi

# Check once more after the grace period (gives the host adb time to re-enumerate)
sleep "${GRACE_PERIOD}"

# Take the last key adbd log line and see whether it progressed past transport registration.
# Skip SELinux audit noise and look only at adbd's own INFO lines.
last_adbd_log=$(logcat -d -b all 2>/dev/null \
    | grep -E "^[0-9-]+ [0-9:.]+ +[0-9]+ +[0-9]+ I adbd" \
    | tail -1)

log -t adb_selfcheck "last adbd log: ${last_adbd_log}"

# If the last adbd line is still functionfs init / transport registration, declare offline
if echo "${last_adbd_log}" | grep -qE "initializing functionfs|opening control endpoint|functionfs successfully initialized|registering usb transport"; then
    log -t adb_selfcheck "adbd stuck at transport registration -> reboot recovery"
    reboot recovery
    exit 1
fi

# adbd made progress (framework connection / host authorisation), so it is healthy
log -t adb_selfcheck "adbd has progressed beyond transport registration, adb healthy"
exit 0
