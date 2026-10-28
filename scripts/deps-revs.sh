# Pinned upstream revisions of the static libraries, shared by the CI setup and
# the host preview. Sourced, not run.
FREETYPE_REV=25a08f24cfc0da879d1938352d026532f280b77e
LITEHTML_REV=9bc84b8b8d15a4e50f18b327aa30955048b441c2

# litehtml is built with litehtml.patch applied (parsed-stylesheet cache); the
# patch hash is part of the build id so changing it rebuilds the library.
LITEHTML_PATCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/litehtml.patch"
LITEHTML_BUILD="$LITEHTML_REV-$(sha256sum "$LITEHTML_PATCH" | cut -c1-12)"
