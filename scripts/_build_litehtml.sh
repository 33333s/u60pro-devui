#!/bin/bash
# Build static liblitehtml.a (+ bundled gumbo) for aarch64 musl, no cmake.
# Output: ~/litehtml-musl/{lib,include}
set -e
TC="$HOME/aarch64--musl--stable-2025.08-1/bin"
CXX="$TC/aarch64-linux-g++"
CC="$TC/aarch64-linux-gcc"
AR="$TC/aarch64-linux-ar"
LH="$HOME/litehtml"
OUT="$HOME/litehtml-musl"
BUILD="$HOME/lh-build"

[ -f "$LH/include/litehtml.h" ] || { echo "litehtml src missing at $LH"; exit 1; }
rm -rf "$BUILD" "$OUT"; mkdir -p "$BUILD" "$OUT/lib"

GUMBO_INC="-I$LH/src/gumbo/include -I$LH/src/gumbo/include/gumbo"

echo ">> compiling gumbo (C)"
for f in "$LH"/src/gumbo/*.c; do
    "$CC" -O2 -w $GUMBO_INC -c "$f" -o "$BUILD/gumbo_$(basename "$f" .c).o"
done

echo ">> compiling litehtml (C++), $(ls "$LH"/src/*.cpp | wc -l) files"
for f in "$LH"/src/*.cpp; do
    "$CXX" -O2 -std=c++17 -w -I"$LH/include" -I"$LH/include/litehtml" $GUMBO_INC -c "$f" \
           -o "$BUILD/lh_$(basename "$f" .cpp).o"
done

"$AR" rcs "$OUT/lib/liblitehtml.a" "$BUILD"/*.o
cp -r "$LH/include" "$OUT/include"
echo "LH-BUILD-OK  $(du -h "$OUT/lib/liblitehtml.a" | cut -f1)"
