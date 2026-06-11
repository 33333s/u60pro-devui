#!/bin/sh
# Run on the device: stop the vendor UI for real, launch u60pro-devui detached
# so it survives the adb session closing, then report state.
# Push with: adb push scripts/device_run.sh /tmp/ && adb shell sh /tmp/device_run.sh

/etc/init.d/zte_topsw_devui stop 2>/dev/null
ubus call service delete '{"name":"zte_topsw_devui"}' 2>/dev/null
killall -9 zte_topsw_devui 2>/dev/null
killall -9 u60pro-devui 2>/dev/null   # replace any previous instance
sleep 1

nohup /tmp/u60pro-devui >/tmp/devui.log 2>&1 </dev/null &
sleep 3

echo "=== ps ==="
ps w | grep u60pro-devui | grep -v grep
echo "=== dri clients ==="
cat /sys/kernel/debug/dri/0/clients 2>/dev/null
echo "=== log ==="
cat /tmp/devui.log 2>/dev/null
