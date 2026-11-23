#!/usr/bin/env bash
# The smoke harness's own plumbing, tested without a seat: binary freshness against a
# synthetic Ninja tree (scripts/build-fresh.sh), injection-node lookup against a sysfs
# fixture (scripts/inject-devices.sh), inject-input.py's key table and pixel mapping,
# the bench warm-up gate against a fake bench (scripts/bench-warmup.sh), and ui-bench.sh's
# --perfetto traced pass against a stand-in bench (a hermetic copy of the script) under
# the real seat broker (scripts/seat-lock.sh), in a lock dir of the fixture's own.
# Everything here is hermetic: no /dev/input, no real build, no compositor — and nothing
# in the host's real lock dir, nothing read about what else runs on the host.
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

echo "bench-warmup.sh — ui-bench's host warm-up gate against a fake bench:"
# shellcheck source=../scripts/bench-warmup.sh
source "$ROOT/scripts/bench-warmup.sh"
contains() { # NAME TEXT NEEDLE — passes when TEXT contains NEEDLE
  [[ "$2" == *"$3"* ]] && pass "$1" || fail "$1 (missing '$3' in '$2')"
}
# The bench stand-in: each invocation pops the next p50 from $FAKE_SEQ (one line per
# run — probes and warm-up rounds alike, in order) and writes it as the run's S2 frame
# p50 to the --json path; "none" writes a run with no frame samples. $FAKE_LOG records
# "<json basename> <p50>" per run, so the cases can assert the probe/warm interleaving.
FAKE_BIN="$TMP/fake-ui-bench"
cat >"$FAKE_BIN" <<'EOF'
#!/usr/bin/env bash
set -u
json=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --json) json="$2"; shift 2 ;;
    *) shift ;;
  esac
done
p50="$(head -1 "$FAKE_SEQ")"
sed -i 1d "$FAKE_SEQ"
if [[ "$p50" == "none" ]]; then
  printf '[{"scenario": "S2", "stages": {"frame": {"samples": 0, "p50": 0}}}]' >"$json"
else
  printf '[{"scenario": "S1", "stages": {"frame": {"samples": 600, "p50": 9.5}}},\n{"scenario": "S2", "stages": {"frame": {"samples": 6000, "p50": %s}}}]' \
    "$p50" >"$json"
fi
echo "$(basename "$json") $p50" >>"$FAKE_LOG"
EOF
chmod +x "$FAKE_BIN"
FAKE_SEQ="$TMP/seq" FAKE_LOG="$TMP/fake-runs"
export FAKE_SEQ FAKE_LOG
# The gate's context: the globals scripts/ui-bench.sh has set where warm_host runs
# ($runner empty stands for a host without setarch).
BENCH="$FAKE_BIN" runner=() preset=test pin=3 warmup=120 frames="" compare=""
ref="$TMP/ref.json"
printf '[{"scenario": "S2", "stages": {"frame": {"samples": 36000, "p50": 36.74}}}]' >"$ref"
baseline="$ref"

printf '[{"scenario": "S1", "stages": {"frame": {"samples": 600, "p50": 9.5}}},\n{"scenario": "S2", "stages": {"frame": {"samples": 6000, "p50": 41.276}}}]' \
  >"$TMP/extract.json"
check "warm_s2_frame_p50: the S2 frame p50 behind an earlier row" "41.276" \
      "$(warm_s2_frame_p50 "$TMP/extract.json")"
printf '[{"scenario": "S2", "stages": {"frame": {"samples": 0, "p50": 0}}}]' >"$TMP/extract.json"
check "warm_s2_frame_p50: a zero-sample stage reads empty" "" "$(warm_s2_frame_p50 "$TMP/extract.json")"
check "warm_s2_frame_p50: a file that does not exist reads empty" "" \
      "$(warm_s2_frame_p50 "$TMP/no-such-file.json")"

: >"$FAKE_LOG"; printf '36.06\n' >"$FAKE_SEQ"
out="$(warm_host vulkan 2>&1)"; rc=$?
check "a probe at the baseline settles on probe 1" 0 $rc
contains "the settle threshold is 1.15x the baseline (36.74 -> 42.25)" "$out" "settles at <= 42.25"
check "settled on probe 1: one probe, no warm-up round" "probe-1.json 36.06" "$(cat "$FAKE_LOG")"

