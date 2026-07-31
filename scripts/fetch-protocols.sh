#!/usr/bin/env bash
# Fetch upstream Wayland protocol XML files from protocols/manifest.toml
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/protocols/upstream"
LIST="$ROOT/protocols/.fetch-list"
TMP="${TMPDIR:-/tmp}/lumen-protocols-$$"
FAILED=0
MISSING=()

mkdir -p "$DEST" "$TMP"

python3 "$ROOT/scripts/protocol_manifest.py" \
    --root "$ROOT" \
    --fetch-out "$LIST"

TAG="$(grep '^TAG=' "$LIST" | cut -d= -f2)"
WP_REPO="https://gitlab.freedesktop.org/wayland/wayland-protocols.git"
WAYLAND_REPO="https://gitlab.freedesktop.org/wayland/wayland.git"
WLR_REPO="https://gitlab.freedesktop.org/wlroots/wlr-protocols.git"

NEED_WLR=0
while IFS='|' read -r source relpath; do
    [[ -z "${source:-}" || "$source" == "TAG" ]] && continue
    [[ "$source" == "wlr" ]] && NEED_WLR=1
done < "$LIST"

echo "==> Cloning wayland-protocols (${TAG})..."
git clone --depth 1 --branch "$TAG" "$WP_REPO" "$TMP/wayland-protocols"

echo "==> Cloning wayland (wayland.xml)..."
git clone --depth 1 "$WAYLAND_REPO" "$TMP/wayland"

if [[ "$NEED_WLR" -eq 1 ]]; then
    echo "==> Cloning wlr-protocols..."
    git clone --depth 1 "$WLR_REPO" "$TMP/wlr-protocols"
fi

while IFS='|' read -r source relpath; do
    [[ -z "${source:-}" || "$source" == "TAG" ]] && continue
    out="$DEST/$relpath"
    mkdir -p "$(dirname "$out")"
    case "$source" in
        wayland)
            src="$TMP/wayland/protocol/$relpath"
            ;;
        wlr)
            src="$TMP/wlr-protocols/$relpath"
            ;;
        *)
            src="$TMP/wayland-protocols/$relpath"
            ;;
    esac
    if [[ ! -f "$src" ]]; then
        echo "  MISSING $relpath (source=$source)" >&2
        MISSING+=("$relpath")
        FAILED=1
        continue
    fi
    cp "$src" "$out"
    echo "  copied $relpath"
done < <(grep -v '^TAG=' "$LIST")

rm -rf "$TMP" "$LIST"

if [[ "$FAILED" -ne 0 ]]; then
    echo "==> Failed: ${#MISSING[@]} protocol path(s) not found:" >&2
    for p in "${MISSING[@]}"; do
        echo "    - $p" >&2
    done
    exit 1
fi

echo "==> Done. Upstream protocols in $DEST"
