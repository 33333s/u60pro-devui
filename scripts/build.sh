#!/bin/bash
# Build u60pro-devui with the local Bootlin aarch64 musl toolchain.
# Usage: wsl -- bash -lc 'bash /mnt/d/.../scripts/build.sh'
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$HOME/aarch64--musl--stable-2025.08-1/bin"
CC="$TC/aarch64-linux-gcc"
cd "$ROOT"

if [ ! -x "$CC" ]; then echo "toolchain missing: $CC"; exit 1; fi
if [ ! -f third_party/lvgl/lvgl.h ]; then echo "LVGL missing"; exit 1; fi

# Optional: static FreeType for CJK text (built by scripts/_build_freetype.sh).
FT_DIR="$HOME/freetype-musl"
FT_CFLAGS=""; FT_LIB=""
if [ -f "$FT_DIR/lib/libfreetype.a" ]; then
  FT_CFLAGS="-I$FT_DIR/include"
  FT_LIB="$FT_DIR/lib/libfreetype.a"
  echo ">> FreeType: $FT_LIB"
fi

CFLAGS="-std=c11 -Os -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter \
  -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -I. -Iinclude -Ithird_party/lvgl $FT_CFLAGS"

# Our sources first so our own errors surface before the LVGL bulk.
APP_SRCS=$(ls src/*.c)
LVGL_SRCS=$(find third_party/lvgl/src -name '*.c' | sort)

echo ">> compiler: $($CC -dumpversion)"
echo ">> app sources: $(echo "$APP_SRCS" | wc -w), lvgl sources: $(echo "$LVGL_SRCS" | wc -w)"
echo ">> building (this takes a few minutes)..."

# shellcheck disable=SC2086
$CC $CFLAGS $APP_SRCS $LVGL_SRCS $FT_LIB -static -Wl,--gc-sections -lm -o u60pro-devui

echo ">> link OK"
"$TC/aarch64-linux-size" u60pro-devui
"$TC/aarch64-linux-strip" -o u60pro-devui.stripped u60pro-devui 2>/dev/null || true
ls -lh u60pro-devui u60pro-devui.stripped 2>/dev/null
echo "BUILD-OK"
