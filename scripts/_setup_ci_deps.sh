#!/bin/bash
# CI helper: make sure $HOME has the aarch64 musl toolchain and the static
# FreeType/litehtml builds that scripts/build.sh expects. Sources are pinned to
# the revisions release builds use; a persistent runner home keeps everything,
# so only the first job (or a revision bump) pays for downloads and builds.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/deps-revs.sh"

bash "$ROOT/scripts/_setup_toolchain.sh"

# fetch_rev <url> <dir> <rev>: shallow checkout of one exact commit.
fetch_rev() {
    if [ "$(git -C "$2" rev-parse HEAD 2>/dev/null)" = "$3" ]; then return 0; fi
    rm -rf "$2"
    git init -q "$2"
    git -C "$2" fetch -q --depth 1 "$1" "$3"
    git -C "$2" checkout -q FETCH_HEAD
}

# build_lib <out dir> <rev> <script>: rebuild only when the recorded rev differs.
build_lib() {
    if [ "$(cat "$1/.rev" 2>/dev/null)" = "$2" ]; then echo ">> $(basename "$1") up to date"; return 0; fi
    bash "$ROOT/scripts/$3"
    echo "$2" > "$1/.rev"
}

fetch_rev https://github.com/freetype/freetype.git "$HOME/freetype" "$FREETYPE_REV"
fetch_rev https://github.com/litehtml/litehtml.git "$HOME/litehtml" "$LITEHTML_REV"
build_lib "$HOME/freetype-musl" "$FREETYPE_REV" _build_freetype.sh
build_lib "$HOME/litehtml-musl" "$LITEHTML_BUILD" _build_litehtml.sh
echo "CI-DEPS-READY"
