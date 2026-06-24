#!/bin/sh
# Boot launcher for the U60Pro screen UI. Installed at /data/u60pro/start.sh
# and invoked from /etc/rc.local. Stops the vendor UI, then starts the data
# backend and our UI from the persistent install dir.
#
# SPDX-License-Identifier: MIT
DIR=/data/u60pro

read_mode_main_state() {
    awk -F"'" '/option mode_main_state/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

read_reboot_reason_code() {
    awk -F"'" '/option reboot_reason_code/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

boot_trace() {
    LOG="$DIR/boot-trace.log"
    {
        echo "=== $(date '+%Y-%m-%d %H:%M:%S') ==="
        echo "mode_main_state=$mode_main_state"
        echo "reboot_reason_code=$reboot_reason_code"
        echo "bootmode=$BOOTMODE"
        echo -n "cmdline="
        cat /proc/cmdline 2>/dev/null
        for f in \
            /sys/class/power_supply/usb/online \
            /sys/class/power_supply/usb/voltage_now \
            /sys/class/power_supply/battery/status \
            /sys/class/power_supply/battery/capacity \
            /sys/class/power_supply/charger_zte/present_mbb \
            /sys/class/power_supply/charger_zte/status_mbb \
            /sys/class/power_supply/type-c_zte/present_mbb \
            /sys/class/power_supply/type-c_zte/real_type_mbb \
            /sys/class/power_supply/statistics_zte/batt_status \
            /sys/class/power_supply/statistics_zte/batt_online \
            /sys/class/power_supply/battery_zte/status_mbb \
            /sys/class/power_supply/battery_zte/online_mbb
        do
            [ -e "$f" ] && echo "$f=$(cat "$f" 2>/dev/null)"
        done
        for e in /dev/input/event*; do
            [ -e "$e" ] || continue
            echo "$e=$(cat /sys/class/input/${e##*/}/device/name 2>/dev/null)"
        done
        echo
    } >> "$LOG"
    tail -n 160 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
}

# Power-off charging boots do not expose silent_boot.mode=nonsilent. We still
# start DevUI there now, but only in its own full-screen charging mode.
BOOTMODE=charge
mode_main_state="$(read_mode_main_state)"
reboot_reason_code="$(read_reboot_reason_code)"
case "$mode_main_state" in
    mode_power_off_*) BOOTMODE=charge ;;
    mode_power_on|mode_power_on_charger) BOOTMODE=normal ;;
    *)
        if grep -q 'silent_boot.mode=nonsilent' /proc/cmdline 2>/dev/null; then
            BOOTMODE=normal
        fi
        ;;
esac
boot_trace

# Release the panel from the vendor UI.
/etc/init.d/zte_topsw_devui stop 2>/dev/null
killall -9 zte_topsw_devui 2>/dev/null
sleep 1

# Data aggregator first on normal boots (the UI reads its snapshot). For
# charge-only boots the full-screen charging UI can fall back to sysfs, so
# there is no need to wake extra polling daemons.
if [ "$BOOTMODE" = normal ] && [ -x "$DIR/u60-datad" ]; then
    nohup "$DIR/u60-datad" -i 1000 >/tmp/u60-datad.log 2>&1 </dev/null &
    sleep 1
fi
[ -x "$DIR/u60pro-devui" ] && nohup "$DIR/u60pro-devui" >/tmp/u60pro-devui.log 2>&1 </dev/null &
