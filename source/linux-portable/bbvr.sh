#!/usr/bin/env bash
# bldbrne-vr Linux portable launcher — menu for dep-check, OpenXR setup, smoke test, play.
# Runs from the directory it lives in; keep it next to the 'shadps4' binary.
set -u
SELF_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$SELF_DIR"
BIN=./shadps4
CONF=./bbvr.conf

have_cmd() { command -v "$1" > /dev/null 2>&1; }

find_vrpathreg() {
    local base
    for base in "${STEAM_HOME:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam"; do
        [ -n "$base" ] || continue
        local c="$base/steamapps/common/SteamVR/bin/linux64/vrpathreg"
        [ -x "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

game_dump() {
    if [ -n "${1:-}" ]; then
        echo "$1"
        return 0
    fi
    if [ -f "$CONF" ]; then
        # shellcheck disable=SC1090
        . "$CONF"
        BBVR_DUMP="${BBVR_DUMP:-}"
    else
        BBVR_DUMP=""
    fi
    if [ -z "$BBVR_DUMP" ] || [ ! -d "$BBVR_DUMP" ]; then
        echo "Where is your Bloodborne 1.09 dump (the folder containing eboot.bin)?"
        read -r -p "Path: " BBVR_DUMP
        if [ -e "$CONF" ] || echo "BBVR_DUMP=\"$BBVR_DUMP\"" > "$CONF"; then
            echo "(saved to $CONF)"
        fi
    fi
    echo "$BBVR_DUMP"
}

dep_check() {
    echo "== dependency check =="
    if [ ! -x "$BIN" ]; then echo "FATAL: $BIN missing next to this script"; return 1; fi
    local missing
    missing=$(ldd "$BIN" 2>&1 | grep "not found")
    if [ -z "$missing" ]; then
        echo "OK: all shared libraries resolved."
        return 0
    fi
    echo "$missing"
    echo
    echo "Install the packages providing those libraries:"
    echo "  Arch/CachyOS: sudo pacman -S systemd-libs libutil-linux   (provides libudev/libuuid)"
    echo "  Debian/Ubuntu: sudo apt install libudev1 libuuid1"
    return 1
}

steamvr_settings_recommended_trims() {
    # Known NVIDIA-observed crash triggers (frunk camera/room view + desktop mirror view).
    # Idempotent: appends the sections only if missing. Revert = delete the two sections.
    local cfg="" base
    for base in "${STEAM_HOME:-}" "$HOME/.local/share/Steam" "$HOME/.steam/steam"; do
        [ -n "$base" ] || continue
        if [ -f "$base/config/steamvr.settings" ]; then cfg="$base/config/steamvr.settings"; break; fi
        [ -d "$base/config" ] && { cfg="$base/config/steamvr.settings"; break; }
    done
    if [ -z "$cfg" ]; then
        echo "steamvr.settings not found — install SteamVR first."
        return 1
    fi
    touch "$cfg"
    local changed=0
    if ! grep -aq "enableCamera" "$cfg"; then
        printf '\n[camera]\nenableCamera = false\nenableCameraForRoomView = false\n' >> "$cfg"
        changed=1
    fi
    if ! grep -aq "mirrorview" "$cfg"; then
        printf '\n[mirrorview]\nmirrorviewfullscreen = false\n' >> "$cfg"
        changed=1
    fi
    if [ "$changed" = 1 ]; then
        echo "Applied recommended SteamVR trims to $cfg:"
        echo "  frunk camera + desktop mirror view off (known NVIDIA compositor crash triggers)."
        echo "  Tradeoff: no in-VR camera/mirror. Revert by deleting those sections."
    else
        echo "SteamVR trims already present — nothing to do."
    fi
}

runtime_setup() {
    echo "== OpenXR runtime setup =="
    local vrpathreg
    if ! vrpathreg=$(find_vrpathreg); then
        echo "SteamVR not found under Steam's usual paths."
        echo "Install SteamVR via Steam, then re-run this option."
        return 1
    fi
    "$vrpathreg" setthis
    steamvr_settings_recommended_trims
    if have_cmd openxr_runtime_list; then
        echo; openxr_runtime_list 2>&1 | sed -n '1,6p'
    else
        echo "(install the 'openxr' package to get the runtime verification tool)"
    fi
}

smoke_test() {
    echo "== headset smoke test =="
    "$BIN" --benchmark-headset=/tmp/bbvr-headset.json || return 1
    echo; echo "-- /tmp/bbvr-headset.json --"
    cat /tmp/bbvr-headset.json 2>/dev/null
    echo
    echo "A valid JSON with your headset's per-eye dimensions means the chain"
    echo "shadps4 -> OpenXR -> SteamVR -> headset works."
}

die_no_dump() {
    echo "Could not determine the game folder." >&2
    exit 1
}

play() {
    local mode="$1" dump
    dump=$(game_dump "${2:-}") || die_no_dump
    if [ ! -d "$dump" ]; then echo "Game folder not found: $dump" >&2; exit 1; fi
    if [ "$mode" = vr ]; then
        exec "$BIN" --openxr "$dump"
    else
        exec "$BIN" "$dump"
    fi
}

usage() {
    cat <<EOF
bldbrne-vr Linux portable (community build) — https://github.com/Dr-Bongjoose/bldbrne-vr

Usage (interactive):  ./bbvr.sh
Usage (direct):       ./bbvr.sh --deps            check shared libraries
                      ./bbvr.sh --openxr-setup    register SteamVR as the OpenXR runtime
                      ./bbvr.sh --smoke           headset smoke test (JSON dims + exit)
                      ./bbvr.sh --vr [dump-dir]   play in VR
                      ./bbvr.sh --flat [dump-dir] play without VR
EOF
}

case "${1:-}" in
    --deps) dep_check ;;
    --openxr-setup) runtime_setup ;;
    --smoke) smoke_test ;;
    --vr) play vr "${2:-}" ;;
    --flat) play flat "${2:-}" ;;
    -h|--help|"") usage ;;
    *) usage ;;
esac
