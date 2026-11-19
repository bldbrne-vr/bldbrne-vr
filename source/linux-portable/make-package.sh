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

# game-data gate: no CUSA/eboot/param.sfo markers may ride the package
if grep -RIlE "CUSA0[0-9]{4}|eboot|param\.sfo|PS4UPDATE" "$PKG_DIR" >/dev/null 2>&1; then
    echo "REFUSING TO PACK: game-data markers found in the staged tree" >&2
    exit 1
fi

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
