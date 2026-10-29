# deploy.ps1 - push the built binary to the device, swap out the vendor UI,
# run zwrt-devui, and (on exit) restore the vendor UI.
#
# Usage:  pwsh scripts/deploy.ps1 [-Persist]
#   (default)  : run once in the foreground; Ctrl-C restores the vendor UI.
#   -Persist   : leave zwrt-devui running and do NOT restart the vendor UI.
#
# Build first (see README) so that ./zwrt-devui exists.
#
# SPDX-License-Identifier: MIT
param([switch]$Persist)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root "zwrt-devui"

if (-not (Test-Path $bin)) {
    throw "binary not found: $bin  (build it first: make CROSS_COMPILE=aarch64-linux-musl-)"
}

adb push $bin /tmp/zwrt-devui
adb shell "chmod 755 /tmp/zwrt-devui"

# Stop the vendor UI so it releases /dev/dri/card0.
adb shell "ubus call service delete '{\"name\":\"zte_topsw_devui\"}' 2>/dev/null; killall -9 zte_topsw_devui 2>/dev/null; sleep 1"

if ($Persist) {
    Write-Host "Launching zwrt-devui detached (vendor UI will NOT auto-restore)..."
    adb shell "/tmp/zwrt-devui >/tmp/zwrt-devui.log 2>&1 &"
    Start-Sleep -Seconds 2
    adb shell "cat /tmp/zwrt-devui.log"
} else {
    Write-Host "Running zwrt-devui (Ctrl-C to stop and restore vendor UI)..."
    try {
        adb shell "/tmp/zwrt-devui"
    } finally {
        Write-Host "Restoring vendor UI..."
        adb shell "/etc/init.d/zte_topsw_devui start 2>/dev/null; sleep 1; ps w | grep zte_topsw_devui | grep -v grep"
    }
}
