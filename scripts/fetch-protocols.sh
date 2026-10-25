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
WLROOTS_TAG="$(grep '^WLROOTS_TAG=' "$LIST" | cut -d= -f2 || true)"
WP_REPO="https://gitlab.freedesktop.org/wayland/wayland-protocols.git"
WAYLAND_REPO="https://gitlab.freedesktop.org/wayland/wayland.git"
WLR_REPO="https://gitlab.freedesktop.org/wlroots/wlr-protocols.git"
WLROOTS_REPO="https://gitlab.freedesktop.org/wlroots/wlroots.git"

NEED_WLR=0
NEED_WLROOTS=0
while IFS='|' read -r source relpath; do
    [[ -z "${source:-}" || "$source" == TAG* || "$source" == WLROOTS_TAG* ]] && continue
    [[ "$source" == "wlr" ]] && NEED_WLR=1
    [[ "$source" == "wlroots" ]] && NEED_WLROOTS=1
done < "$LIST"

echo "==> Cloning wayland-protocols (${TAG})..."
git -c advice.detachedHead=false clone --depth 1 --branch "$TAG" "$WP_REPO" "$TMP/wayland-protocols"

echo "==> Cloning wayland (wayland.xml)..."
git -c advice.detachedHead=false clone --depth 1 "$WAYLAND_REPO" "$TMP/wayland"

if [[ "$NEED_WLR" -eq 1 ]]; then
    echo "==> Cloning wlr-protocols..."
    git -c advice.detachedHead=false clone --depth 1 "$WLR_REPO" "$TMP/wlr-protocols"
fi

if [[ "$NEED_WLROOTS" -eq 1 ]]; then
    # Only protocol/ is needed: a blobless sparse checkout keeps this a few hundred KiB.
    echo "==> Cloning wlroots (${WLROOTS_TAG}, protocol/ only)..."
    git -c advice.detachedHead=false clone --depth 1 --branch "$WLROOTS_TAG" --filter=blob:none --sparse \
        "$WLROOTS_REPO" "$TMP/wlroots"
    git -C "$TMP/wlroots" sparse-checkout set protocol
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
        wlroots)
            src="$TMP/wlroots/protocol/$relpath"
            out="$DEST/wlroots/$relpath"
            mkdir -p "$(dirname "$out")"
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
done < <(grep -v -e '^TAG=' -e '^WLROOTS_TAG=' "$LIST")

rm -rf "$TMP" "$LIST"

# The build lists this stamp as a configure dependency, so the next build picks up what changed.
touch "$DEST/.fetch-stamp"

if [[ "$FAILED" -ne 0 ]]; then
    echo "==> Failed: ${#MISSING[@]} protocol path(s) not found:" >&2
    for p in "${MISSING[@]}"; do
        echo "    - $p" >&2
    done
    exit 1
fi

echo "==> Done. Upstream protocols in $DEST"
