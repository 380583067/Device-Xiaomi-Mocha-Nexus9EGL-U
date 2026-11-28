#!/system/bin/sh
# [2026-11-28 WiFi auto-connect fix] Point the captive-portal probes at
# China-reachable services. The AOSP defaults (connectivitycheck.gstatic.com
# for HTTPS, www.google.com/generate_204 as fallback) are blocked in China:
# every association then fails validation, NetworkMonitor reports
# PARTIAL_CONNECTIVITY / NO_INTERNET, and WifiConfigManager escalates to
# NETWORK_SELECTION_DISABLED_NO_INTERNET_PERMANENT - after a few reboots all
# saved networks are permanently disabled and nothing auto-connects
# (reproduced 2026-11-27/28, see sepolicy boot-audit notes).
#
# hicloud answers generate_204 in ~20ms on this network, miui in ~70ms; both
# are stable. settings put persists in the SettingsProvider DB, so this
# script only matters for fresh installs / new userdata. Run once at
# boot_completed via the mocha_netconfig service in init.tn8.rc.
settings put global captive_portal_http_url http://connectivitycheck.platform.hicloud.com/generate_204
settings put global captive_portal_https_url https://connectivitycheck.platform.hicloud.com/generate_204
settings put global captive_portal_fallback_url http://connect.rom.miui.com/generate_204
settings put global captive_portal_other_fallback_urls http://connect.rom.miui.com/generate_204
