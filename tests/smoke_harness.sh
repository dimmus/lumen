#!/usr/bin/env bash
# The smoke harness's own plumbing, tested without a seat: binary freshness against a
# synthetic Ninja tree (scripts/build-fresh.sh), injection-node lookup against a sysfs
# fixture (scripts/inject-devices.sh), and inject-input.py's key table and pixel mapping.
# Everything here is hermetic: no /dev/input, no real build, no compositor.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fails=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; fails=$((fails + 1)); }
check() { # NAME EXPECTED ACTUAL
  [[ "$2" == "$3" ]] && pass "$1" || fail "$1 (want '$2', got '$3')"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "build-fresh.sh — the build system's own up-to-date determination:"
# shellcheck source=../scripts/build-fresh.sh
source "$ROOT/scripts/build-fresh.sh"
T=$TMP/build
mkdir -p "$T/CMakeFiles"
cat >"$T/a.cpp" <<'EOF'
int a();
EOF
cat >"$T/s.cppm" <<'EOF'
export module s;
EOF
echo "project(synthetic)" >"$T/CMakeLists.txt"
echo "# no CONFIGURE_DEPENDS globs here" >"$T/CMakeFiles/VerifyGlobs.cmake"
cat >"$T/build.ninja" <<'EOF'
rule TOUCH
  command = touch $out
rule REGEN
  command = true
  generator = 1
build build.ninja: REGEN | CMakeLists.txt CMakeFiles/VerifyGlobs.cmake
build liba.o: TOUCH a.cpp
build lumen-compositor: TOUCH liba.o
build libshell.o: TOUCH s.cppm
build lumen-shell: TOUCH libshell.o
default lumen-compositor lumen-shell
EOF
ninja -C "$T" >/dev/null 2>&1

build_fresh "$T" lumen-compositor; check "a built tree is fresh" 0 $?
build_fresh "$T" lumen-compositor lumen-shell; check "both targets fresh" 0 $?
build_fresh "$T" no-such-target; check "an unknown target cannot be judged" 2 $?
build_fresh "$TMP/nowhere" lumen-compositor; check "no build dir cannot be judged" 2 $?

touch "$T/s.cppm"
build_fresh "$T" lumen-compositor; check "a file only another binary links is not staleness (the dwfrun-5eff3d6b false trip)" 0 $?
build_fresh "$T" lumen-shell; check "its own binary is stale though" 1 $?
ninja -C "$T" lumen-shell >/dev/null 2>&1

touch "$T/a.cpp"
build_fresh "$T" lumen-compositor; check "a genuinely stale binary fails" 1 $?
ninja -C "$T" lumen-compositor >/dev/null 2>&1

touch "$T/CMakeLists.txt"
build_fresh "$T" lumen-compositor; check "build files newer than the manifest fail (re-run cmake)" 1 $?
# A real regeneration rewrites build.ninja; the synthetic REGEN's command is `true`, so
# touch the manifest where a real one would have been written.
touch "$T/build.ninja"
build_fresh "$T" lumen-compositor lumen-shell; check "fresh again after regenerating" 0 $?

echo "inject-devices.sh — the nodes found by name, env winning:"
# shellcheck source=../scripts/inject-devices.sh
source "$ROOT/scripts/inject-devices.sh"
S=$TMP/sys
mkdir -p "$S/class/input"
mkdev() { # NAME EVENT ABS REL — one input device in the fixture, laid out like sysfs
  local d=$S/class/input/input$2
  mkdir -p "$d/capabilities" "$d/event$2"
  echo "$1" >"$d/name"
  printf '%s\n' "$3" >"$d/capabilities/abs"
  printf '%s\n' "$4" >"$d/capabilities/rel"
  # /sys/class/input/eventN points at the device's event dir, whose device link is the device
  ln -s "input$2/event$2" "$S/class/input/event$2"
  ln -s ../ "$d/event$2/device"
}
mkdev "SynPS/2 Synaptics TouchPad" 9 660800011000003 0
mkdev "Lumen Inject Mouse" 10 3 100          # an older holder's device: wheel only
mkdev "Lumen Inject Mouse" 21 3 103          # re-enumerated, with REL_X/REL_Y
mkdev "AT Translated Set 2 keyboard" 4 0 0

unset LUMEN_INJECT_POINTER LUMEN_INJECT_KEYBOARD LUMEN_INJECT_RELATIVE
inject_pick_devices "$S" "$TMP/dev" >/dev/null
check "pointer: the REL-capable mouse wins over the older same-named one" "$TMP/dev/input/event21" "$POINTER_DEV"
check "relative: the same device (REL_X/REL_Y and wheel)" "$TMP/dev/input/event21" "$RELATIVE_DEV"
check "keyboard: found by name" "$TMP/dev/input/event4" "$KEYBOARD_DEV"

inject_node_has_rel_motion "$TMP/dev/input/event10" "$S"
check "an older mouse has no relative motion" 1 $?
inject_node_has_rel_motion "$TMP/dev/input/event21" "$S"
check "a current mouse has it" 0 $?

LUMEN_INJECT_POINTER=/dev/input/by-hand LUMEN_INJECT_KEYBOARD=/dev/input/by-hand-kbd \
LUMEN_INJECT_RELATIVE=/dev/input/by-hand-rel inject_pick_devices "$S" "$TMP/dev" >/dev/null
check "the environment overrides every node" "/dev/input/by-hand /dev/input/by-hand-kbd /dev/input/by-hand-rel" \
      "$POINTER_DEV $KEYBOARD_DEV $RELATIVE_DEV"
unset LUMEN_INJECT_POINTER LUMEN_INJECT_KEYBOARD LUMEN_INJECT_RELATIVE

rm -rf "$S"
mkdir -p "$S/class/input"
inject_pick_devices "$S" "$TMP/dev" >/dev/null
check "nothing found: the dev VM's nodes" "/dev/input/event2 /dev/input/event1 /dev/input/event3" \
      "$POINTER_DEV $KEYBOARD_DEV $RELATIVE_DEV"

echo "seat-session.sh — the run dir is an argument; the config comes back byte-identical:"
X=$TMP/xdg
mkdir -p "$X/lumen"
printf 'a = 1\n' >"$X/lumen/config.toml"
XDG_CONFIG_HOME=$X bash "$ROOT/scripts/seat-session.sh" config-backup "$TMP/runA" >/dev/null
printf 'a = 2\n' >"$X/lumen/config.toml"          # the first session's edit
# A second session with its own run dir, mid-way: the trampling case.
XDG_CONFIG_HOME=$X bash "$ROOT/scripts/seat-session.sh" config-backup "$TMP/runB" >/dev/null
XDG_CONFIG_HOME=$X bash "$ROOT/scripts/seat-session.sh" config-restore "$TMP/runA" >/dev/null
check "the config is restored byte-identical from its own run dir" "a = 1" "$(cat "$X/lumen/config.toml")"
check "a second session's run dir keeps its own backup" "a = 2" "$(cat "$TMP/runB/config-BEFORE.toml")"
check "the first run dir holds its own artifacts only" \
      "config-BEFORE.toml config-sha256-AFTER-TEST.txt config-sha256-BEFORE.txt config-sha256-RESTORED.txt" \
      "$(ls "$TMP/runA" | tr '\n' ' ' | sed 's/ $//')"
bash "$ROOT/scripts/seat-session.sh" precheck >/dev/null 2>&1
check "a command without its run dir is refused" 2 $?
bash "$ROOT/scripts/seat-session.sh" bogus "$TMP/runA" >/dev/null 2>&1
check "an unknown command is refused" 2 $?

echo "inject-input.py — the key table and the pixel mapping:"
python3 -I - "$ROOT/scripts/inject-input.py" <<'PY'
import importlib.util, re, sys

spec = importlib.util.spec_from_file_location("inject_input", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)  # only definitions: main() runs under __name__ == "__main__"

fails = 0

def check(name, want, got):
    global fails
    print(f"  {'ok  ' if want == got else 'FAIL'} {name}" + ("" if want == got else f" (want {want!r}, got {got!r})"))
    fails += want != got

check("KEY_EQUAL is evdev 13", 13, m.KEYS["KEY_EQUAL"])
try:
    codes = {}
    with open("/usr/include/linux/input-event-codes.h", encoding="utf-8") as f:
        for line in f:
            mm = re.match(r"#define (KEY_\w+)\s+(\d+)", line)
            if mm and not mm.group(1).endswith(("_MIN", "_MAX")):
                codes[mm.group(1)] = int(mm.group(2))
    bad = {k: (v, codes.get(k)) for k, v in m.KEYS.items() if codes.get(k) != v}
    check("every name carries its kernel evdev code", {}, bad)
except FileNotFoundError:
    print("  .    (no kernel header to check the table against)")

def mapped(inj, extent):
    """(px -> device value -> the pixel libinput floors it into), for one axis."""
    lo, hi = inj.range[0]
    def pixel(px):
        v = inj.to_device(px, extent, 0)
        return int((v - lo) * extent / (hi - lo + 1))
    return pixel

# The virtual mouse's 1:1 axes: every pixel must map to itself (pixel 1749 used to come
# out as 1750, and half the injected positions landed on a neighbour).
inj = m.Injector("p", "k", None, "")
inj.range = [(0, 3199), (0, 1799)]
one_to_one = mapped(inj, 3200)
bad = [px for px in range(3200) if one_to_one(px) != px]
check("a 1:1 axis hits every pixel exactly", [], bad[:5])
check("a 1:1 axis keeps fractional positions in their pixel", 1603, one_to_one(1603.33))

# The VMware-style fine axis: unchanged, the pixel aimed at.
inj.range = [(0, 65535), (0, 65535)]
fine = mapped(inj, 1914)
bad = [px for px in range(0, 1914, 7) if fine(px) != px]
check("a fine axis still hits the pixel aimed at", [], bad[:5])

# Consecutive hover pixels must stay distinct writes on a 1:1 device.
inj.range = [(0, 3199), (0, 1799)]
distinct = {inj.to_device(px, 3200, 0) for px in range(880, 884)}
check("consecutive pixels are distinct writes", 4, len(distinct))

sys.exit(1 if fails else 0)
PY
ab=$?
(( ab == 0 )) || fails=$((fails + 1))

echo
if (( fails == 0 )); then
  echo "smoke_harness: PASS"
  exit 0
fi
echo "smoke_harness: $fails failure(s)"
exit 1
