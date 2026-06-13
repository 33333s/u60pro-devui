#!/bin/bash
# Build a minimal static libfreetype.a for aarch64 musl WITHOUT make/configure,
# by compiling FreeType's per-module amalgamation files directly. Just enough
# for LVGL's binding (TTF/OTF + smooth AA + FT cache). Output: ~/freetype-musl/.
set -e

TC="$HOME/aarch64--musl--stable-2025.08-1/bin"
CC="$TC/aarch64-linux-gcc"
AR="$TC/aarch64-linux-ar"
FT="$HOME/freetype"
OUT="$HOME/freetype-musl"
BUILD="$HOME/ft-build"

[ -x "$CC" ] || { echo "toolchain missing"; exit 1; }
[ -f "$FT/src/base/ftbase.c" ] || { echo "freetype src missing at $FT"; exit 1; }

rm -rf "$BUILD" "$OUT"
mkdir -p "$BUILD" "$OUT/lib"

# Only the modules we compile (avoids undefined refs from the full default list).
cat > "$BUILD/ftmodule_min.h" <<'EOF'
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
EOF

SRCS="base/ftbase base/ftsystem base/ftinit base/ftdebug base/ftbbox \
      base/ftbitmap base/ftglyph base/ftmm cache/ftcache autofit/autofit \
      truetype/truetype cff/cff psaux/psaux psnames/psnames \
      pshinter/pshinter sfnt/sfnt smooth/smooth gzip/ftgzip"

for s in $SRCS; do
    o="$BUILD/$(echo "$s" | tr / _).o"
    "$CC" -O2 -DFT2_BUILD_LIBRARY -I"$FT/include" -I"$BUILD" \
          -DFT_CONFIG_MODULES_H='"ftmodule_min.h"' \
          -c "$FT/src/$s.c" -o "$o"
    echo "  CC $s"
done

"$AR" rcs "$OUT/lib/libfreetype.a" "$BUILD"/*.o
cp -r "$FT/include" "$OUT/include"
echo "FT-BUILD-OK  $(du -h "$OUT/lib/libfreetype.a" | cut -f1)"
