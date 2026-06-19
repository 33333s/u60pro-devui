#!/bin/sh
# Boot launcher for the U60Pro screen UI. Installed at /data/u60pro/start.sh
# and invoked from /etc/rc.local. Stops the vendor UI, then starts the data
# backend and our UI from the persistent install dir.
#
# SPDX-License-Identifier: MIT
DIR=/data/u60pro

# Do not steal the vendor charging animation during power-off charging boots.
# Normal boots on this firmware expose silent_boot.mode=nonsilent; charger-only
# boots are intentionally left to zte_topsw_devui.
if ! grep -q 'silent_boot.mode=nonsilent' /proc/cmdline 2>/dev/null; then
    echo "u60pro-devui: skip autostart on non-nonsilent boot"
    exit 0
fi

# Release the panel from the vendor UI.
/etc/init.d/zte_topsw_devui stop 2>/dev/null
killall -9 zte_topsw_devui 2>/dev/null
sleep 1

# Data aggregator first (the UI reads its snapshot), then the UI. nohup so they
# survive the launching shell; busybox has no setsid.
[ -x "$DIR/u60-datad" ] && nohup "$DIR/u60-datad" -i 1000 >/tmp/u60-datad.log 2>&1 </dev/null &
sleep 1
[ -x "$DIR/u60pro-devui" ] && nohup "$DIR/u60pro-devui" >/tmp/u60pro-devui.log 2>&1 </dev/null &
