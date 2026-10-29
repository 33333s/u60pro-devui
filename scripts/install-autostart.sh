#!/bin/sh
# Device-side installer: restores the known-good boot path: vendor
# zte_topsw_devui stays enabled for early panel/touch bring-up, and rc.local
# later runs /data/plugins/zwrt-devui/start.sh to hand over to our DevUI.
#
# Expects the binary + start.sh already copied into the plugin dir, and the
# backend installed with zwrt-datad's own installer (/data/zwrt-datad, which
# also writes its own rc.local entry):
#   adb push zwrt-devui scripts/start.sh /data/plugins/zwrt-devui/
#   adb push scripts/install-autostart.sh /tmp/ && adb shell sh /tmp/install-autostart.sh
#
# This script never stops, starts or moves zwrt-datad itself.
#
# SPDX-License-Identifier: MIT
DEVUI_DIR=/data/plugins/zwrt-devui
DATAD_SERVICE=/data/zwrt-datad/service.sh
UI_DIR=$DEVUI_DIR/ui
LEGACY_DIR=/data/u60pro
# pre-rename install (the project was called u60pro-devui)
OLD_DEVUI_DIR=/data/plugins/u60pro-devui
LEGACY_UI_DIR=/data/ui
RC=/etc/rc.local
HOOK="[ -x $DEVUI_DIR/start.sh ] && sh $DEVUI_DIR/start.sh >/tmp/zwrt-devui-boot.log 2>&1 & # zwrt_devui"

count_ui_pages() {
    find "$1" -maxdepth 1 -type f -name '*.html' 2>/dev/null | wc -l | tr -d ' '
}

migrate_legacy_ui() {
    [ -d "$LEGACY_UI_DIR" ] || return 0
    [ -f "$LEGACY_UI_DIR/.lockpin" ] && [ ! -f "$UI_DIR/.lockpin" ] \
        && cp -af "$LEGACY_UI_DIR/.lockpin" "$UI_DIR/.lockpin" 2>/dev/null
    old_count=$(count_ui_pages "$LEGACY_UI_DIR")
    new_count=$(count_ui_pages "$UI_DIR")
    if [ "$new_count" -le 0 ] && [ "$old_count" -gt 0 ]; then
        cp -af "$LEGACY_UI_DIR"/. "$UI_DIR"/ 2>/dev/null || true
    fi
}

remove_legacy_rc_hook() {
    [ -f "$RC" ] || return 0
    tmp=$(mktemp)
    grep -v "$DEVUI_DIR/start.sh" "$RC" | grep -v "$OLD_DEVUI_DIR/start.sh" | grep -v "$LEGACY_DIR/start.sh" \
        | grep -v "zwrt_devui" | grep -v "u60pro_devui" > "$tmp"
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

mkdir -p "$DEVUI_DIR" "$UI_DIR"
[ -f "$LEGACY_DIR/devui.conf" ] && [ ! -f "$DEVUI_DIR/devui.conf" ] && cp -f "$LEGACY_DIR/devui.conf" "$DEVUI_DIR/devui.conf"
for f in devui.conf .device signal.cache ui/.lockpin; do
    [ -f "$OLD_DEVUI_DIR/$f" ] && [ ! -f "$DEVUI_DIR/$f" ] && cp -af "$OLD_DEVUI_DIR/$f" "$DEVUI_DIR/$f"
done
migrate_legacy_ui
chmod 755 "$DEVUI_DIR/start.sh" "$DEVUI_DIR/zwrt-devui" 2>/dev/null
rm -f "$DEVUI_DIR"/*.new \
      "$DEVUI_DIR/ui.tar.gz" "$DEVUI_DIR/u60pro_ui.tar.gz" "$DEVUI_DIR/boot-trace.log.tmp" 2>/dev/null
rm -rf "$DEVUI_DIR/ui_extract" 2>/dev/null

remove_legacy_rc_hook
install_rc_hook

/etc/init.d/zwrt-devui disable 2>/dev/null
/etc/init.d/u60pro-devui disable 2>/dev/null
/etc/init.d/u60-datad disable 2>/dev/null
rm -f /etc/rc.d/S*zwrt-devui /etc/rc.d/K*zwrt-devui \
      /etc/rc.d/S*u60pro-devui /etc/rc.d/K*u60pro-devui \
      /etc/rc.d/S*u60-datad /etc/rc.d/K*u60-datad

/etc/init.d/zwrt-devui stop 2>/dev/null
/etc/init.d/u60pro-devui stop 2>/dev/null
/etc/init.d/u60-datad stop 2>/dev/null
rm -f /etc/init.d/u60pro-devui

killall -9 zwrt-devui 2>/dev/null
killall -9 u60pro-devui 2>/dev/null
killall -9 u60-datad 2>/dev/null

rm -f "$LEGACY_DIR/u60pro-devui" "$LEGACY_DIR/u60-datad" "$LEGACY_DIR/zwrt-datad" \
      "$LEGACY_DIR/start.sh" "$LEGACY_DIR/u60pro_ui.tar.gz" "$LEGACY_DIR/ui.tar.gz" \
      "$LEGACY_DIR/boot-trace.log" "$LEGACY_DIR/boot-trace.log.tmp" "$LEGACY_DIR"/*.new 2>/dev/null
rm -rf "$LEGACY_DIR/ui_extract" 2>/dev/null
rm -rf "$LEGACY_DIR" "$LEGACY_UI_DIR" "$OLD_DEVUI_DIR"

/etc/init.d/zte_topsw_devui enable 2>/dev/null
/etc/init.d/zte_topsw_devui start 2>/dev/null

sh "$DEVUI_DIR/start.sh" >/tmp/zwrt-devui-boot.log 2>&1 &

echo "--- service status ---"
if [ -x /etc/init.d/zte_topsw_devui ]; then
    /etc/init.d/zte_topsw_devui enabled >/dev/null 2>&1
echo "zte_topsw_devui_enabled=$?"
fi
echo "zwrt_devui_enabled=legacy_rc_local"
if [ -f "$DATAD_SERVICE" ]; then
    sh "$DATAD_SERVICE" status
else
    echo "zwrt-datad not installed: run its installer (install-datad.sh) to get /data/zwrt-datad"
fi
ps | grep -E 'zte_topsw_devui|zwrt-devui|u60pro-devui|zwrt-datad|u60-datad' | grep -v grep || true

echo "--- rc.local tail ---"
grep -n "u60pro\|plugins\|zwrt-datad\|exit 0" "$RC" 2>/dev/null || true
