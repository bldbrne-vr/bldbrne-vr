#!/usr/bin/env bash
# Packages the Linux portable release from linux-portable/ + a freshly built shadps4.
# Refuses to pack if anything that smells like game data is present (BYOD policy).
set -euo pipefail
SELF_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
VERSION="${1:-0.9.61}"
BINARY="${2:-$SELF_DIR/../build-clang/shadps4}"
OUT_DIR="${3:-/tmp}"

STAGE=$(mktemp -d "$OUT_DIR/bbvr-stage.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
PKG_DIR="$STAGE/bldbrne-vr-$VERSION-linux"

mkdir -p "$PKG_DIR"
cp -p "$BINARY" "$PKG_DIR/shadps4"
cp -p "$SELF_DIR/bbvr.sh" "$SELF_DIR/README-LINUX.txt" "$PKG_DIR/"
chmod +x "$PKG_DIR/bbvr.sh" "$PKG_DIR/shadps4"
chmod 644 "$PKG_DIR/README-LINUX.txt"

# game-data gate: no game files may ride the package (file-name scan). The binary's
# identity is proven by sha256 (below); mentions of dump IDs in docs are upstream's own
# wording and are not game data.
for f in "$PKG_DIR"/*; do
    case "$(basename "$f")" in
        eboot*|param.sfo|PS4UPDATE*|CUSA*) echo "REFUSING TO PACK: $(basename "$f") looks like game data" >&2; exit 1;;
    esac
done
# verify the binary inside the package is byte-identical to the built one
SRC_SHA=$(sha256sum "$BINARY" | cut -d' ' -f1)
PKG_SHA=$(sha256sum "$PKG_DIR/shadps4" | cut -d' ' -f1)
[ "$SRC_SHA" = "$PKG_SHA" ] || { echo "FATAL: packaged binary differs from source" >&2; exit 1; }

cd "$STAGE"
TARBALL="$OUT_DIR/Bldbrne-VR-$VERSION-linux-portable.tar.gz"
tar -czf "$TARBALL" "$(basename "$PKG_DIR")"

echo "package dir:   $PKG_DIR (staged)"
echo "tarball:       $TARBALL"
echo "binary sha256: $PKG_SHA"
stat -c "size:          %s bytes" "$TARBALL"
