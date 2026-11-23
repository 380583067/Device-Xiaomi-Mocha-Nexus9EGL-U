#!/system/bin/sh
# [setup wizard skip / fallback 2026-11-17] Keep this tablet from showing the setup
# wizard and boot straight to the launcher.
#
# The main fix is in the device tree overlay (def_device_provisioned /
# def_user_setup_complete default to true), so a fresh database is provisioned from
# the start with no race.
#
# This script only handles what the main fix cannot: userdata already holds a
# SettingsProvider database (defaults apply only at creation), in which case the two
# flags are written here.
#
# It also disables com.android.messaging: this device has ro.radio.noril=yes and no
# telephony. That used to be `on property:ro.radio.noril=yes` + `exec pm disable`, but
# at that point system_server is not up and pm must fail, hence the move here.
#
# Started by the mocha_provision oneshot service in init.tn8.rc once
# sys.boot_completed=1.

TAG="mocha_provision"

i=0
ok=0
while [ "$i" -lt 20 ]; do
    settings put global device_provisioned 1 >/dev/null 2>&1
    settings put secure user_setup_complete 1 >/dev/null 2>&1

    # Do not trust the return code of the put; read it back to confirm SettingsProvider stored it.
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

# Disable the SMS app (no RIL, useless anyway). A failure does not affect anything
# above, it only leaves one log line.
pm disable com.android.messaging >/dev/null 2>&1 ||
    echo "$TAG: pm disable com.android.messaging failed (package missing?)" >&2

exit 0
