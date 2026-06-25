#!/bin/sh
# Device-side installer: restores the known-good boot path: vendor
# zte_topsw_devui stays enabled for early panel/touch bring-up, and rc.local
# later runs /data/plugins/u60pro-devui/start.sh to hand over to our DevUI.
#
# Expects the binaries + start.sh already copied into the plugin dirs.
#   adb push u60pro-devui scripts/start.sh /data/plugins/u60pro-devui/
#   adb push zwrt-datad /data/plugins/zwrt-datad/
#   adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/u60pro-devui
DATAD_DIR=/data/plugins/zwrt-datad
LEGACY_DIR=/data/u60pro
RC=/etc/rc.local
HOOK="[ -x $DEVUI_DIR/start.sh ] && sh $DEVUI_DIR/start.sh >/tmp/u60pro-boot.log 2>&1 & # u60pro_devui"

remove_legacy_rc_hook() {
    [ -f "$RC" ] || return 0
    tmp=$(mktemp)
    grep -v "$DEVUI_DIR/start.sh" "$RC" | grep -v "$LEGACY_DIR/start.sh" | grep -v "u60pro_devui" > "$tmp"
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

mkdir -p "$DEVUI_DIR" "$DATAD_DIR"
[ -f "$LEGACY_DIR/devui.conf" ] && [ ! -f "$DEVUI_DIR/devui.conf" ] && cp -f "$LEGACY_DIR/devui.conf" "$DEVUI_DIR/devui.conf"
chmod 755 "$DEVUI_DIR/start.sh" "$DEVUI_DIR/u60pro-devui" "$DATAD_DIR/zwrt-datad" 2>/dev/null
rm -f "$DATAD_DIR/u60-datad" "$DEVUI_DIR"/*.new "$DATAD_DIR"/*.new \
      "$DEVUI_DIR/ui.tar.gz" "$DEVUI_DIR/u60pro_ui.tar.gz" "$DEVUI_DIR/boot-trace.log.tmp" 2>/dev/null
rm -rf "$DEVUI_DIR/ui_extract" 2>/dev/null

remove_legacy_rc_hook
install_rc_hook

/etc/init.d/u60pro-devui disable 2>/dev/null
/etc/init.d/zwrt-datad disable 2>/dev/null
/etc/init.d/u60-datad disable 2>/dev/null
rm -f /etc/rc.d/S*u60pro-devui /etc/rc.d/K*u60pro-devui \
      /etc/rc.d/S*zwrt-datad /etc/rc.d/K*zwrt-datad \
      /etc/rc.d/S*u60-datad /etc/rc.d/K*u60-datad

/etc/init.d/u60pro-devui stop 2>/dev/null
/etc/init.d/zwrt-datad stop 2>/dev/null
/etc/init.d/u60-datad stop 2>/dev/null

killall -9 u60pro-devui 2>/dev/null
killall -9 zwrt-datad 2>/dev/null
killall -9 u60-datad 2>/dev/null

rm -f "$LEGACY_DIR/u60pro-devui" "$LEGACY_DIR/u60-datad" "$LEGACY_DIR/zwrt-datad" \
      "$LEGACY_DIR/start.sh" "$LEGACY_DIR/u60pro_ui.tar.gz" "$LEGACY_DIR/ui.tar.gz" \
      "$LEGACY_DIR/boot-trace.log" "$LEGACY_DIR/boot-trace.log.tmp" "$LEGACY_DIR"/*.new 2>/dev/null
rm -rf "$LEGACY_DIR/ui_extract" 2>/dev/null
rmdir "$LEGACY_DIR" 2>/dev/null || true

/etc/init.d/zte_topsw_devui enable 2>/dev/null
/etc/init.d/zte_topsw_devui start 2>/dev/null

sh "$DEVUI_DIR/start.sh" >/tmp/u60pro-boot.log 2>&1 &

echo "--- service status ---"
if [ -x /etc/init.d/zte_topsw_devui ]; then
    /etc/init.d/zte_topsw_devui enabled >/dev/null 2>&1
echo "zte_topsw_devui_enabled=$?"
fi
echo "u60pro_devui_enabled=legacy_rc_local"
ps | grep -E 'zte_topsw_devui|u60pro-devui|zwrt-datad|u60-datad' | grep -v grep || true

echo "--- rc.local tail ---"
grep -n "u60pro\|plugins\|exit 0" "$RC" 2>/dev/null || true
