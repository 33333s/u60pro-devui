#!/bin/bash
# Build the HTML-rendering PoC: drm_disp + html_view(litehtml) + FreeType.
set -e
TC="$HOME/aarch64--musl--stable-2025.08-1/bin"
CC="$TC/aarch64-linux-gcc"
CXX="$TC/aarch64-linux-g++"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FT="$HOME/freetype-musl"
LH="$HOME/litehtml-musl"
cd "$ROOT"

[ -f "$LH/lib/liblitehtml.a" ] || { echo "build liblitehtml first (_build_litehtml.sh)"; exit 1; }

for f in drm_disp json data touch_input key_input backlight htmlmain; do
    "$CC" -O2 -D_GNU_SOURCE -Iinclude -c "src/$f.c" -o "/tmp/$f.o"
done
"$CXX" -O2 -std=c++17 -w -Iinclude -I"$FT/include" \
       -I"$LH/include" -I"$LH/include/litehtml" -c src/html_view.cpp -o /tmp/html_view.o

"$CXX" -static /tmp/drm_disp.o /tmp/json.o /tmp/data.o /tmp/touch_input.o \
       /tmp/key_input.o /tmp/backlight.o /tmp/htmlmain.o /tmp/html_view.o \
       "$LH/lib/liblitehtml.a" "$FT/lib/libfreetype.a" -lm -o html-poc

"$TC/aarch64-linux-strip" -o html-poc.stripped html-poc
ls -lh html-poc.stripped
echo "HTMLPOC-OK"