: >"$FAKE_LOG"; printf '71.0\n40.0\n36.0\n' >"$FAKE_SEQ"
out="$(warm_host vulkan 2>&1)"; rc=$?
check "a slow probe warms one round, then settles" 0 $rc
check "probe and warm-up rounds interleave, bounded (probe, warm, probe)" \
"probe-1.json 71.0
warm-1.json 40.0
probe-2.json 36.0" "$(cat "$FAKE_LOG")"

: >"$FAKE_LOG"; printf 'none\n' >"$FAKE_SEQ"
out="$(warm_host vulkan 2>&1)"; rc=$?
check "a probe with no S2 frame samples fails open (exit 0)" 0 $rc
contains "and says so" "$out" "probe 1 produced no S2 frame samples — measuring anyway"

: >"$FAKE_LOG"; printf '71.0\n70.0\n72.0\n70.0\n73.0\n' >"$FAKE_SEQ"
out="$(warm_host vulkan 2>&1)"; rc=$?
check "three unsettled probes refuse to measure (exit 2)" 2 $rc
contains "the refusal names the pole and the threshold" "$out" \
      "73.0 us against the baseline's 36.74 us (settle threshold 42.25)"
check "refused after 3 probes and 2 warm-up rounds, no more" 5 "$(wc -l <"$FAKE_LOG")"

printf '[{"scenario": "S2", "stages": {"frame": {"samples": 36000, "p50": 20.0}}}]' >"$TMP/ref-fast.json"
compare="$TMP/ref-fast.json"
: >"$FAKE_LOG"; printf '30.0\n' >"$FAKE_SEQ"
out="$(warm_host vulkan "$ref" 2>&1)"; rc=$?
check "an explicit settle reference beats --compare" 0 $rc
contains "its threshold comes from the argument (36.74 -> 42.25), not --compare's 23.00" \
      "$out" "settles at <= 42.25"
check "and settles on probe 1 only against that larger threshold" "probe-1.json 30.0" "$(cat "$FAKE_LOG")"
: >"$FAKE_LOG"; printf '21.0\n' >"$FAKE_SEQ"
out="$(warm_host vulkan 2>&1)"; rc=$?
check "without one, --compare stays the settle reference" 0 $rc
contains "its threshold comes from --compare (20.0 -> 23.00)" "$out" "settles at <= 23.00"
compare=""

printf '[{"scenario": "S5", "stages": {"frame": {"samples": 10, "p50": 3.0}}}]' >"$TMP/ref-no-s2.json"
baseline="$TMP/ref-no-s2.json"
: >"$FAKE_LOG"
out="$(warm_host gl 2>&1)"; rc=$?
check "a reference with no S2 frame p50 skips the warm-up" 0 $rc
contains "and says so" "$out" "no S2 frame p50"
check "skipped: nothing ran" "" "$(cat "$FAKE_LOG")"
baseline="$ref"

echo "ui-bench.sh --perfetto — a traced pass, never a measured one; under the seat broker:"
# A hermetic copy of the script (its ROOT decides where it reads git state and writes
# results) with a stand-in bench that logs the environment it was given and writes a
# minimal valid trace JSON to $LUMEN_TRACE_FILE. ui-bench.sh re-execs its ROOT's
# seat-lock.sh unless LUMEN_SEAT_LOCKED is set (scripts/ui-bench.sh:106-109), so the
# fixture carries a scripts/seat-lock.sh too — the one script it does not copy verbatim.
P=$TMP/perfetto
mkdir -p "$P/scripts" "$P/build" "$P/tests/bench/results" "$P/tests/bench/baseline"
# Verbatim: everything the copy reaches for under $ROOT/scripts/ but seat-lock.sh, the
# wrapper written below (the mechanism guard holds this list to what the copy references).
verbatim="ui-bench.sh bench-warmup.sh inject-input.py inject-devices.sh"
for f in $verbatim; do
  cp "$ROOT/scripts/$f" "$P/scripts/"
