#!/bin/sh
# Boot launcher for the U60Pro screen UI. Installed at
# /data/plugins/u60pro-devui/start.sh
# and invoked either by the procd init service or by the legacy rc.local hook.
# In procd mode it execs the foreground UI process. In legacy mode it keeps the
# old nohup-based launch path for manual installs and older plugins.
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/u60pro-devui
DATAD_DIR=/data/plugins/zwrt-datad
DEVUI_BIN=$DEVUI_DIR/u60pro-devui
DATAD_BIN=$DATAD_DIR/zwrt-datad
MODE="${1:-legacy}"

mkdir -p "$DEVUI_DIR" "$DATAD_DIR"

read_mode_main_state() {
    awk -F"'" '/option mode_main_state/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

read_reboot_reason_code() {
    awk -F"'" '/option reboot_reason_code/ { print $2; exit }' /etc/config/zwrt_zte_mc_tmp 2>/dev/null
}

boot_trace() {
    LOG="$DEVUI_DIR/boot-trace.log"
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

stop_vendor_ui() {
    had_vendor=0
    pidof zte_topsw_devui >/dev/null 2>&1 && had_vendor=1
    /etc/init.d/zte_topsw_devui stop 2>/dev/null
    killall -9 zte_topsw_devui 2>/dev/null
    [ "$had_vendor" -eq 1 ] && sleep 1
}

start_datad_legacy() {
    [ "$BOOTMODE" = normal ] || return 0
    [ -x "$DATAD_BIN" ] || return 0
    pidof zwrt-datad >/dev/null 2>&1 && return 0
    killall -9 u60-datad 2>/dev/null
    nohup "$DATAD_BIN" -i 1000 >/tmp/zwrt-datad.log 2>&1 </dev/null &
    sleep 1
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

case "$MODE" in
    procd)
        stop_vendor_ui
        [ -x "$DEVUI_BIN" ] || exit 1
        exec "$DEVUI_BIN"
        ;;
    legacy)
        stop_vendor_ui
        # Data aggregator first on normal boots (the UI reads its snapshot).
        # For charge-only boots the full-screen charging UI can fall back to
        # sysfs, so there is no need to wake extra polling daemons.
        start_datad_legacy
        [ -x "$DEVUI_BIN" ] && nohup "$DEVUI_BIN" >/tmp/u60pro-devui.log 2>&1 </dev/null &
        ;;
    *)
        echo "usage: $0 [procd|legacy]" >&2
        exit 2
        ;;
esac
