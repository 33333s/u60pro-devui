#!/bin/sh
# Device-side installer: makes the U60Pro UI start on boot via /etc/rc.local.
# Expects the binaries + start.sh already copied into /data/u60pro/.
#   adb push u60pro-devui u60-datad scripts/start.sh /data/u60pro/
#   adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
#
# SPDX-License-Identifier: MIT
DIR=/data/u60pro
RC=/etc/rc.local
HOOK="[ -x $DIR/start.sh ] && sh $DIR/start.sh >/tmp/u60pro-boot.log 2>&1 &"

chmod 755 "$DIR/start.sh" "$DIR/u60pro-devui" "$DIR/u60-datad" 2>/dev/null

if grep -q "$DIR/start.sh" "$RC"; then
    echo "rc.local already hooked"
else
    # Insert the hook just before the final 'exit 0'.
    tmp=$(mktemp)
    awk -v hook="$HOOK" '/^exit 0/ && !d { print hook; d=1 } { print }' "$RC" > "$tmp" \
        && cat "$tmp" > "$RC" && rm -f "$tmp" && echo "rc.local hooked"
fi

echo "--- rc.local tail ---"
grep -n "u60pro\|exit 0" "$RC"