done
# The fixture's scripts/seat-lock.sh, where the copied ui-bench.sh re-execs
# `with bench ui-bench.sh -- env LUMEN_SEAT_LOCKED=1 bash …`. A verbatim copy of the
# tree's broker is not hermetic: its preflight greps /proc for compositors and smokes and
# takes the run down with exit 2 when it finds one (scripts/seat-lock.sh:85-127, 327-333),
# and its lock dir is derived from XDG_RUNTIME_DIR (:41) — unset, the host's real one.
# So this is a wrapper: it refuses every runtime dir but the fixture's private one,
# records the argv it was given, and execs the tree's real broker with --no-preflight
# (:309) ahead of the `--`: acquire, export, run and release for real, the host-dependent
# preflight off. The first lines are the harness's own: where the private dir, the tree's
# broker and the argv log are — fixed here, not read back from the environment.
SEAT="$P/runtime"                          # the fixture's private XDG_RUNTIME_DIR
BROKER_ARGV="$TMP/perfetto-broker-argv"    # one line per call reaching the wrapper
mkdir -m 700 "$SEAT"
: >"$BROKER_ARGV"
{
  echo '#!/usr/bin/env bash'
  printf 'private_runtime=%q\n' "$SEAT"
  printf 'broker=%q\n' "$ROOT/scripts/seat-lock.sh"
  printf 'argv_log=%q\n' "$BROKER_ARGV"
  cat <<'EOF'
# Written by tests/smoke_harness.sh into the perfetto fixture, for the copied ui-bench.sh
# to re-exec. See there: the real broker runs, against the fixture's private lock dir
# only, with the preflight off. Anything else is refused — exit 2, by name.
set -u
if [[ "${XDG_RUNTIME_DIR:-}" != "$private_runtime" ]]; then
  echo "seat-lock.sh (smoke_harness fixture): refusing — XDG_RUNTIME_DIR='${XDG_RUNTIME_DIR:-}' is not the fixture's private runtime dir" >&2
  exit 2
fi
printf '%s\n' "$*" >>"$argv_log"
opts=()
while [[ $# -gt 0 && "$1" != "--" ]]; do opts+=("$1"); shift; done
[[ $# -gt 0 ]] || { echo "seat-lock.sh (smoke_harness fixture): no -- before the job" >&2; exit 2; }
exec "$broker" "${opts[@]}" --no-preflight "$@"
EOF
} >"$P/scripts/seat-lock.sh"
chmod +x "$P/scripts/seat-lock.sh"

# Mechanism guard — the fixture is complete and verbatim. Everything the copied ui-bench.sh
# reaches for under $ROOT/scripts/, exec'd or sourced on any path, is staged: the next
# script to gain a re-exec like the seat broker's reds this harness by name, not with 13
# sub-FAILs of exit 127.
refs="$(grep -v '^[[:space:]]*#' "$P/scripts/ui-bench.sh" \
         | grep -oE '\$\{?ROOT\}?/scripts/[A-Za-z0-9._-]+' | sed 's|.*/||' | sort -u)"
missing=""
for name in $refs; do
  [[ -e "$P/scripts/$name" ]] || missing+=" $name"
done
check "staged: every \$ROOT/scripts/ reference of the copied ui-bench.sh ($(echo $refs))" "" "$missing"
altered=""
for f in $verbatim; do
  cmp -s "$ROOT/scripts/$f" "$P/scripts/$f" || altered+=" $f"
done
check "staged verbatim: the tree's scripts byte for byte (seat-lock.sh alone is the wrapper)" "" "$altered"

# A repo of its own: the trace (and JSON) names carry the tree's sha, and the script's
# `git rev-parse` is the first thing set -e would die on outside one.
git -C "$P" init -q -b main
git -C "$P" -c user.email=smoke@harness -c user.name=smoke_harness commit -q --allow-empty -m x
echo "CMAKE_BUILD_TYPE:STRING=Release" >"$P/build/CMakeCache.txt"
FAKE_PLOG="$TMP/perfetto-bench-runs"
export FAKE_PLOG
# The stand-in logs the seat it was given too: locked/class/ticket are what the broker
# exports to a job (scripts/seat-lock.sh:321), held=yes that the ticket it was given is
# the one held in the lock dir at that moment.
cat >"$P/build/lumen-ui-bench" <<'EOF'
#!/usr/bin/env bash
set -u
held="${XDG_RUNTIME_DIR:-}/lumen-seat/held"
printf 'trace=%s events=%s locked=%s class=%s held=%s ticket=%s\n' \
  "${LUMEN_TRACE_FILE:-unset}" "${LUMEN_TRACE_EVENTS:-unset}" \
  "${LUMEN_SEAT_LOCKED:-unset}" "${LUMEN_SEAT_CLASS:-unset}" \
  "$([[ -f "$held" && "$(sed -n 's/^id=//p' "$held")" == "${LUMEN_SEAT_TICKET:-}" ]] && echo yes || echo no)" \
  "${LUMEN_SEAT_TICKET:-unset}" >>"$FAKE_PLOG"
printf '{"displayTimeUnit":"ns","traceEvents":[{"ph":"M","name":"process_name","pid":1,"tid":1,"args":{"name":"lumen-ui-bench"}},{"ph":"X","cat":"bench","name":"S1","ts":1.0,"dur":2.0,"pid":1,"tid":1},{"ph":"C","name":"ui.frame_us","ts":1.0,"pid":1,"tid":1,"args":{"value":42}}]}\n' >"$LUMEN_TRACE_FILE"
EOF
chmod +x "$P/build/lumen-ui-bench"
# The probe's job (below): what a brokered process sees — the ticket held in its lock
# dir, and the three variables the broker exports to it.
cat >"$P/probe-job.sh" <<'EOF'
#!/usr/bin/env bash
set -u
held="$XDG_RUNTIME_DIR/lumen-seat/held"
echo "held: $(sed -n 's/^class=//p' "$held") $(sed -n 's/^owner=//p' "$held")" \
     "same-id-$([[ "$(sed -n 's/^id=//p' "$held")" == "${LUMEN_SEAT_TICKET:-}" ]] && echo yes || echo no)"
echo "job: class=${LUMEN_SEAT_CLASS:-unset} locked=${LUMEN_SEAT_LOCKED:-unset} ticket=${LUMEN_SEAT_TICKET:+set}"
EOF
prun() { # extra ui-bench.sh arguments; rc, the run's output in $TMP/prun.log
  # Outside any broker's environment, as a plain invocation is: ui-bench.sh has to take
  # its own ticket, and only ever in the fixture's private lock dir.
  env -u LUMEN_SEAT_LOCKED -u LUMEN_SEAT_TICKET -u LUMEN_SEAT_CLASS \
    XDG_RUNTIME_DIR="$SEAT" LUMEN_BUILD_DIR="$P/build" \
    bash "$P/scripts/ui-bench.sh" --perfetto --backend vulkan --no-build "$@" >"$TMP/prun.log" 2>&1
}
seat_state() { # the fixture's lock dir: held=<absent|present> queued=<n>
  echo "held=$([[ -e "$SEAT/lumen-seat/held" ]] && echo present || echo absent)" \
       "queued=$(ls -A "$SEAT/lumen-seat/queue" 2>/dev/null | wc -l)"
}
brokered_run() { # the last run, start to end: its proof lines in the run log, then the lock dir
  echo "$(grep -c 'seat-lock: acquired class bench owner ui-bench.sh' "$TMP/prun.log") $(seat_state)"
}
brokered() { # logged bench runs carrying what `with` exports (seat-lock.sh:321), on the ticket held
  grep -c ' locked=1 class=bench held=yes ticket=ticket-[0-9-]*[0-9]$' "$FAKE_PLOG"
}
tickets() { # the distinct ticket ids the logged bench runs were given
  grep -oE 'ticket=ticket-[0-9-]*[0-9]$' "$FAKE_PLOG" | sort -u | wc -l
}

: >"$FAKE_PLOG"
prun; rc=$?
(( rc == 0 )) || cat "$TMP/prun.log"
check "the traced pass runs every scenario and validates each trace" 0 "$rc"
check "one traced invocation per scenario" 4 "$(wc -l <"$FAKE_PLOG")"
contains "the bench got a per-scenario trace path and the event bound" "$(head -1 "$FAKE_PLOG")" \
        "-S1.trace.json events=2000000"
contains "the traces live under results/<machine>-<backend>/" "$(head -1 "$FAKE_PLOG")" \
        "/tests/bench/results/"
contains "each trace is parsed, its spans and counters counted" "$(cat "$TMP/prun.log")" "spans,"
contains "the pass names what it produced" "$(cat "$TMP/prun.log")" "never compared, never a baseline"
check "no baseline written" "" "$(ls "$P/tests/bench/baseline")"

# The broker contract (.zcode/skills/lumen-seat): ui-bench.sh re-execs its ROOT's
# seat-lock.sh as class bench under its own name, the job after a `--`; the broker takes
# a ticket, exports it to the job and releases it on exit.
check "the copied ui-bench.sh hands the re-exec to the fixture's seat-lock.sh: with, class bench, owner ui-bench.sh, --, the job" \
      "with bench ui-bench.sh -- env LUMEN_SEAT_LOCKED=1 bash $P/scripts/ui-bench.sh --perfetto --backend vulkan --no-build" \
      "$(cat "$BROKER_ARGV")"
check "every scenario ran brokered: LUMEN_SEAT_LOCKED=1, LUMEN_SEAT_CLASS=bench, the ticket held in the private lock dir" \
      4 "$(brokered)"
check "one ticket for the whole pass" 1 "$(tickets)"
check "brokered, and released on exit: the broker's proof line in the run log, the private lock dir free after" \
      "1 held=absent queued=0" "$(brokered_run)"
check "the preflight is off in the fixture: the run log reads nothing about the host" "" \
      "$(grep -E 'loadavg:|compositor, no smoke|preflight' "$TMP/prun.log")"

prun --update-baseline
check "--update-baseline is refused from a traced run" 2 $?
prun --client
check "--client is refused (per-process trace files)" 2 $?
prun --e2e
check "--e2e is refused" 2 $?
prun --compare "$TMP/ref.json"
check "--compare is refused (a traced pass feeds no comparison)" 2 $?
check "a refused run is brokered and released the same" "1 held=absent queued=0" "$(brokered_run)"

: >"$FAKE_PLOG"
prun; rc=$?
(( rc == 0 )) || cat "$TMP/prun.log"
check "a second traced run of the same tree still passes" 0 "$rc"
check "its traces take the -2 suffix, the first run's kept" 4 \
      "$(ls "$P"/tests/bench/results/*-vulkan/*-2-S*.trace.json 2>/dev/null | wc -l)"
check "eight trace files in total (two runs, four scenarios each)" 8 \
      "$(ls "$P"/tests/bench/results/*-vulkan/*.trace.json 2>/dev/null | wc -l)"
check "the second pass brokered and released the same" "1 held=absent queued=0" "$(brokered_run)"

# The same pass started from inside an outer broker's environment (a gate wrapped in
# `seat-lock.sh with`): nothing of the outer ticket reaches the fixture — the pass takes
# its own, as above.
: >"$FAKE_PLOG"
LUMEN_SEAT_LOCKED=1 LUMEN_SEAT_TICKET=ticket-outer LUMEN_SEAT_CLASS=smoke prun; rc=$?
(( rc == 0 )) || cat "$TMP/prun.log"
check "an outer brokered environment changes nothing: the pass still brokers itself and runs" 0 "$rc"
check "on its own ticket and class — none of the outer one's" 4 "$(brokered)"
check "brokered and released the same" "1 held=absent queued=0" "$(brokered_run)"

# The chain apart from ui-bench.sh: a job the wrapper is given runs under the tree's real
# broker, which holds the ticket in the private lock dir while it runs — the very id the
# job was given — and leaves nothing behind.
XDG_RUNTIME_DIR="$SEAT" bash "$P/scripts/seat-lock.sh" with smoke harness-probe -- \
  bash "$P/probe-job.sh" >"$TMP/probe.log" 2>&1; rc=$?
check "a job through the fixture's seat-lock.sh runs under the tree's real broker" 0 "$rc"
check "the ticket is held in the private lock dir while the job runs, the one it was given" \
      "held: smoke harness-probe same-id-yes
job: class=smoke locked=1 ticket=set" "$(grep -v '^seat-lock:' "$TMP/probe.log")"
check "and released after it" "held=absent queued=0" "$(seat_state)"

# Hermeticity: the wrapper serves its private runtime dir only. XDG_RUNTIME_DIR unset is
# the case to refuse hardest — the tree's broker would then derive the host's real lock
# dir (scripts/seat-lock.sh:41), where a seat job of the host's may hold a ticket: this
# ctest takes none there, and asserts nothing about it.
guard() { # "": XDG_RUNTIME_DIR unset; else that dir → rc, the output in $TMP/guard.log
  if [[ -z "$1" ]]; then
    env -u XDG_RUNTIME_DIR bash "$P/scripts/seat-lock.sh" with smoke harness-probe -- true
  else
    XDG_RUNTIME_DIR="$1" bash "$P/scripts/seat-lock.sh" with smoke harness-probe -- true
  fi >"$TMP/guard.log" 2>&1
}
: >"$BROKER_ARGV"
guard ""; rc=$?
check "XDG_RUNTIME_DIR unset: refused (exit 2) — it would mean the host's real lock dir" 2 "$rc"
contains "and says so" "$(cat "$TMP/guard.log")" "is not the fixture's private runtime dir"
guard "$TMP/xdg-foreign"; rc=$?
check "XDG_RUNTIME_DIR elsewhere: refused (exit 2)" 2 "$rc"
contains "and says so" "$(cat "$TMP/guard.log")" "is not the fixture's private runtime dir"
check "a refused call reaches no broker: nothing locked, nothing logged" "" \
      "$(ls -A "$TMP/xdg-foreign" 2>/dev/null)$(cat "$BROKER_ARGV")"

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
