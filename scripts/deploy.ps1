# deploy.ps1 - push the built binary to the device, swap out the vendor UI,
# run u60pro-devui, and (on exit) restore the vendor UI.
#
# Usage:  pwsh scripts/deploy.ps1 [-Persist]
#   (default)  : run once in the foreground; Ctrl-C restores the vendor UI.
#   -Persist   : leave u60pro-devui running and do NOT restart the vendor UI.
#
# Build first (see README) so that ./u60pro-devui exists.
#
# SPDX-License-Identifier: MIT
param([switch]$Persist)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root "u60pro-devui"

if (-not (Test-Path $bin)) {
    throw "binary not found: $bin  (build it first: make CROSS_COMPILE=aarch64-linux-musl-)"
}

adb push $bin /tmp/u60pro-devui
adb shell "chmod 755 /tmp/u60pro-devui"

# Stop the vendor UI so it releases /dev/dri/card0.
adb shell "ubus call service delete '{\"name\":\"zte_topsw_devui\"}' 2>/dev/null; killall -9 zte_topsw_devui 2>/dev/null; sleep 1"

if ($Persist) {
    Write-Host "Launching u60pro-devui detached (vendor UI will NOT auto-restore)..."
    adb shell "/tmp/u60pro-devui >/tmp/u60pro-devui.log 2>&1 &"
    Start-Sleep -Seconds 2
    adb shell "cat /tmp/u60pro-devui.log"
} else {
    Write-Host "Running u60pro-devui (Ctrl-C to stop and restore vendor UI)..."
    try {
        adb shell "/tmp/u60pro-devui"
    } finally {
        Write-Host "Restoring vendor UI..."
        adb shell "/etc/init.d/zte_topsw_devui start 2>/dev/null; sleep 1; ps w | grep zte_topsw_devui | grep -v grep"
    }
}
