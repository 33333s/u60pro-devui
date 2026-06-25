#!/bin/sh
# Device-side installer: restores the known-good boot path: vendor
# zte_topsw_devui stays enabled for early panel/touch bring-up, and rc.local
# later runs /data/u60pro/start.sh to hand over to our DevUI.
#
# Expects the binaries + start.sh already copied into /data/u60pro/.
#   adb push u60pro-devui u60-datad scripts/start.sh /data/u60pro/
#   adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
#
# SPDX-License-Identifier: MIT
DIR=/data/u60pro
RC=/etc/rc.local
HOOK="[ -x $DIR/start.sh ] && sh $DIR/start.sh >/tmp/u60pro-boot.log 2>&1 & # u60pro_devui"

remove_legacy_rc_hook() {
    [ -f "$RC" ] || return 0
    tmp=$(mktemp)
    grep -v "$DIR/start.sh" "$RC" | grep -v "u60pro_devui" > "$tmp"
    cat "$tmp" > "$RC"
    rm -f "$tmp"
}

install_rc_hook() {
    [ -f "$RC" ] || return 1
    tmp=$(mktemp)
    awk -v hook="$HOOK" '/^exit 0/ && !d { print hook; d=1 } { print }' "$RC" > "$tmp" \
        && cat "$tmp" > "$RC"
    rm -f "$tmp"
}

chmod 755 "$DIR/start.sh" "$DIR/u60pro-devui" "$DIR/u60-datad" 2>/dev/null

remove_legacy_rc_hook
install_rc_hook

/etc/init.d/u60pro-devui disable 2>/dev/null
/etc/init.d/u60-datad disable 2>/dev/null
rm -f /etc/rc.d/S*u60pro-devui /etc/rc.d/K*u60pro-devui \
      /etc/rc.d/S*u60-datad /etc/rc.d/K*u60-datad

/etc/init.d/u60pro-devui stop 2>/dev/null
/etc/init.d/u60-datad stop 2>/dev/null

killall -9 u60pro-devui 2>/dev/null
killall -9 u60-datad 2>/dev/null

/etc/init.d/zte_topsw_devui enable 2>/dev/null
/etc/init.d/zte_topsw_devui start 2>/dev/null

sh "$DIR/start.sh" >/tmp/u60pro-boot.log 2>&1 &

echo "--- service status ---"
if [ -x /etc/init.d/zte_topsw_devui ]; then
    /etc/init.d/zte_topsw_devui enabled >/dev/null 2>&1
    echo "zte_topsw_devui_enabled=$?"
fi
echo "u60pro_devui_enabled=legacy_rc_local"
ps | grep -E 'zte_topsw_devui|u60pro-devui|u60-datad' | grep -v grep || true

echo "--- rc.local tail ---"
grep -n "u60pro\|exit 0" "$RC" 2>/dev/null || true
