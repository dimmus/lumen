#!/usr/bin/env bash
# Golden / compile smoke test for wl-scanner-cpp.
# Usage: test_scanner_golden.sh <scanner-bin> <repo-root> <output-dir> [cxx]
set -euo pipefail

SCANNER="${1:?scanner binary required}"
ROOT="${2:?repo root required}"
OUT="${3:?output dir required}"
CXX="${4:-${CXX:-c++}}"

ZLM_XML="$ROOT/protocols/lumen/zlm_shell_v1.xml"
WAYLAND_XML="$ROOT/protocols/upstream/wayland.xml"

mkdir -p "$OUT/lumen"

echo "==> generating from zlm_shell_v1.xml"
"$SCANNER" \
  --input "$ZLM_XML" \
  --module lx.wayland.protocols.lumen \
  --output-dir "$OUT/lumen"

CPPM="$OUT/lumen/lx.wayland.protocols.lumen.cppm"
GEN="$OUT/lumen/lx.wayland.protocols.lumen.gen.cpp"

test -f "$CPPM"
test -f "$GEN"
test -f "$OUT/lumen/lx.wayland.protocols.lumen.dispatch.cppm"

echo "==> verifying capability hex enums are non-zero and distinct"
# Shrunk zlm_shell_v1 — layer/decoration/output capabilities removed (standards are public API).
grep -q 'tiling = 1,' "$CPPM"
grep -q 'workspace_drag = 4,' "$CPPM"
grep -q 'explicit_sync = 16,' "$CPPM"

# Must not collapse hex to zero via base-10 stoi
if grep -E 'tiling = 0,' "$CPPM"; then
  echo "FAIL: capability tiling parsed as 0 (hex base bug)" >&2
  exit 1
fi

echo "==> compiling .gen.cpp with $CXX"
"$CXX" -std=c++20 -c "$GEN" -o "$OUT/lumen-smoke.o"

# Marker symbol present
grep -q 'lx_wayland_protocol_.*_generated' "$GEN"

if [[ -f "$WAYLAND_XML" ]]; then
  echo "==> generating from upstream wayland.xml"
  mkdir -p "$OUT/wayland"
  "$SCANNER" \
    --input "$WAYLAND_XML" \
    --module lx.wayland.protocols.wayland \
    --output-dir "$OUT/wayland"

  WCPPM="$OUT/wayland/lx.wayland.protocols.wayland.cppm"
  WGEN="$OUT/wayland/lx.wayland.protocols.wayland.gen.cpp"
  test -f "$WCPPM"
  test -f "$WGEN"

  # Keyword / numeric enum entry escaping
  grep -q 'e_default = ' "$WCPPM"
  grep -q 'e_90 = ' "$WCPPM"
  grep -q 'e_180 = ' "$WCPPM"
  grep -q 'e_270 = ' "$WCPPM"

  echo "==> compiling wayland .gen.cpp"
  "$CXX" -std=c++20 -c "$WGEN" -o "$OUT/wayland-smoke.o"
else
  echo "==> skipping wayland.xml (not present)"
fi

echo "scanner_golden: OK"
