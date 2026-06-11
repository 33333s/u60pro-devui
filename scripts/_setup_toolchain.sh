#!/bin/bash
# Download + extract a self-contained aarch64 musl cross toolchain (no root).
# Transient helper; safe to delete.
set -e
cd ~
URL="https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--musl--stable-2025.08-1.tar.xz"
F=bootlin-aarch64-musl.tar.xz
DIR=aarch64--musl--stable-2025.08-1

if [ ! -x "$HOME/$DIR/bin/aarch64-linux-gcc" ]; then
    echo ">> downloading $URL"
    curl -fL --retry 30 --retry-all-errors --retry-delay 5 --connect-timeout 20 -C - -o "$F" "$URL"
    echo ">> size: $(du -h "$F" | cut -f1)"
    echo ">> extracting"
    rm -rf "$DIR"
    tar xf "$F"
fi

GCC="$HOME/$DIR/bin/aarch64-linux-gcc"
echo ">> compiler: $GCC"
"$GCC" --version | head -1
echo "TOOLCHAIN-READY"
