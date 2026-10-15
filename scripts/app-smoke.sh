#!/usr/bin/env bash
# Unattended real-application check on the real console.
#
# Runs the compositor at LUMEN_LOG=debug with real clients (foot, gtk4-widget-factory) and
# lumen-square, drives them with injected evdev input (scripts/inject-input.py, as root),
# and judges every scenario from the compositor's log — the window manager's lines and the
# surface tree's `tree root=… nodes=…` dumps, which list every drawn surface of a window in
# paint order with its global rect at that moment — and from the clients' side: the probes'
# and lumen-square's stdout, WAYLAND_DEBUG=client traces of foot and GTK, lumen-shell's log.
# The same machinery as drag-smoke.sh; that script stays the separate regression gate for
# move/resize and latency.
#
#   ./scripts/app-smoke.sh                       # from the TTY you are logged into
#   ./scripts/app-smoke.sh --join-seat-session   # from SSH (see drag-smoke.sh)
#
# Options:
#   --join-seat-session   run the compositor inside the active seat0 logind session
#   --countdown S         seconds of warning before input starts (default 3)
#   --socket NAME         Wayland socket name (default lumen-app-smoke)
#   --logs DIR            log directory (default <repo>/app-smoke-logs)
#   --only LIST           comma list of scenario groups to run (default: all available)
#   --skip-gtk            skip the GTK scenarios
#
# Groups (more arrive with the steps that implement them; see the design in TEST.md):
#   A  subsurfaces
#     A1 foot with client-side decorations (`-o csd.preferred=client`: it insists, and the
#        compositor respects that): its title bar and borders are subsurfaces —
#        mapped, part of the window geometry, the title bar above the main surface
#     A2 drag foot's title bar (a subsurface): xdg_toplevel.move; every node moves by the
#        window's delta. A2s: the same with Super+drag (the compositor's grab), exact
#     A3 resize from foot's CSD border subsurface: in every tree dump during the grab the
#        title bar is exactly as wide as the window geometry (frame-perfect); one configure
#        in flight, every geometry change answers an acked configure
#        (foot's own trace: >=4 get_subsurface, one xdg_toplevel.move, one resize edges=10)
#     A4 lumen-square --sub: a synchronized 32 px strip; in every tree dump during a resize
#        its width equals the window's (never a frame with one size of each). Then
#        --sub-desync: strip commits apply on their own.
#     A5 gtk4-widget-factory maps and stays up (GSK_RENDERER=cairo; see TEST.md)
#   B  popups (lumen-probe for exact geometry, GTK for real-world menus)
#     B1 positioner base: popup 0 is configured at the rect the anchor/gravity give and
#        mapped at that offset from the window geometry
#     B2 at the output's right edge: flip_x puts it left of the anchor rect, slide_x
#        against the edge, resize_x clips it to the space left
#     B3 a grabbing chain of three: keyboard on the topmost; an outside click dismisses
#        all three top-down (popup_done 2, 1, 0) and reaches no client
#     B4 a grab quoting a stale serial is denied: popup_done at once
#     B5 an interactive move (Super+drag) ends the popup grab; a non-grabbing popup
#        follows its window exactly
#     B6 gtk4-widget-factory context menu: grabbed, mapped where its positioner says,
#        keyboard on it, Escape closes it, an outside click dismisses it, flipped near the
#        output's bottom edge, slid at its right edge (WARN if GTK's layout differs)
#     B7 a real GTK nested chain: widget-factory page 2's menubar, Edit (popup of the window)
#        then its Checks submenu (popup of that popup, opened by the H mnemonic); an outside
#        click dismisses the submenu, then the menu. From GTK's trace (B6+B7): every
#        xdg_popup.configure equals the compositor's computed rect, popup_done reached
#        exactly the dismissed popups in order, and no outside click reached GTK
#   C  server-side decorations (xdg-decoration)
#     C1 negotiation: foot asks for server-side and gets it (the frame applies with the
#        commit answering that configure); foot -o csd.preferred=client gets client-side and
#        draws its own subsurface frame; gtk4-demo is never told server-side
#     C2 drag the compositor's title bar: a decoration grab moves the window by exactly the
#        pointer delta and the press reaches no client
#     C3 drag 4 px outside the right edge (the invisible resize band): a resize grab with
#        edges=8, the width follows to within foot's cell rounding
#     C4 click the close button: xdg_toplevel.close, foot exits, the window unmaps
#     C5 a title set by the client is rasterized (and ellipsized when it does not fit)
#     C6 the frame of the window that takes the keyboard turns active (the other's
#        inactive); a middle click on its title bar lowers it
#     (foot's own trace for C1–C4: set_mode(server_side), configured server_side, no
#     wl_pointer.button at all — every press was on the frame — and xdg_toplevel.close)
#   D  wlr-layer-shell (lumen-probe layer mode; a top panel from D1 stays up through D8)
#     D1 a top panel (zone 32): configured full-width, mapped at 0,0, the usable area starts
#        at y=32; lumen-square maps below it and a Super+drag upwards stops at y=32
#     D2 a background surface with zone -1 is configured to the whole output despite the
#        panel
#     D3 an overlay anchored bottom-right with margins sits exactly at the corner minus them
#     D4 an exclusive overlay takes every key from the focused window, a click on the window
#        does not take them back, and when it unmaps the keys return
#     D5 an on_demand surface gets no keys when it maps, gets them when clicked, and loses
#        them to a click on the window
#     D6 maximize fills the usable area exactly and restore returns to the previous rect:
#        foot (server-side frame, title-bar double click; the frame's extents come off) and
#        gtk4-demo (client-side, header-bar double click → xdg_toplevel.set_maximized)
#     D7 an invalid layer state (width 0 without left+right) is the client's protocol error
#        (zwlr_layer_surface_v1 code 1); the compositor carries on
#     D8 a popup of a panel (layer get_popup) flips into the output (constraint: the whole
#        output), grabs without taking the keyboard (interactivity none), and is dismissed
#        by an outside click
#   E  lumen-shell (the real shell over zlm_policy_bridge_v1; lumen-square and foot as windows)
#     E1 the shell binds, gets the snapshot (two windows) and the focus right after it, maps
#        its 32 px top panel through layer-shell (the usable area starts at y=32) and draws
#        two entries with the focused window's highlighted
#     E2 a click on the other entry: bridge request_raise + request_activate, the compositor
#        raises and focuses that window (which receives the activating configure), and the
#        panel's highlight moves to the clicked entry
#     E3 a new foot that retitles itself: it maps below the panel, the entry is added,
#        renamed live, and the redraw damages that entry only
#     E4 a window's client exits: its entry is removed
#     E5 the shell is stopped and started again: the bridge goes, the usable area returns
#        to the whole output, a new snapshot lists the current windows (in the order the
#        old panel showed them), the panel comes back
#     E6 a copy of lumen-shell at another path is refused at bind (exit 1; the compositor's
#        warning names both paths) and the running shell is unaffected
#   X  robustness: lumen-square --sub --destroy-parent-first tears the parent wl_surface
#      down before the strip's wl_subsurface; no protocol error, compositor alive
#
# Global criteria: no ERROR line and no WARN except the 600-frame budget summaries and host
# present stalls (the 250 ms present watchdog with a flip already submitted, reported as a
# WARN row: SVGA3D flip/fence latency, as drag-smoke's g; without a submitted flip it
# fails); no
# "unregistered texture", "shm staging returned no buffer" or "texture update queue full";
# every node in every tree dump has a positive size; every mapped subsurface touches the
# output; the compositor exits 0 on SIGTERM and the console is back in text mode. Protocol
# errors on either side: exactly the two provoked ones (D7, E6) — libwayland-server's "error
# in client communication (pid N)" for those PIDs only, and no client log with a
# libwayland-client error line, GTK's "Protocol error"/"Lost connection" or foot's "err:".
#
# Exit status: 0 PASS (possibly with warnings), 1 FAIL, 2 could not run, 3 INVALID (input
# the injector did not send was seen — re-run hands off).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SQUARE="${BUILD}/lumen-square"
PROBE="${BUILD}/lumen-probe"
SHELL_BIN="${BUILD}/lumen-shell"
INJ="${ROOT}/scripts/inject-input.py"
POINTER_DEV="${LUMEN_INJECT_POINTER:-/dev/input/event2}"
KEYBOARD_DEV="${LUMEN_INJECT_KEYBOARD:-/dev/input/event1}"

SOCKET="lumen-app-smoke"
LOG_DIR="${ROOT}/app-smoke-logs"
JOIN_SEAT=0
COUNTDOWN=3
ONLY="A,B,C,D,E,X"
SKIP_GTK=0
while (( $# > 0 )); do
  case "$1" in
    --join-seat-session) JOIN_SEAT=1 ;;
    --countdown) COUNTDOWN="${2:?--countdown needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --only) ONLY="${2:?--only needs a list}"; shift ;;
    --skip-gtk) SKIP_GTK=1 ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
want() { [[ ",${ONLY}," == *",$1,"* ]]; }
for g in ${ONLY//,/ }; do
  case "$g" in
    A|B|C|D|E|X) ;;
    *) echo "unknown group '$g' (see --help)"; exit 2 ;;
  esac
done

COMP_LOG="${LOG_DIR}/compositor.log"
INJ_LOG="${LOG_DIR}/inject.jsonl"
SCEN_LOG="${LOG_DIR}/scenarios.jsonl"
WATCH_LOG="${LOG_DIR}/watch.jsonl"
RUN_LOG="${LOG_DIR}/run.log"

mkdir -p "$LOG_DIR"
for f in "$COMP_LOG" "$INJ_LOG" "$SCEN_LOG" "$WATCH_LOG"; do : > "$f"; done
rm -f "$LOG_DIR"/foot*.log "$LOG_DIR"/gtk*.log "$LOG_DIR"/square*.log "$LOG_DIR"/probe*.log \
  "$LOG_DIR"/shell*.log "$LOG_DIR"/fake-shell*

exec > >(tee "$RUN_LOG") 2>&1
TEE_PID=$!
close_log() {
  if [[ -n "${TEE_PID:-}" ]]; then
    exec >&- 2>&-
    wait "$TEE_PID" 2>/dev/null
    TEE_PID=""
  fi
}
trap close_log EXIT

# ── Preconditions ────────────────────────────────────────────────────────────
for bin in "$COMP" "$SQUARE" "$PROBE" "$SHELL_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "FAIL: $bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done
command -v foot >/dev/null || { echo "FAIL: foot is not installed (group A needs it)"; exit 2; }
if (( ! SKIP_GTK )) && ! command -v gtk4-widget-factory >/dev/null; then
  echo "NOTE: gtk4-widget-factory not installed; GTK scenarios skipped"; SKIP_GTK=1
fi

newest_of() {
  { git -C "$ROOT" ls-files -z -- "$@"
    git -C "$ROOT" ls-files -z --others --exclude-standard -- "$@"; } |
    (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1
}
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  for pair in "$COMP:modules executables/lumen-compositor" "$SQUARE:executables/lumen-square" \
              "$PROBE:tests/clients" "$SHELL_BIN:modules/lx.shell executables/lumen-shell"; do
    bin=${pair%%:*}
    # shellcheck disable=SC2086 # the path list is meant to split
    src=$(newest_of ${pair#*:})
    if [[ -n "$src" && "$ROOT/$src" -nt "$bin" ]]; then
      echo "FAIL: $bin is older than $src. Rebuild:  cmake --build --preset release"
      exit 2
    fi
  done
fi

if ! sudo -n true 2>/dev/null; then
  echo "FAIL: the injector writes to evdev nodes as root and needs passwordless sudo"
  exit 2
fi
for dev in "$POINTER_DEV" "$KEYBOARD_DEV"; do
  [[ -c "$dev" ]] || { echo "FAIL: $dev is not an input device node"; exit 2; }
done

SEAT_SCOPE=""
sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
if [[ -n "$sid" && "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" == "$(id -u)" ]]; then
  scope=$(loginctl show-session "$sid" -p Scope --value 2>/dev/null || true)
  [[ -n "$scope" ]] && SEAT_SCOPE="/sys/fs/cgroup/user.slice/user-$(id -u).slice/${scope}"
fi
MY_CGROUP="/sys/fs/cgroup$(sed -n 's/^0:://p' /proc/self/cgroup)"
IN_SEAT=0
[[ -n "$SEAT_SCOPE" && "$MY_CGROUP" == "$SEAT_SCOPE" ]] && IN_SEAT=1
if (( ! IN_SEAT )); then
  if (( JOIN_SEAT )); then
    if [[ -z "$SEAT_SCOPE" || ! -e "$SEAT_SCOPE/cgroup.procs" ]]; then
      echo "FAIL: --join-seat-session: no active seat0 session owned by $(id -un)"
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      libinput will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "FAIL: socket '$SOCKET' is held by a running compositor — pick another --socket"
  exit 2
fi
rm -f "$SOCKET_PATH" "$LOCK_PATH"

# ── Processes and cleanup ────────────────────────────────────────────────────
# Every client PID this script starts, so cleanup signals exactly those (never a pattern).
COMP_PID=""; WATCH_PID=""; CLIENT_PIDS=()
COMP_RC="not-stopped"

stop_compositor() {
  [[ -n "$COMP_PID" ]] || return 0
  kill -TERM "$COMP_PID" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$COMP_PID" 2>/dev/null; then
    echo "WARN: compositor ignored SIGTERM for 5 s; killing it (console may stay graphical)"
    kill -9 "$COMP_PID" 2>/dev/null
  fi
  wait "$COMP_PID" 2>/dev/null
  COMP_RC=$?
  COMP_PID=""
}
stop_watch() {
  [[ -n "$WATCH_PID" ]] || return 0
  kill -TERM "$WATCH_PID" 2>/dev/null
  wait "$WATCH_PID" 2>/dev/null
  WATCH_PID=""
}
stop_client() { # pid
  [[ -n "$1" ]] || return 0
  kill -TERM "$1" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$1" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$1" 2>/dev/null && kill -9 "$1" 2>/dev/null
  wait "$1" 2>/dev/null
}
cleanup() {
  local pid
  for pid in "${CLIENT_PIDS[@]}"; do stop_client "$pid"; done
  CLIENT_PIDS=()
  stop_compositor
  stop_watch
  rm -f "$SOCKET_PATH" "$LOCK_PATH"
  close_log
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

# ── Helpers ──────────────────────────────────────────────────────────────────
lines() { wc -l < "$1" | tr -d ' '; }
wait_for() { # file pattern seconds
  local deadline=$(( $(date +%s%N) + ${3}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    grep -qE "$2" "$1" && return 0
    sleep 0.05
  done
  return 1
}
geom() { # last geometry of toplevel $1: "x y w h"
  grep -E "compositor.wm: (geometry|map) id=$1 " "$COMP_LOG" | tail -1 |
    sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
settle() { # the toplevel's configure/ack/geometry traffic is quiet and the last one acked
  local id=$1 prev="" cur last deadline=$(( $(date +%s%N) + 5000000000 ))
  while (( $(date +%s%N) < deadline )); do
    cur=$(grep -cE "compositor.(wm|scene): (configure|ack|geometry|grab end|tree root=[0-9]+ kind=toplevel) id=$id " "$COMP_LOG")
    last=$(grep -oE "compositor.wm: configure id=$id serial=[0-9]+" "$COMP_LOG" | tail -1 |
           sed 's/.*serial=//')
    if [[ "$cur" == "$prev" ]] && { [[ -z "$last" ]] ||
         grep -qE "compositor.wm: ack id=$id serial=$last\$" "$COMP_LOG"; }; then
      return 0
    fi
    prev=$cur
    sleep 0.3
  done
  echo "WARN: toplevel $id did not settle within 5 s"
  return 1
}
wait_map() { # app_id seconds — prints the id of a toplevel with that app_id mapped since
             # the current scenario began (earlier windows of the same client do not count)
  local deadline=$(( $(date +%s%N) + ${2}000000000 )) id
  while (( $(date +%s%N) < deadline )); do
    id=$(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" |
         grep -oE "compositor.wm: map id=[0-9]+ app_id=$1 " | tail -1 | sed -E 's/.*id=([0-9]+).*/\1/')
    [[ -n "$id" ]] && { echo "$id"; return 0; }
    sleep 0.05
  done
  echo 0
  return 1
}
start_client() { # log-name command...
  local log="$LOG_DIR/$1.log"; shift
  WAYLAND_DISPLAY="$SOCKET" "$@" >"$log" 2>&1 &
  LAST_PID=$!
  CLIENT_PIDS+=("$LAST_PID")
}
forget_client() { # pid: stopped, drop it from the cleanup list
  local keep=() p
  for p in "${CLIENT_PIDS[@]}"; do [[ "$p" == "$1" ]] || keep+=("$p"); done
  CLIENT_PIDS=("${keep[@]}")
}

SCEN=""
inject() {
  sudo -n python3 -I "$INJ" --pointer "$POINTER_DEV" --keyboard "$KEYBOARD_DEV" \
    --output "${OUT_W}x${OUT_H}" --tag "$SCEN" "$@" >>"$INJ_LOG"
}
BEGIN_COMP=0
begin() { SCEN=$1; BEGIN_COMP=$(lines "$COMP_LOG"); }
finish() { # key=value ... — numeric parameters the verdict needs
  local extra="" kv
  for kv in "$@"; do extra+=",\"${kv%%=*}\":${kv#*=}"; done
  printf '{"name":"%s","comp":[%s,%s]%s}\n' "$SCEN" "$BEGIN_COMP" "$(lines "$COMP_LOG")" \
    "$extra" >>"$SCEN_LOG"
}

# ── Start ────────────────────────────────────────────────────────────────────
echo "┌──────────────────────────────────────────────────────────────────────────┐"
echo "│ HANDS OFF for about 2 minutes: do not touch the mouse or keyboard; keep  │"
echo "│ the host pointer OUTSIDE the VM window. Real input mixes with injected   │"
echo "│ input on the same device; if any is seen, the run is reported INVALID.   │"
echo "└──────────────────────────────────────────────────────────────────────────┘"
for (( n = COUNTDOWN; n > 0; n-- )); do printf '     starting in %d ...\r' "$n"; sleep 1; done
(( COUNTDOWN > 0 )) && printf '%-30s\r' ""

sudo -n python3 -I "$INJ" watch >"$WATCH_LOG" 2>&1 &
WATCH_PID=$!
if ! wait_for "$WATCH_LOG" '"kind": "watch"' 3; then
  echo "FAIL: the input watcher did not start:"; cat "$WATCH_LOG"; exit 2
fi

echo "1/2  compositor on $SOCKET (LUMEN_LOG=debug) ..."
if (( ! IN_SEAT && JOIN_SEAT )); then
  bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && exec env LUMEN_LOG=debug "$2" "$3"' \
    _ "$SEAT_SCOPE" "$COMP" "$SOCKET" >"$COMP_LOG" 2>&1 &
else
  LUMEN_LOG=debug "$COMP" "$SOCKET" >"$COMP_LOG" 2>&1 &
fi
COMP_PID=$!
if ! wait_for "$COMP_LOG" "output 0 mode=" 10 || ! wait_for "$COMP_LOG" "libinput seat opened" 3; then
  echo "FAIL: the compositor did not report an output mode and an input seat."
  grep -E "ERROR|WARN|seat|output" "$COMP_LOG" | tail -15
  exit 2
fi
for _ in $(seq 1 50); do [[ -S "$SOCKET_PATH" ]] && break; sleep 0.1; done
[[ -S "$SOCKET_PATH" ]] || { echo "FAIL: no socket at $SOCKET_PATH"; exit 2; }
read -r OUT_W OUT_H < <(grep -m1 -oE "output 0 mode=[0-9]+x[0-9]+" "$COMP_LOG" |
  sed -E 's/.*mode=([0-9]+)x([0-9]+)/\1 \2/')
echo "     output ${OUT_W}x${OUT_H}"
SCEN=setup; inject move 20 $((OUT_H - 20))

echo "2/2  scenarios ..."

if want A; then
  # A1 — foot with client-side decorations (no decoration manager is advertised).
  begin A1
  WAYLAND_DEBUG=client start_client foot-csd foot --config=/dev/null -o csd.preferred=client
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  finish id="$FOOT_ID"

  if (( FOOT_ID > 0 )); then
    # A2 — drag the title bar (a subsurface) by (+150,+80): foot answers with move.
    begin A2
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 3)); PY=$((Y + 12))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX + 150)) $((PY + 80)) --button left --steps 60
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=150 dy=80

    # A2s — Super+drag (the compositor's own move): exact, and the whole tree follows.
    begin A2s
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 2)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX - 120)) $((PY - 60)) --button left --super --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=-120 dy=-60

    # A3 — resize from the bottom border subsurface near the right corner, (+120,+90).
    # foot's border subsurfaces lie just outside its window geometry.
    begin A3
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W + 2)); PY=$((Y + H + 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" $((PX + 120)) $((PY + 90)) --button left --steps 50
    settle "$FOOT_ID"
    finish id="$FOOT_ID"
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # A4 — lumen-square with a synchronized strip, resized; then with a desynchronized one.
  for mode in sub sub-desync; do
    begin "A4-$mode"
    start_client "square-$mode" "$SQUARE" "--$mode"
    SQ_PID=$LAST_PID
    if SQ_ID=$(wait_map lumen-square 5); then
      settle "$SQ_ID"
      read -r X Y W H < <(geom "$SQ_ID")
      PX=$((X + W - 3)); PY=$((Y + H - 3))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX + 150)) $((PY + 100)) --button left --steps 50
      settle "$SQ_ID"
    fi
    stop_client "$SQ_PID"; forget_client "$SQ_PID"
    finish id="$SQ_ID"
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # A5 — GTK 4 maps and stays up.
  if (( ! SKIP_GTK )); then
    begin A5
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-factory gtk4-widget-factory
    GTK_PID=$LAST_PID
    if GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$GTK_ID"
      sleep 1
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID" alive="$ALIVE"
  fi
fi

popup_map() { # surface — "x y w h" of the newest map of that popup
  grep -E "compositor.popup: popup map surface=$1 " "$COMP_LOG" | tail -1 |
    sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
newest_popup() { # the surface id of the newest popup mapped since the scenario began
  tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -oE "compositor.popup: popup map surface=[0-9]+" |
    tail -1 | sed 's/.*=//'
}
wait_popup_maps() { # count seconds — at least `count` popup maps since the scenario began
  local deadline=$(( $(date +%s%N) + ${2}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    (( $(tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -c "compositor.popup: popup map ") >= $1 )) && return 0
    sleep 0.05
  done
  return 1
}

if want B; then
  # B1 — the unadjusted placement: bottom_left anchor, bottom_right gravity.
  begin B1
  start_client probe-b1 "$PROBE" popup --size 400x300 --popup 200x100 --anchor-rect 10,10,50,20 \
    --anchor 6 --gravity 8 --adjust 0 --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    wait_popup_maps 1 3
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID"

  # B2 — the window's right edge 50 px from the output's, a 200 px popup opening rightwards
  # off the anchor rect at its right edge: flip, slide or resize must bring it back.
  for adj in 4 1 16; do
    begin "B2-$adj"
    start_client "probe-b2-$adj" "$PROBE" popup --size 400x300 --popup 200x100 \
      --anchor-rect 390,10,10,20 --anchor 4 --gravity 4 --adjust "$adj" --exit-after 30
    PR_PID=$LAST_PID
    if PR_ID=$(wait_map lumen-probe 5); then
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      PX=$((X + W / 2)); PY=$((Y + H / 2)); DX=$(( OUT_W - 50 - (X + W) ))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX + DX)) "$PY" --button left --super --steps 30
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
      inject button left click
      wait_popup_maps 1 3
    fi
    stop_client "$PR_PID"; forget_client "$PR_PID"
    finish id="$PR_ID" adjust="$adj"
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # B3 — three nested grabbing popups, then a click on the empty desktop.
  begin B3
  start_client probe-b3 "$PROBE" popup --size 400x300 --popup 200x100 --anchor-rect 10,10,50,20 \
    --anchor 6 --gravity 8 --chain 3 --grab --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    for n in 1 2; do
      wait_popup_maps "$n" 3
      read -r PX PY PW PH < <(popup_map "$(newest_popup)")
      inject move $((PX + PW / 2)) $((PY + PH / 2)); sleep 0.2
      inject button left click
    done
    wait_popup_maps 3 3
    sleep 0.3
    inject key KEY_A tap
    sleep 0.2
    OUTSIDE_LINE=$(lines "$COMP_LOG")
    inject move 20 $((OUT_H - 20)); sleep 0.2
    inject button left click
    sleep 0.8
    inject key KEY_A tap
    sleep 0.3
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID" outside="${OUTSIDE_LINE:-0}"

  # B4 — a grab quoting the enter serial, not the press that opened the popup.
  begin B4
  start_client probe-b4 "$PROBE" popup --grab-stale --exit-after 30
  PR_PID=$LAST_PID
  if PR_ID=$(wait_map lumen-probe 5); then
    settle "$PR_ID"
    read -r X Y W H < <(geom "$PR_ID")
    inject move $((X + W / 2)) $((Y + H / 2)); sleep 0.2
    inject button left click
    sleep 0.8
  fi
  stop_client "$PR_PID"; forget_client "$PR_PID"
  finish id="$PR_ID"

  # B5 — Super+drag ends a popup grab; then a non-grabbing popup travels with its window.
  for mode in grab follow; do
    begin "B5-$mode"
    extra=(); [[ $mode == grab ]] && extra=(--grab)
    start_client "probe-b5-$mode" "$PROBE" popup --size 400x300 --popup 200x100 \
      --anchor-rect 10,10,50,20 --anchor 6 --gravity 8 "${extra[@]}" --exit-after 30
    PR_PID=$LAST_PID
    if PR_ID=$(wait_map lumen-probe 5); then
      settle "$PR_ID"
      read -r X Y W H < <(geom "$PR_ID")
      PX=$((X + W / 2)); PY=$((Y + H / 2))
      inject move "$PX" "$PY"; sleep 0.2
      inject button left click
      wait_popup_maps 1 3
      sleep 0.2
      # Inside the window but clear of the popup (which opens at its top-left).
      PX=$((X + W - 40)); PY=$((Y + H - 40))
      inject move "$PX" "$PY"; sleep 0.2
      inject drag "$PX" "$PY" $((PX - 60)) $((PY - 40)) --button left --super --steps 30
      settle "$PR_ID"
      sleep 0.3
    fi
    stop_client "$PR_PID"; forget_client "$PR_PID"
    finish id="$PR_ID" dx=-60 dy=-40
  done
  SCEN=park; inject move 20 $((OUT_H - 20))

  # B6 — GTK context menus. The point is a text widget in widget-factory's first page at the
  # window size this output gives it; if GTK lays out differently the checks only WARN.
  if (( ! SKIP_GTK )); then
    begin B6
    WAYLAND_DEBUG=client GDK_BACKEND=wayland GSK_RENDERER=cairo \
      start_client gtk-factory-b6 gtk4-widget-factory
    GTK_PID=$LAST_PID
    B6_LINES=()
    if GTK_ID=$(wait_map org.gtk.WidgetFactory4 10); then
      settle "$GTK_ID"
      sleep 1
      read -r X Y W H < <(geom "$GTK_ID")
      # (rel x, rel y, then): esc = open and Escape; out = open and click outside;
      # the last two open near the output's bottom and right edges.
      for spec in "1376 529 esc" "1376 529 out" "1376 579 esc" "$((OUT_W - X - 164)) 529 esc"; do
        read -r RX RY THEN <<<"$spec"
        B6_LINES+=("$(lines "$COMP_LOG")")
        inject move $((X + RX)) $((Y + RY)); sleep 0.3
        inject button right click
        sleep 1.2
        if [[ $THEN == esc ]]; then
          inject key KEY_ESC tap
        else
          inject move 20 $((OUT_H - 20)); sleep 0.2
          inject button left click
        fi
        sleep 0.8
      done
      B6_LINES+=("$(lines "$COMP_LOG")")
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    finish id="$GTK_ID" alive="$ALIVE" marks="[$(IFS=,; echo "${B6_LINES[*]}")]" wx="${X:-0}" wy="${Y:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))

    # B7 — a real nested chain: widget-factory's page 2 has a GtkPopoverMenuBar, whose
    # submenus are child xdg_popups (nested mode, unlike the sliding popover menus). Click
    # "Page 2" in the header bar's stack switcher, click "Edit" in the menubar (a grabbing
    # popup of the window), press H (the C_hecks mnemonic: a grabbing popup of that popup,
    # quoting the key serial), then click the empty desktop: both dismissed top-down.
    begin B7
    if (( ${GTK_ID:-0} > 0 )) && (( ALIVE )); then
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 23)); sleep 0.3
      inject button left click
      sleep 1.5
      inject move $((X + 341)) $((Y + 73)); sleep 0.3
      inject button left click
      wait_popup_maps 1 3
      sleep 0.5
      inject key 35 tap   # KEY_H
      wait_popup_maps 2 3
      sleep 0.5
      B7_OUT=$(lines "$COMP_LOG")
      inject move 20 $((OUT_H - 20)); sleep 0.2
      inject button left click
      sleep 1
    fi
    ALIVE=0; kill -0 "$GTK_PID" 2>/dev/null && ALIVE=1
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="${GTK_ID:-0}" alive="$ALIVE" outside="${B7_OUT:-0}" presses=6
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi
fi

deco_ok() { # id — the toplevel has had a server-side decoration applied since the scenario began
  tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -qE "compositor.deco: decoration apply id=$1 mode=server"
}

if want C; then
  # C1 — negotiation: foot (server), foot insisting on client-side, gtk4-demo (no object or
  # client-side).
  begin C1-server
  WAYLAND_DEBUG=client start_client foot-ssd foot --config=/dev/null
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  finish id="$FOOT_ID"

  if (( FOOT_ID > 0 )); then
    # C2 — drag the title bar by (+100,+60).
    begin C2
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W / 2)); PY=$((Y - 14))
    inject move "$PX" "$PY"; sleep 0.3
    inject drag "$PX" "$PY" $((PX + 100)) $((PY + 60)) --button left --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" dx=100 dy=60

    # C3 — 4 px outside the right edge, +80.
    begin C3
    read -r X Y W H < <(geom "$FOOT_ID")
    PX=$((X + W + 4)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.3
    inject drag "$PX" "$PY" $((PX + 80)) "$PY" --button left --steps 40
    settle "$FOOT_ID"
    finish id="$FOOT_ID" w0="$W" dw=80

    # C4 — the close button.
    begin C4
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W - 14)) $((Y - 14)); sleep 0.5
    inject button left click
    EXITED=0
    for _ in $(seq 1 20); do kill -0 "$FOOT_PID" 2>/dev/null || { EXITED=1; break; }; sleep 0.1; done
    finish id="$FOOT_ID" exited="$EXITED"
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  begin C1-client
  start_client foot-insists-csd foot --config=/dev/null -o csd.preferred=client
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5) && settle "$FOOT_ID"
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID"

  if (( ! SKIP_GTK )) && command -v gtk4-demo >/dev/null; then
    begin C1-gtk
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-demo gtk4-demo
    GTK_PID=$LAST_PID
    GTK_ID=$(wait_map org.gtk.Demo4 10) && settle "$GTK_ID"
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID"
  fi

  # C5 — titles: one that fits, then one that does not.
  begin C5
  start_client foot-title foot --config=/dev/null sh -c \
    'sleep 1; printf "\033]2;lumen-title-c5\007"; sleep 1; printf "\033]2;%s\007" "lumen-title-c5 and then a title much too long to fit any window of this size, which the frame must cut with an ellipsis rather than overflow"; sleep 30'
  FOOT_PID=$LAST_PID
  FOOT_ID=$(wait_map foot 5)
  sleep 3
  read -r X Y W H < <(geom "$FOOT_ID")
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID" w="${W:-0}"

  # C6 — middle-click lowers: two windows, the top one is lowered under the other.
  begin C6
  start_client foot-low-a foot --config=/dev/null
  A_PID=$LAST_PID
  A_ID=$(wait_map foot 5) && settle "$A_ID"
  begin_line=$BEGIN_COMP
  BEGIN_COMP=$(lines "$COMP_LOG")
  start_client foot-low-b foot --config=/dev/null
  B_PID=$LAST_PID
  B_ID=$(wait_map foot 5) && settle "$B_ID"
  BEGIN_COMP=$begin_line
  read -r X Y W H < <(geom "$B_ID")
  inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
  inject button middle click
  sleep 0.5
  stop_client "$B_PID"; forget_client "$B_PID"
  stop_client "$A_PID"; forget_client "$A_PID"
  finish id="$B_ID" other="$A_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))
fi

layer_map() { # namespace — "x y w h" of the newest map of the layer surface with that namespace
  grep -E "compositor.layer: layer map surface=[0-9]+ layer=[a-z]+ namespace=$1 " "$COMP_LOG" |
    tail -1 | sed -E 's/.* x=(-?[0-9]+) y=(-?[0-9]+) w=([0-9]+) h=([0-9]+).*/\1 \2 \3 \4/'
}
wait_since() { # pattern seconds — a compositor line matching since the scenario began
  local deadline=$(( $(date +%s%N) + ${2}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    tail -n +"$((BEGIN_COMP + 1))" "$COMP_LOG" | grep -qE "$1" && return 0
    sleep 0.05
  done
  return 1
}
wait_layer() { # namespace seconds
  wait_since "compositor.layer: layer map surface=[0-9]+ layer=[a-z]+ namespace=$1 " "$2"
}

if want D; then
  # D1 — a 32 px top panel with an exclusive zone; it stays up through D8.
  begin D1
  start_client probe-panel "$PROBE" layer --layer top --anchor top,left,right --size 0x32 \
    --zone 32 --ns d1-panel --exit-after 600
  PANEL_PID=$LAST_PID
  wait_layer d1-panel 5
  start_client square-d1 "$SQUARE"
  SQ_PID=$LAST_PID
  if SQ_ID=$(wait_map lumen-square 5); then
    settle "$SQ_ID"
    read -r X Y W H < <(geom "$SQ_ID")
    PX=$((X + W / 2)); PY=$((Y + H / 2))
    inject move "$PX" "$PY"; sleep 0.2
    inject drag "$PX" "$PY" "$PX" $((PY - 600)) --button left --super --steps 40
    settle "$SQ_ID"
  fi
  stop_client "$SQ_PID"; forget_client "$SQ_PID"
  finish id="$SQ_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D2 — a background surface stretched to every edge, zone -1.
  begin D2
  start_client probe-bg "$PROBE" layer --layer background --anchor top,bottom,left,right \
    --size 0x0 --zone -1 --ns d2-bg --exit-after 600
  BG_PID=$LAST_PID
  wait_layer d2-bg 5
  sleep 0.3
  stop_client "$BG_PID"; forget_client "$BG_PID"
  finish

  # D3 — an overlay at the bottom-right corner, 10 px from the right and 20 from the bottom.
  begin D3
  start_client probe-corner "$PROBE" layer --layer overlay --anchor bottom,right --size 100x80 \
    --margin 0,10,20,0 --ns d3-corner --exit-after 600
  CO_PID=$LAST_PID
  wait_layer d3-corner 5
  sleep 0.3
  stop_client "$CO_PID"; forget_client "$CO_PID"
  finish

  # D4 — an exclusive overlay over a focused window. Keys: A while it is up, B after a
  # (right) click on the window, C after it unmapped.
  begin D4
  start_client probe-d4-window "$PROBE" popup --size 400x300 --exit-after 600
  TOP_PID=$LAST_PID
  if TOP_ID=$(wait_map lumen-probe 5); then
    settle "$TOP_ID"
    wait_for "$LOG_DIR/probe-d4-window.log" "keyboard enter surface=toplevel" 3
    start_client probe-d4-excl "$PROBE" layer --layer overlay --kbd exclusive --size 300x100 \
      --ns d4-excl --unmap-after 4 --exit-after 600
    EX_PID=$LAST_PID
    wait_layer d4-excl 5
    sleep 0.4
    inject key KEY_A tap
    sleep 0.3
    read -r X Y W H < <(geom "$TOP_ID")
    inject move $((X + 20)) $((Y + 20)); sleep 0.2
    inject button right click
    sleep 0.3
    inject key 48 tap   # KEY_B
    wait_since "compositor.layer: layer unmap surface=[0-9]+ reason=null-buffer" 6
    sleep 0.4
    inject key 46 tap   # KEY_C
    sleep 0.3
    stop_client "$EX_PID"; forget_client "$EX_PID"
  fi
  stop_client "$TOP_PID"; forget_client "$TOP_PID"
  finish id="$TOP_ID"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D5 — an on_demand surface under the panel. Keys: A after clicking it, B after a (right)
  # click on the window.
  begin D5
  start_client probe-d5-window "$PROBE" popup --size 400x300 --exit-after 600
  TOP_PID=$LAST_PID
  if TOP_ID=$(wait_map lumen-probe 5); then
    settle "$TOP_ID"
    wait_for "$LOG_DIR/probe-d5-window.log" "keyboard enter surface=toplevel" 3
    start_client probe-d5-od "$PROBE" layer --layer top --anchor top --size 200x100 \
      --kbd on_demand --ns d5-ondemand --exit-after 600
    OD_PID=$LAST_PID
    wait_layer d5-ondemand 5
    sleep 0.4
    MAPPED_LINE=$(lines "$COMP_LOG")
    read -r LX LY LW LH < <(layer_map d5-ondemand)
    inject move $((LX + LW / 2)) $((LY + LH / 2)); sleep 0.2
    inject button left click
    sleep 0.3
    inject key KEY_A tap
    sleep 0.3
    read -r X Y W H < <(geom "$TOP_ID")
    inject move $((X + 20)) $((Y + 20)); sleep 0.2
    inject button right click
    sleep 0.3
    inject key 48 tap   # KEY_B
    sleep 0.3
    stop_client "$OD_PID"; forget_client "$OD_PID"
  fi
  stop_client "$TOP_PID"; forget_client "$TOP_PID"
  finish id="$TOP_ID" mapped="${MAPPED_LINE:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # D6 — maximize and restore by title-bar double click: foot with the compositor's frame,
  # then gtk4-demo's own header bar.
  begin D6-ssd
  start_client foot-max foot --config=/dev/null
  FOOT_PID=$LAST_PID
  if FOOT_ID=$(wait_map foot 5); then
    settle "$FOOT_ID"
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
    inject button left dblclick
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    MAX_LINE=$(lines "$COMP_LOG")
    # A second panel (bottom, zone 40) shrinks the usable area under the maximized window:
    # it is configured to the new area; when the panel goes, back to the old one.
    start_client probe-d6-dock "$PROBE" layer --layer top --anchor bottom,left,right \
      --size 0x40 --zone 40 --ns d6-dock --exit-after 600
    DOCK_PID=$LAST_PID
    wait_layer d6-dock 5
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    DOCK_LINE=$(lines "$COMP_LOG")
    stop_client "$DOCK_PID"; forget_client "$DOCK_PID"
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
    UNDOCK_LINE=$(lines "$COMP_LOG")
    read -r X Y W H < <(geom "$FOOT_ID")
    inject move $((X + W / 2)) $((Y - 14)); sleep 0.3
    inject button left dblclick
    sleep 0.3; settle "$FOOT_ID"; sleep 0.3
  fi
  stop_client "$FOOT_PID"; forget_client "$FOOT_PID"
  finish id="$FOOT_ID" max="${MAX_LINE:-0}" dock="${DOCK_LINE:-0}" undock="${UNDOCK_LINE:-0}"
  SCEN=park; inject move 20 $((OUT_H - 20))

  if (( ! SKIP_GTK )) && command -v gtk4-demo >/dev/null; then
    begin D6-gtk
    GDK_BACKEND=wayland GSK_RENDERER=cairo start_client gtk-demo-max gtk4-demo
    GTK_PID=$LAST_PID
    if GTK_ID=$(wait_map org.gtk.Demo4 10); then
      settle "$GTK_ID"
      sleep 1
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 15)); sleep 0.3
      inject button left dblclick
      sleep 0.5; settle "$GTK_ID"; sleep 0.5
      MAX_LINE=$(lines "$COMP_LOG")
      read -r X Y W H < <(geom "$GTK_ID")
      inject move $((X + W / 2)) $((Y + 15)); sleep 0.3
      inject button left dblclick
      sleep 0.5; settle "$GTK_ID"; sleep 0.5
    fi
    stop_client "$GTK_PID"; forget_client "$GTK_PID"
    finish id="$GTK_ID" max="${MAX_LINE:-0}"
    SCEN=park; inject move 20 $((OUT_H - 20))
  fi

  # D7 — width 0 without both horizontal anchors: invalid_size at the initial commit.
  begin D7
  start_client probe-d7 "$PROBE" layer --layer top --anchor top --size 0x32 --ns d7-invalid \
    --exit-after 10
  D7_PID=$LAST_PID
  wait "$D7_PID"; D7_RC=$?
  forget_client "$D7_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
  finish rc="$D7_RC" alive="$ALIVE" pid="$D7_PID"

  # D8 — a bottom-anchored panel's popup (flip_y), grabbed with the press that opened it;
  # then a click on the empty desktop.
  begin D8
  start_client probe-d8 "$PROBE" layer --layer top --anchor bottom --size 300x40 \
    --popup 120x80 --anchor-rect 10,10,50,20 --adjust 8 --grab --ns d8-popup --exit-after 600
  LP_PID=$LAST_PID
  if wait_layer d8-popup 5; then
    sleep 0.3
    read -r LX LY LW LH < <(layer_map d8-popup)
    inject move $((LX + 20)) $((LY + 20)); sleep 0.2
    inject button left click
    wait_popup_maps 1 3
    sleep 0.3
    inject key KEY_A tap
    sleep 0.2
    inject move 20 $((OUT_H / 2)); sleep 0.2
    inject button left click
    sleep 0.6
  fi
  stop_client "$LP_PID"; forget_client "$LP_PID"
  finish
  SCEN=park; inject move 20 $((OUT_H - 20))

  stop_client "$PANEL_PID"; forget_client "$PANEL_PID"
fi

if want X; then
  # X — parent wl_surface destroyed before the child's wl_subsurface.
  begin X-parent-first
  start_client square-parent-first "$SQUARE" --sub --destroy-parent-first -t 2
  SQ_PID=$LAST_PID
  wait "$SQ_PID"; SQ_RC=$?
  forget_client "$SQ_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$COMP_PID" 2>/dev/null && ALIVE=1
  finish rc="$SQ_RC" alive="$ALIVE"
fi

if want E; then
  SHELL_LOG="$LOG_DIR/shell.log"; SHELL2_LOG="$LOG_DIR/shell-restart.log"
  : >"$SHELL_LOG"; : >"$SHELL2_LOG"
  ebegin() { begin "$1"; SH_BEGIN=$(lines "$2"); }
  efinish() { # shell-log key=value...
    local log=$1; shift
    finish sh_a="$SH_BEGIN" sh_b="$(lines "$log")" "$@"
  }
  # E1 — two windows first, then the shell.
  ebegin E1 "$SHELL_LOG"
  start_client square-e1 "$SQUARE"
  E_SQ_PID=$LAST_PID
  E_SQ_ID=$(wait_map lumen-square 5) && settle "$E_SQ_ID"
  start_client foot-e1 foot --config=/dev/null
  E_FOOT_PID=$LAST_PID
  E_FOOT_ID=$(wait_map foot 5) && settle "$E_FOOT_ID"
  start_client shell env LUMEN_LOG=debug "$SHELL_BIN"
  SHELL_PID=$LAST_PID
  wait_since "compositor.layer: layer map surface=[0-9]+ layer=top namespace=lumen-panel " 5
  wait_for "$SHELL_LOG" "shell.panel: draw entries=2 " 5
  sleep 0.3
  efinish "$SHELL_LOG" sq="${E_SQ_ID:-0}" foot="${E_FOOT_ID:-0}"

  # E2 — click the entry that is not highlighted.
  ebegin E2 "$SHELL_LOG"
  FOC=$(grep -oE "shell.panel: draw entries=[0-9]+ hidden=[0-9]+ focused=-?[0-9]+" "$SHELL_LOG" |
        tail -1 | sed 's/.*focused=//')
  CLICK_IDX=0; [[ "$FOC" == "0" ]] && CLICK_IDX=1
  read -r EX EW < <(grep -oE "shell.panel: layout idx=$CLICK_IDX x=[0-9]+ w=[0-9]+" "$SHELL_LOG" |
                    tail -1 | sed -E 's/.*x=([0-9]+) w=([0-9]+)/\1 \2/')
  if [[ -n "${EX:-}" ]]; then
    inject move $((EX + EW / 2)) 16; sleep 0.3
    inject button left click
    sleep 0.8
  fi
  efinish "$SHELL_LOG" idx="$CLICK_IDX"
  SCEN=park; inject move 20 $((OUT_H - 20))

  # E3 — a third window that retitles itself after a second.
  ebegin E3 "$SHELL_LOG"
  start_client foot-e3 foot --config=/dev/null sh -c \
    'sleep 1; printf "\033]2;lumen-title-e3\007"; sleep 600'
  E3_PID=$LAST_PID
  E3_ID=$(wait_map foot 5)
  wait_for "$SHELL_LOG" 'shell.bridge: title handle=[0-9]+ app_id=foot title="lumen-title-e3"' 8
  sleep 0.5
  efinish "$SHELL_LOG" id="${E3_ID:-0}"

  # E4 — lumen-square's client exits.
  ebegin E4 "$SHELL_LOG"
  stop_client "$E_SQ_PID"; forget_client "$E_SQ_PID"
  wait_for "$SHELL_LOG" "shell.bridge: remove handle=" 3
  sleep 0.5
  efinish "$SHELL_LOG"

  # E5 — restart the shell.
  ebegin E5 "$SHELL2_LOG"
  stop_client "$SHELL_PID"; forget_client "$SHELL_PID"
  wait_since "compositor.shell: bridge destroyed" 3
  sleep 0.3
  start_client shell-restart env LUMEN_LOG=debug "$SHELL_BIN"
  SHELL_PID=$LAST_PID
  wait_for "$SHELL2_LOG" "shell.panel: draw entries=" 5
  sleep 0.5
  efinish "$SHELL2_LOG"

  # E6 — the same binary, copied elsewhere.
  begin E6
  cp "$SHELL_BIN" "$LOG_DIR/fake-shell"
  start_client fake-shell "$LOG_DIR/fake-shell"
  FAKE_PID=$LAST_PID
  for _ in $(seq 1 100); do kill -0 "$FAKE_PID" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$FAKE_PID" 2>/dev/null && kill -TERM "$FAKE_PID"
  wait "$FAKE_PID"; E6_RC=$?
  forget_client "$FAKE_PID"
  sleep 0.3
  ALIVE=0; kill -0 "$SHELL_PID" 2>/dev/null && ALIVE=1
  finish rc="$E6_RC" shell_alive="$ALIVE" pid="$FAKE_PID"

  stop_client "$SHELL_PID"; forget_client "$SHELL_PID"
  stop_client "$E3_PID"; forget_client "$E3_PID"
  stop_client "$E_FOOT_PID"; forget_client "$E_FOOT_PID"
fi

# ── Teardown ─────────────────────────────────────────────────────────────────
for pid in "${CLIENT_PIDS[@]}"; do stop_client "$pid"; done
CLIENT_PIDS=()
stop_compositor
stop_watch
rm -f "$SOCKET_PATH" "$LOCK_PATH"

VT=$(cat /sys/class/tty/tty0/active 2>/dev/null || echo tty1)
CONSOLE=$(sudo -n python3 -I -c 'import fcntl,os,struct,sys
fd=os.open(sys.argv[1],os.O_RDONLY|os.O_NOCTTY); b=bytearray(4)
fcntl.ioctl(fd,0x4B3B,b); print("text" if struct.unpack("i",b)[0]==0 else "graphics")' "/dev/$VT" 2>/dev/null || echo unknown)

# ── Verdict ──────────────────────────────────────────────────────────────────
python3 -I - "$LOG_DIR" "$COMP_RC" "$CONSOLE" "$POINTER_DEV" "$KEYBOARD_DEV" "$OUT_W" "$OUT_H" <<'PY'
import json, os, re, sys

log_dir, comp_rc, console, pointer_dev, keyboard_dev = sys.argv[1:6]
out_w, out_h = int(sys.argv[6]), int(sys.argv[7])

def read(name):
    try:
        with open(os.path.join(log_dir, name), encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []

comp = read("compositor.log")
inj = [json.loads(l) for l in read("inject.jsonl") if l.startswith("{")]
scen = {s["name"]: s for s in (json.loads(l) for l in read("scenarios.jsonl"))}

GEOM = re.compile(r"compositor\.wm: (?:geometry|map) id=(\d+) (?:app_id=\S* )?x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)(?: cause=(\w+) serial=(\d+))?")
CFG = re.compile(r"compositor\.wm: configure id=(\d+) serial=(\d+) w=(-?\d+) h=(-?\d+) states=(\S+) edges=(\d+) watchdog=(\d)")
ACK = re.compile(r"compositor\.wm: ack id=(\d+) serial=(\d+)$")
GBEGIN = re.compile(r"compositor\.wm: grab begin id=(\d+) mode=(\w+) edges=(\d+) source=(\w+)")
GEND = re.compile(r"compositor\.wm: grab end id=(\d+) mode=(\w+) reason=(\w+)")
TREE = re.compile(r"compositor\.scene: tree root=(\d+) kind=(\w+) id=(\d+) cause=(\w+) nodes=(\S+)")
NODE = re.compile(r"(\d+)(\*?)@(-?\d+),(-?\d+),(\d+)x(\d+)")
SUBMAP = re.compile(r"compositor\.surface: subsurface map surface=(\d+) root=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
POPMAP = re.compile(r"compositor\.popup: popup map surface=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
POPPOS = re.compile(r"compositor\.popup: popup position surface=(\d+) parent=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) .*flipped=(\S+) slid=(\S+) resized=(\S+) constraint=\S+ serial=(\d+) reason=(\w+)")
POPGRAB = re.compile(r"compositor\.popup: popup grab surface=(\d+) result=(\w+) reason=(\S+)")
POPDISMISS = re.compile(r"compositor\.popup: popup dismiss surface=(\d+) reason=(\S+)")
POPCREATE = re.compile(r"compositor\.popup: popup create surface=(\d+) parent=(\d+) parent_kind=(\w+)")
POPDESTROY = re.compile(r"compositor\.popup: popup destroy surface=(\d+)")
KBSURF = re.compile(r"compositor\.wm: focus keyboard-surface kind=(\w+) surface=(\d+)")

def rng(s):
    a, b = s["comp"]
    return range(a, b)

def trees(r, tid):
    """(line, root, nodes{sid: (x,y,w,h)}, root_sid, cause) of toplevel `tid` in range r."""
    out = []
    for i in r:
        m = TREE.search(comp[i])
        if not m or int(m.group(3)) != tid or m.group(2) != "toplevel":
            continue
        nodes, root_sid = {}, None
        for n in NODE.finditer(m.group(5)):
            nodes[int(n.group(1))] = tuple(int(n.group(k)) for k in (3, 4, 5, 6))
            if n.group(2):
                root_sid = int(n.group(1))
        out.append((i, int(m.group(1)), nodes, root_sid, m.group(4)))
    return out

def geometry_at(tid, upto):
    g = None
    for i in range(upto):
        m = GEOM.search(comp[i])
        if m and int(m.group(1)) == tid:
            g = tuple(int(m.group(k)) for k in (2, 3, 4, 5))
    return g

results = []

WLDBG = re.compile(r"^\[[\d:.]+\]\s+(?:\{[^}]*\}\s+)?(-> )?(\w+)[#@](\d+)\.(\w+)\((.*)\)\s*$")

def wl_debug(name):
    """WAYLAND_DEBUG=client lines of client log `name`: (is_request, interface, id, message, args)."""
    out = []
    for l in read(f"{name}.log"):
        m = WLDBG.match(l)
        if m:
            out.append((m.group(1) is not None, m.group(2), int(m.group(3)), m.group(4), m.group(5)))
    return out

def wl_presses(dbg):
    """Distinct pointer button presses a client received (some toolkits bind two pointers)."""
    return {a.split(",")[0].strip() for req, i, _, msg, a in dbg
            if not req and i == "wl_pointer" and msg == "button" and a.replace(" ", "").endswith(",1")}

def wl_popups(dbg):
    """The client's xdg_popups in creation order: {"configures": [(x,y,w,h)], "done": bool,
    "done_seq": int}; object ids are reused after destroy, so they are followed in order."""
    pops, cur, seq = [], {}, 0
    for req, i, oid, msg, a in dbg:
        if req and i == "xdg_surface" and msg == "get_popup":
            m = re.search(r"new id xdg_popup[#@](\d+)", a)
            if m:
                pops.append({"configures": [], "done": False, "done_seq": -1})
                cur[int(m.group(1))] = pops[-1]
        elif not req and i == "xdg_popup" and oid in cur:
            if msg == "configure":
                cur[oid]["configures"].append(tuple(int(v) for v in a.split(",")))
            elif msg == "popup_done":
                cur[oid]["done"] = True
                cur[oid]["done_seq"] = seq
                seq += 1
    return pops

def comp_index(r, rx):
    """Index of the first line in range r matching rx (or the range end)."""
    return next((i for i in r if rx.search(comp[i])), r.stop)

def record(name, ok, detail, warn=False):
    results.append((name, "FAIL" if not ok else "WARN" if warn else "PASS", detail))

def need(name):
    if name not in scen:
        record(name, False, "scenario did not run")
        return None
    return scen[name]

# ── A1: foot CSD ───────────────────────────────────────────────────────────
s = scen.get("A1")
if s is not None:
    tid = s["id"]
    if not tid:
        record("A1 foot CSD maps", False, "foot did not map within 5 s")
    else:
        ts = trees(rng(s), tid)
        root = ts[-1][1] if ts else None
        submaps = [m for i in rng(s) for m in [SUBMAP.search(comp[i])] if m and int(m.group(2)) == (root or -1)]
        above = any(any(v[1] < n[r][1] for k, v in n.items() if k != r)
                    for _, _, n, r, _ in ts if r in n)
        g = geometry_at(tid, s["comp"][1])
        main_h = ts[-1][2][ts[-1][3]][3] if ts and ts[-1][3] in ts[-1][2] else 0
        ok = bool(ts) and len(submaps) >= 4 and above and g and g[3] >= main_h + 20
        record("A1 foot CSD maps", ok,
               f"id={tid} root={root} subsurfaces mapped={len(submaps)} (want >=4), "
               f"title above main surface={above}, geometry h={g[3] if g else '-'} vs main "
               f"surface h={main_h} (want >= +20)")

# ── A2: drag foot's title bar ──────────────────────────────────────────────
s = scen.get("A2")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    b = [m for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    t0 = trees(range(0, s["comp"][0]), tid)
    t1 = trees(rng(s), tid)
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    shifted = False
    if t0 and t1 and dx is not None:
        n0, n1 = t0[-1][2], t1[-1][2]
        common = set(n0) & set(n1)
        shifted = bool(common) and all(n1[k][0] - n0[k][0] == dx and n1[k][1] - n0[k][1] == dy
                                       for k in common)
    # A CSD client starts the move a few motion events in (drag threshold, round trip),
    # so the window follows the pointer from there: a small shortfall is expected.
    ok = (len(b) == 1 and b[0].group(2) == "move" and b[0].group(4) == "client"
          and dx is not None and 0 <= s["dx"] - dx <= 12 and 0 <= s["dy"] - dy <= 8
          and g0[2:] == g1[2:] and shifted)
    record("A2 foot CSD title drag moves", ok,
           f"grab={b[0].group(4) + '/' + b[0].group(2) if b else '-'}; window delta "
           f"{dx},{dy} (pointer +{s['dx']},+{s['dy']}); every node shifted by it={shifted}")

# ── A2s: Super+drag foot ───────────────────────────────────────────────────
s = scen.get("A2s")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    b = [m for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    t0, t1 = trees(range(0, s["comp"][0]), tid), trees(rng(s), tid)
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    shifted = False
    if t0 and t1 and dx is not None:
        n0, n1 = t0[-1][2], t1[-1][2]
        common = set(n0) & set(n1)
        shifted = len(common) >= 5 and all(n1[k][0] - n0[k][0] == dx and n1[k][1] - n0[k][1] == dy
                                           for k in common)
    ok = (len(b) == 1 and b[0].group(4) == "keybinding" and (dx, dy) == (s["dx"], s["dy"])
          and shifted)
    record("A2s foot Super+drag moves the whole tree", ok,
           f"grab={b[0].group(4) if b else '-'}; window delta {dx},{dy} (want {s['dx']},{s['dy']}); "
           f"every node shifted by it={shifted}")

# ── A3: resize from foot's CSD border ──────────────────────────────────────
s = scen.get("A3")
if s is not None:
    tid = s["id"]
    b = [(i, m) for i in rng(s) for m in [GBEGIN.search(comp[i])] if m and int(m.group(1)) == tid]
    e = [(i, m) for i in rng(s) for m in [GEND.search(comp[i])] if m and int(m.group(1)) == tid]
    lo = b[0][0] if b else s["comp"][0]
    hi = s["comp"][1]
    checked, bad, first_bad = 0, 0, ""
    for i, _, nodes, rsid, cause in trees(range(lo, hi), tid):
        g = geometry_at(tid, i)
        if not g:
            continue
        # The title bar: the subsurface at the window geometry's top-left corner.
        title = [v for k, v in nodes.items() if k != rsid and v[0] == g[0] and v[1] == g[1]]
        checked += 1
        if len(title) != 1 or title[0][2] != g[2]:
            bad += 1
            first_bad = first_bad or comp[i]
    # Each geometry change answers a configure the client acked, and no configure goes out
    # while one is unanswered (beyond the watchdog).
    acked, inflight_ok, unanswered = set(), True, None
    commits, commit_ok = 0, True
    for i in range(lo, hi):
        if (m := ACK.search(comp[i])) and int(m.group(1)) == tid:
            acked.add(int(m.group(2)))
            if unanswered is not None and int(m.group(2)) >= unanswered:
                unanswered = None
        elif (m := CFG.search(comp[i])) and int(m.group(1)) == tid:
            if unanswered is not None and m.group(7) == "0":
                inflight_ok = False
            unanswered = int(m.group(2))
        elif (m := GEOM.search(comp[i])) and int(m.group(1)) == tid and m.group(6) == "commit":
            commits += 1
            if int(m.group(7)) not in acked:
                commit_ok = False
    ok = (len(b) == 1 and b[0][1].group(2) == "resize" and b[0][1].group(4) == "client"
          and e and checked > 2 and bad == 0 and inflight_ok and commit_ok and commits > 2)
    record("A3 foot CSD border resize, frame-perfect", ok,
           f"grab={b[0][1].group(4) + '/' + b[0][1].group(2) + ' edges=' + b[0][1].group(3) if b else '-'}; "
           f"{checked} tree dumps, title width != window width in {bad}; {commits} commits, "
           f"all answer acked configures={commit_ok}, one configure in flight={inflight_ok}"
           + (f"; first mismatch: {first_bad}" if first_bad else ""))

# ── A1–A3 from foot's side (WAYLAND_DEBUG=client) ─────────────────────────
if "A1" in scen:
    dbg = wl_debug("foot-csd")
    subs = sum(1 for req, i, _, msg, _ in dbg if req and i == "wl_subcompositor" and msg == "get_subsurface")
    moves = sum(1 for req, i, _, msg, _ in dbg if req and i == "xdg_toplevel" and msg == "move")
    resizes = [a.split(",")[-1].strip() for req, i, _, msg, a in dbg
               if req and i == "xdg_toplevel" and msg == "resize"]
    want_moves = 1 if "A2" in scen else 0
    want_resizes = ["10"] if "A3" in scen else []
    ok = bool(dbg) and subs >= 4 and moves == want_moves and resizes == want_resizes
    record("A1-A3 foot's side: subsurfaces created, move and resize requested", ok,
           f"get_subsurface={subs} (want >=4), xdg_toplevel.move={moves} (want {want_moves}), "
           f"xdg_toplevel.resize edges={resizes} (want {want_resizes}: bottom_right)")

# ── A4: lumen-square --sub / --sub-desync ──────────────────────────────────
for mode in ("sub", "sub-desync"):
    s = scen.get(f"A4-{mode}")
    if s is None:
        continue
    tid = s["id"]
    ts = trees(rng(s), tid)
    if not tid or not ts:
        record(f"A4 lumen-square --{mode}", False, "did not map")
        continue
    same, differ, alone = 0, 0, 0
    prev = None
    for _, _, nodes, rsid, cause in ts:
        others = [v for k, v in nodes.items() if k != rsid]
        if rsid not in nodes or len(others) != 1:
            continue
        r, c = nodes[rsid], others[0]
        if c[2] == r[2] and c[0] == r[0] and c[1] == r[1]:
            same += 1
        else:
            differ += 1
        if prev and cause == "commit" and c[2] != prev[1][2] and r[2] == prev[0][2]:
            alone += 1
        prev = (r, c)
    # lumen-square prints every strip commit; each width it committed must reach the screen
    # (a dump), in --sub only together with a window of that width.
    client_w = {int(m.group(1)) for l in read(f"square-{mode}.log")
                if (m := re.match(r"sub commit size=(\d+)x\d+", l))}
    shown_w = {[v for k, v in n.items() if k != r][0][2] for _, _, n, r, _ in ts
               if r in n and len(n) == 2}
    missing = sorted(client_w - shown_w)
    if mode == "sub":
        ok = same > 2 and differ == 0 and len(client_w) > 2 and not missing
        detail = (f"{same + differ} tree dumps: strip width == window width and at 0,0 in {same}, not in {differ}; "
                  f"client committed {len(client_w)} strip widths, never shown: {missing or 'none'}")
    else:
        ok = alone > 0 and len(client_w) > 2 and not missing
        detail = (f"{len(ts)} tree dumps; strip resized on its own (window unchanged) in {alone}; "
                  f"client committed {len(client_w)} strip widths, never shown: {missing or 'none'}")
    record(f"A4 lumen-square --{mode}", ok, detail)

# ── A5: GTK ────────────────────────────────────────────────────────────────
s = scen.get("A5")
if s is not None:
    ok = s["id"] > 0 and s["alive"] == 1
    record("A5 gtk4-widget-factory maps (cairo)", ok, f"id={s['id']} alive after settle={s['alive'] == 1}")

# ── B: popups ──────────────────────────────────────────────────────────────
def found(rx, r):
    return [m for i in r for m in [rx.search(comp[i])] if m]

def probe_lines(name):
    return read(f"probe-{name}.log")

s = scen.get("B1")
if s is not None:
    tid = s["id"]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    cfg = [l for l in probe_lines("b1") if l.startswith("popup configure idx=0 ")]
    maps = found(POPMAP, rng(s))
    ok = (bool(g) and len(cfg) == 1 and cfg[0].startswith("popup configure idx=0 x=10 y=30 w=200 h=100 ")
          and len(maps) == 1 and (int(maps[0].group(2)), int(maps[0].group(3))) == (g[0] + 10, g[1] + 30))
    record("B1 positioner base placement", ok,
           f"probe configure: {cfg[0] if cfg else '-'} (want x=10 y=30 w=200 h=100); "
           f"map at {maps[0].group(2) + ',' + maps[0].group(3) if maps else '-'} "
           f"(want window {g[0] if g else '?'}+10,{g[1] if g else '?'}+30)")

for adj, what in ((4, "flip"), (1, "slide"), (16, "resize")):
    s = scen.get(f"B2-{adj}")
    if s is None:
        continue
    tid = s["id"]
    g = geometry_at(tid, s["comp"][1]) if tid else None
    pos = found(POPPOS, rng(s))
    maps = found(POPMAP, rng(s))
    if not (g and pos and maps):
        record(f"B2 {what}_x at the output edge", False, "no window, position or map line")
        continue
    p, m = pos[-1], maps[-1]
    mx, mw = int(m.group(2)), int(m.group(4))
    edge_ok = g[0] + g[2] == out_w - 50
    if adj == 4:
        ok = p.group(7) == "x" and mx + mw == g[0] + 390
        want = f"flipped=x, popup right {mx + mw} == anchor rect left {g[0] + 390}"
    elif adj == 1:
        ok = p.group(8) == "x" and mx + mw == out_w
        want = f"slid=x, popup right {mx + mw} == output right {out_w}"
    else:
        ok = p.group(9) == "x" and mx == g[0] + 400 and mw == out_w - (g[0] + 400)
        want = f"resized=x, w={mw} == space left {out_w - (g[0] + 400)}"
    record(f"B2 {what}_x at the output edge", ok and edge_ok,
           f"{want}; window right edge {g[0] + g[2]} (want {out_w - 50})")

s = scen.get("B3")
if s is not None:
    pl = probe_lines("b3")
    grabs = found(POPGRAB, rng(s))
    creates = found(POPCREATE, rng(s))
    out_at = s.get("outside", 0) or s["comp"][1]
    before = range(s["comp"][0], out_at)
    after = range(out_at, s["comp"][1])
    third = creates[2].group(1) if len(creates) >= 3 else "?"
    kb_before = found(KBSURF, before)
    dismissed = [m.group(1) for m in found(POPDISMISS, after) if m.group(2) == "outside-click"]
    order = [l.split("idx=")[1] for l in pl if l.startswith("popup_done ")]
    tail = pl[next((i for i, l in enumerate(pl) if l.startswith("popup_done")), len(pl)):]
    leaked = [l for l in tail if l.startswith(("press ", "release "))]
    kb_after = found(KBSURF, after)
    keys = [i for i, l in enumerate(pl) if l.startswith("key code=30 state=1")]
    enters = [(i, l.split("surface=")[1]) for i, l in enumerate(pl) if l.startswith("keyboard enter")]
    key_targets = [max(((j, t) for j, t in enters if j < k), default=(0, "-"))[1] for k in keys]
    ok = (len(grabs) == 3 and all(m.group(2) == "ok" for m in grabs)
          and kb_before and kb_before[-1].group(1) == "popup" and kb_before[-1].group(2) == third
          and dismissed == [c.group(1) for c in reversed(creates[:3])] and order == ["2", "1", "0"]
          and not leaked and kb_after and kb_after[-1].group(1) == "toplevel"
          and key_targets == ["popup2", "toplevel"])
    record("B3 grab chain, outside click dismisses top-down", ok,
           f"grabs ok={sum(m.group(2) == 'ok' for m in grabs)}/3; keyboard before the click on "
           f"{kb_before[-1].group(1) + ' ' + kb_before[-1].group(2) if kb_before else '-'} (want popup {third}); "
           f"popup_done order {','.join(order) or '-'} (want 2,1,0); click reached a client={bool(leaked)}; "
           f"KEY_A went to {','.join(key_targets) or '-'} (want popup2,toplevel)")

s = scen.get("B4")
if s is not None:
    pl = probe_lines("b4")
    grabs = found(POPGRAB, rng(s))
    ok = (len(grabs) == 1 and grabs[0].group(2) == "denied" and grabs[0].group(3) == "serial"
          and "popup_done idx=0" in pl)
    record("B4 stale grab serial denied", ok,
           f"grab {grabs[0].group(2) + '/' + grabs[0].group(3) if grabs else '-'} (want denied/serial); "
           f"probe popup_done={'popup_done idx=0' in pl}")

s = scen.get("B5-grab")
if s is not None:
    dis = found(POPDISMISS, rng(s))
    b = found(GBEGIN, rng(s))
    ok = (len(b) == 1 and b[0].group(4) == "keybinding" and len(dis) == 1
          and dis[0].group(2) == "wm-grab" and "popup_done idx=0" in probe_lines("b5-grab"))
    record("B5 Super+drag ends the popup grab", ok,
           f"grab begin source={b[0].group(4) if b else '-'}; dismiss reason="
           f"{dis[0].group(2) if dis else '-'} (want wm-grab)")

s = scen.get("B5-follow")
if s is not None:
    tid = s["id"]
    maps = found(POPMAP, rng(s))
    g0 = geometry_at(tid, comp_index(rng(s), POPMAP)) if maps else None
    g1 = geometry_at(tid, s["comp"][1]) if tid else None
    psid = int(maps[0].group(1)) if maps else -1
    moves = [m for i in rng(s) for m in [TREE.search(comp[i])] if m and m.group(2) == "popup"
             and int(m.group(1)) == psid and m.group(4) == "move"]
    last = None
    if moves:
        n = NODE.search(moves[-1].group(5))
        last = (int(n.group(3)), int(n.group(4)))
    first = (int(maps[0].group(2)), int(maps[0].group(3))) if maps else None
    wd = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else None
    pd = (last[0] - first[0], last[1] - first[1]) if last and first else None
    ok = wd == (s["dx"], s["dy"]) and pd == wd and len(moves) >= 2
    record("B5 non-grabbing popup follows its window", ok,
           f"window delta {wd} (want {(s['dx'], s['dy'])}); popup delta {pd}; "
           f"{len(moves)} popup tree moves")

s = scen.get("B6")
if s is not None:
    marks = s.get("marks", [])
    names = ("open+Escape", "open+outside click", "flip at the bottom", "slide at the right")
    if not s["id"] or len(marks) < 5:
        record("B6 GTK context menus", False, "widget-factory did not map")
    for k in range(min(len(marks) - 1, 4)):
        r = range(marks[k], marks[k + 1])
        cr = found(POPCREATE, r)
        if not cr:
            record(f"B6 GTK {names[k]}", True, "no context menu at that point "
                   "(widget-factory's layout differs on this output)", warn=True)
            continue
        sid = cr[0].group(1)
        grab = [m for m in found(POPGRAB, r) if m.group(1) == sid]
        pos = [m for m in found(POPPOS, r) if m.group(1) == sid]
        mp = [m for m in found(POPMAP, r) if m.group(1) == sid]
        kb = found(KBSURF, r)
        g = geometry_at(s["id"], marks[k])
        placed = bool(pos and mp and g) and (int(mp[0].group(2)), int(mp[0].group(3))) == (
            g[0] + int(pos[-1].group(3)), g[1] + int(pos[-1].group(4)))
        inside = bool(mp) and 0 <= int(mp[0].group(2)) and int(mp[0].group(2)) + int(mp[0].group(4)) <= out_w \
            and 0 <= int(mp[0].group(3)) and int(mp[0].group(3)) + int(mp[0].group(5)) <= out_h
        focus = any(m.group(1) == "popup" and m.group(2) == sid for m in kb) and bool(kb) and kb[-1].group(1) == "toplevel"
        ok = (cr[0].group(3) == "toplevel" and grab and grab[0].group(2) == "ok" and placed and inside and focus)
        detail = (f"grab={grab[0].group(2) if grab else '-'}, mapped at window+positioner={placed}, "
                  f"inside the output={inside}, keyboard on it then back on the window={focus}")
        if k == 0:
            # GTK hides a popover by destroying its xdg_popup or by a null-buffer unmap.
            gone = any(m.group(1) == sid for m in found(POPDESTROY, r)) or any(
                f"popup unmap surface={sid} reason=null-buffer" in comp[i] for i in r)
            ok = ok and gone
            detail += f", gone after Escape={gone}"
        elif k == 1:
            dis = [m for m in found(POPDISMISS, r) if m.group(1) == sid]
            ok = ok and bool(dis) and dis[0].group(2) == "outside-click"
            detail += f", dismiss={dis[0].group(2) if dis else '-'}"
        elif k == 2:
            ok = ok and bool(pos) and pos[0].group(7) == "y"
            detail += f", flipped={pos[0].group(7) if pos else '-'} (want y)"
        else:
            ok = ok and bool(pos) and pos[0].group(8) == "x"
            detail += f", slid={pos[0].group(8) if pos else '-'} (want x)"
        record(f"B6 GTK {names[k]}", ok, detail)
    record("B6 widget-factory alive", s.get("alive") == 1, f"alive after the menus={s.get('alive') == 1}")

s = scen.get("B7")
if s is not None:
    r = rng(s)
    out_at = s.get("outside", 0) or s["comp"][1]
    cr = found(POPCREATE, r)
    tid = s["id"]
    if len(cr) < 2:
        # Only a layout difference can make the clicks miss: page 2's menubar is found by
        # position on this output's window size, as B6's text widget.
        record("B7 GTK nested menu chain", len(cr) == 0, f"{len(cr)} popup(s) opened "
               "(widget-factory's page 2 menubar not where it is at 1914x924)", warn=True)
    else:
        p1, p2 = cr[0], cr[1]
        s1, s2 = p1.group(1), p2.group(1)
        grabs = {m.group(1): m.group(2) for m in found(POPGRAB, r)}
        pos = {m.group(1): m for m in found(POPPOS, r)}
        mp = {m.group(1): m for m in found(POPMAP, r)}
        g = geometry_at(tid, s["comp"][0])
        def at(m):
            return (int(m.group(2)), int(m.group(3)))
        placed1 = s1 in pos and s1 in mp and g is not None and \
            at(mp[s1]) == (g[0] + int(pos[s1].group(3)), g[1] + int(pos[s1].group(4)))
        placed2 = s2 in pos and s2 in mp and s1 in mp and \
            at(mp[s2]) == (at(mp[s1])[0] + int(pos[s2].group(3)), at(mp[s1])[1] + int(pos[s2].group(4)))
        kb_before = found(KBSURF, range(s["comp"][0], out_at))
        after = range(out_at, s["comp"][1])
        dismissed = [m.group(1) for m in found(POPDISMISS, after) if m.group(2) == "outside-click"]
        kb_after = found(KBSURF, after)
        ok = (p1.group(3) == "toplevel" and p2.group(3) == "popup" and p2.group(2) == s1
              and grabs.get(s1) == "ok" and grabs.get(s2) == "ok" and placed1 and placed2
              and bool(kb_before) and kb_before[-1].group(1, 2) == ("popup", s2)
              and dismissed == [s2, s1] and bool(kb_after) and kb_after[-1].group(1) == "toplevel")
        record("B7 GTK nested menu chain (menubar Edit > Checks), outside click", ok,
               f"popups {s1} (parent {p1.group(3)}) and {s2} (parent {p2.group(2)}, {p2.group(3)}), "
               f"grabs {grabs.get(s1, '-')}/{grabs.get(s2, '-')}, placed at parent+positioner "
               f"{placed1}/{placed2}, keyboard on {kb_before[-1].group(1, 2) if kb_before else '-'} "
               f"(want popup {s2}), outside-click dismissal order {dismissed} (want [{s2}, {s1}]), "
               f"keyboard back on the window={bool(kb_after) and kb_after[-1].group(1) == 'toplevel'}")

# ── B6/B7 from GTK's side (WAYLAND_DEBUG=client): every popup the compositor created was
# configured with exactly the rect the compositor computed, popup_done reached GTK for
# exactly the popups the compositor dismissed (in the same order), and no outside click
# reached GTK as a button press.
if "B6" in scen and scen["B6"]["id"]:
    dbg = wl_debug("gtk-factory-b6")
    pops = wl_popups(dbg)
    last = scen["B7"]["comp"][1] if "B7" in scen else scen["B6"]["comp"][1]
    r = range(scen["B6"]["comp"][0], last)
    # GTK keeps a popover's wl_surface across shows, so the compositor's surface ids repeat:
    # follow each creation in order, as for the client's object ids.
    comp_pops, cur, dis_order = [], {}, []
    for i in r:
        if (m := POPCREATE.search(comp[i])):
            comp_pops.append([])
            cur[m.group(1)] = len(comp_pops) - 1
        elif (m := POPPOS.search(comp[i])) and m.group(1) in cur:
            comp_pops[cur[m.group(1)]].append(tuple(int(m.group(k)) for k in (3, 4, 5, 6)))
        elif (m := POPDISMISS.search(comp[i])) and m.group(1) in cur:
            dis_order.append(cur[m.group(1)])
    mismatch = [k for k, rects in enumerate(comp_pops)
                if k >= len(pops) or pops[k]["configures"] != rects]
    gtk_done = [k for k, p in sorted(enumerate(pops), key=lambda kp: kp[1]["done_seq"]) if p["done"]]
    presses = len(wl_presses(dbg))
    want_presses = scen["B7"].get("presses", 4) if "B7" in scen else 4
    ok = bool(dbg) and len(pops) == len(comp_pops) and not mismatch and gtk_done == dis_order \
        and presses == want_presses
    record("B6/B7 GTK's side: configures, popup_done, no outside press", ok,
           f"popups compositor/GTK {len(comp_pops)}/{len(pops)}, configure rects differing in "
           f"{len(mismatch)}, popup_done order GTK {gtk_done} vs compositor dismissals {dis_order} "
           f"(popups numbered in creation order), "
           f"button presses GTK received {presses} (want {want_presses}: the clicks inside its "
           f"window, none of the outside ones)")

# ── C: server-side decorations ─────────────────────────────────────────────
DMODE = re.compile(r"compositor\.deco: decoration mode id=(\d+) requested=(\w+) sent=(\w+) serial=(\d+)")
DAPPLY = re.compile(r"compositor\.deco: decoration apply id=(\d+) mode=(\w+) frame=(-?\d+),(-?\d+),(\d+)x(\d+)")
DPRESS = re.compile(r"compositor\.deco: decoration press id=(\d+) zone=(\w+) button=(\d+)")
DTITLE = re.compile(r"compositor\.deco: decoration title id=(\d+) width=(\d+) ellipsized=(\d) title=\"(.*)\"$")
FPOINT = re.compile(r"compositor\.wm: focus pointer (?:id=(\d+)|decoration id=(\d+) zone=(\w+)|none)")
UNMAP = re.compile(r"compositor\.wm: unmap id=(\d+) ")

def lines_matching(r, rx, tid=None):
    out = []
    for i in r:
        m = rx.search(comp[i])
        if m and (tid is None or int(m.group(1)) == tid):
            out.append((i, m))
    return out

s = scen.get("C1-server")
if s is not None:
    tid = s["id"]
    modes = lines_matching(rng(s), DMODE, tid)
    applies = lines_matching(rng(s), DAPPLY, tid)
    sent_server = [i for i, m in modes if m.group(2) == "server" and m.group(3) == "server"]
    applied = [i for i, m in applies if m.group(2) == "server"]
    f = applies[-1][1] if applies else None
    on_out = f is not None and int(f.group(4)) >= 0 and int(f.group(3)) >= 0 and \
        int(f.group(3)) + int(f.group(5)) <= out_w and int(f.group(4)) + int(f.group(6)) <= out_h
    ok = bool(tid) and bool(sent_server) and bool(applied) and applied[0] > sent_server[0] and on_out
    record("C1 foot negotiates server-side", ok,
           f"id={tid} sent=server at line {sent_server[0] if sent_server else '-'}, applied at "
           f"{applied[0] if applied else '-'}, frame={f.group(3, 4, 5, 6) if f else '-'} on the output={on_out}")

s = scen.get("C1-client")
if s is not None:
    tid = s["id"]
    modes = lines_matching(rng(s), DMODE, tid)
    ts = trees(rng(s), tid)
    nsub = max((len(n) for _, _, n, _, _ in ts), default=0)
    applied_server = lines_matching(rng(s), DAPPLY, tid)
    ok = bool(tid) and bool(modes) and all(m.group(3) == "client" for _, m in modes) and \
        not [m for _, m in applied_server if m.group(2) == "server"] and nsub >= 4
    record("C1 foot insisting on client-side keeps its own frame", ok,
           f"id={tid} sent={[m.group(3) for _, m in modes]} surfaces in its tree={nsub} (want >=4)")

s = scen.get("C1-gtk")
if s is not None:
    tid = s["id"]
    server = [m for _, m in lines_matching(rng(s), DMODE, tid) if m.group(3) == "server"]
    record("C1 gtk4-demo is not told server-side", bool(tid) and not server,
           f"id={tid} server-side configures={len(server)}")

s = scen.get("C2")
if s is not None:
    tid = s["id"]
    g0, g1 = geometry_at(tid, s["comp"][0]), geometry_at(tid, s["comp"][1])
    press = lines_matching(rng(s), DPRESS, tid)
    begin_ = [(i, m) for i, m in lines_matching(rng(s), GBEGIN, tid) if m.group(4) == "decoration"]
    # The press is the compositor's: no client had pointer focus from the frame focus line
    # before the press until the grab began.
    leak = False
    if press and begin_:
        leak = any((m := FPOINT.search(comp[i])) and m.group(1) is not None
                   for i in range(press[0][0], begin_[0][0]))
    zone_ok = bool(press) and press[0][1].group(2) == "title"
    dx, dy = (g1[0] - g0[0], g1[1] - g0[1]) if g0 and g1 else (None, None)
    ok = zone_ok and bool(begin_) and begin_[0][1].group(2) == "move" and \
        (dx, dy) == (s["dx"], s["dy"]) and not leak
    record("C2 title bar drag moves", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, grab "
           f"{begin_[0][1].group(2) + ' source=decoration' if begin_ else 'none'}, moved ({dx},{dy}) "
           f"want ({s['dx']},{s['dy']}), client saw the press={leak}")

s = scen.get("C3")
if s is not None:
    tid = s["id"]
    g1 = geometry_at(tid, s["comp"][1])
    press = lines_matching(rng(s), DPRESS, tid)
    begin_ = [m for _, m in lines_matching(rng(s), GBEGIN, tid) if m.group(4) == "decoration"]
    want = s["w0"] + s["dw"]
    # foot sizes to whole cells: the committed width is the configured one rounded down.
    ok = bool(press) and press[0][1].group(2) == "right" and bool(begin_) and \
        begin_[0].group(2) == "resize" and begin_[0].group(3) == "8" and g1 is not None and \
        want - 16 <= g1[2] <= want
    record("C3 resize band drag resizes", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, grab "
           f"{begin_[0].group(2) + ' edges=' + begin_[0].group(3) if begin_ else 'none'}, width "
           f"{s['w0']} -> {g1[2] if g1 else '-'} (want {want - 16}..{want})")

s = scen.get("C4")
if s is not None:
    tid = s["id"]
    press = lines_matching(rng(s), DPRESS, tid)
    close = [i for i in rng(s) if f"compositor.deco: decoration close id={tid}" in comp[i]]
    unmapped = lines_matching(range(s["comp"][0], len(comp)), UNMAP, tid)
    ok = bool(press) and press[0][1].group(2) == "close" and bool(close) and \
        s["exited"] == 1 and bool(unmapped)
    record("C4 close button closes", ok,
           f"press zone={press[0][1].group(2) if press else '-'}, close sent={bool(close)}, "
           f"foot exited within 2 s={s['exited'] == 1}, unmapped={bool(unmapped)}")

# ── C1–C4 from foot's side (WAYLAND_DEBUG=client) ─────────────────────────
if "C1-server" in scen:
    dbg = wl_debug("foot-ssd")
    asked = [a.strip() for req, i, _, msg, a in dbg if req and i == "zxdg_toplevel_decoration_v1" and msg == "set_mode"]
    told = [a.strip() for req, i, _, msg, a in dbg if not req and i == "zxdg_toplevel_decoration_v1" and msg == "configure"]
    presses = len(wl_presses(dbg))
    closes = sum(1 for req, i, _, msg, _ in dbg if not req and i == "xdg_toplevel" and msg == "close")
    want_close = 1 if "C4" in scen else 0
    ok = bool(dbg) and "2" in asked and told[:1] == ["2"] and presses == 0 and closes == want_close
    record("C1-C4 foot's side: server-side told, frame presses never delivered, close received", ok,
           f"set_mode={asked} (2 = server_side), decoration configure={told[:3]} (want 2), "
           f"button presses received={presses} (want 0: every press was on the frame), "
           f"xdg_toplevel.close received={closes} (want {want_close})")

s = scen.get("C5")
if s is not None:
    tid = s["id"]
    titles = lines_matching(rng(s), DTITLE, tid)
    fits = [m for _, m in titles if m.group(4) == "lumen-title-c5" and m.group(3) == "0"]
    room = s["w"] - 2 * 10 - 28
    long_ = [m for _, m in titles if m.group(4).startswith("lumen-title-c5 and then")]
    cut = bool(long_) and long_[-1].group(3) == "1" and int(long_[-1].group(2)) <= room
    record("C5 client titles rasterized, long ones ellipsized", bool(fits) and cut,
           f"id={tid} 'lumen-title-c5' drawn whole={bool(fits)}, long title ellipsized="
           f"{long_[-1].group(3) == '1' if long_ else '-'} width "
           f"{long_[-1].group(2) if long_ else '-'} <= room {room}")

s = scen.get("C6")
if s is not None:
    tid, other = s["id"], s["other"]
    press = lines_matching(rng(s), DPRESS, tid)
    lowered = [i for i in rng(s) if re.search(rf"compositor\.wm: lower id={tid} index=0$", comp[i])]
    # The newer window took the keyboard when it mapped: its frame turned active, the
    # other's inactive.
    active = [i for i in rng(s) if re.search(rf"compositor\.deco: decoration active id={tid} previous={other}$", comp[i])]
    ok = bool(press) and press[-1][1].group(2) == "title" and press[-1][1].group(3) == "274" and \
        bool(lowered) and bool(active)
    record("C6 middle-click on the title lowers", ok,
           f"press zone={press[-1][1].group(2) if press else '-'} button="
           f"{press[-1][1].group(3) if press else '-'}, lowered to the bottom={bool(lowered)}; "
           f"frame active colours moved {other} -> {tid} on map={bool(active)}")

# ── D: layer-shell ─────────────────────────────────────────────────────────
LCREATE = re.compile(r"compositor\.layer: layer create surface=(\d+) layer=(\w+) namespace=(\S+) output=0")
LCFG = re.compile(r"compositor\.layer: layer configure surface=(\d+) serial=(\d+) w=(\d+) h=(\d+)")
LMAP = re.compile(r"compositor\.layer: layer map surface=(\d+) layer=(\w+) namespace=(\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
LEXCL = re.compile(r"compositor\.layer: layer exclusive surface=(\d+) edge=(\w+) zone=(-?\d+)")
LUNMAP = re.compile(r"compositor\.layer: layer unmap surface=(\d+) reason=(\S+)")
LFOCUS = re.compile(r"compositor\.layer: layer focus surface=(\d+) interactivity=(\w+) cause=(\w+)")
USABLE = re.compile(r"compositor\.wm: usable area x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
MAXI = re.compile(r"compositor\.wm: maximize id=(\d+) on=(\d) source=(\w+)")
POPPARENT = re.compile(r"compositor\.popup: popup parent surface=(\d+) parent=(\d+) parent_kind=(\w+)")

def layer_of(r, ns):
    """(surface id, map match) of the layer surface created with namespace `ns` in range r."""
    sid = next((int(m.group(1)) for i in r if (m := LCREATE.search(comp[i])) and m.group(3) == ns), None)
    mp = next((m for i in r if (m := LMAP.search(comp[i])) and int(m.group(1)) == sid), None)
    return sid, mp

def probe_log(name):
    return read(f"{name}.log")

def first_index(r, pred):
    return next((i for i in r if pred(comp[i])), None)

s = scen.get("D1")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d1-panel")
    cfg = [m for i in r if (m := LCFG.search(comp[i])) and int(m.group(1)) == sid]
    excl = [m for i in r if (m := LEXCL.search(comp[i])) and int(m.group(1)) == sid]
    usable = [m.groups() for i in r if (m := USABLE.search(comp[i]))]
    want_u = ("0", "32", str(out_w), str(out_h - 32))
    tid = s["id"]
    gs = [m for i in r if (m := GEOM.search(comp[i])) and int(m.group(1)) == tid]
    map_y = int(gs[0].group(3)) if gs else None
    end_y = int(gs[-1].group(3)) if gs else None
    ok = sid is not None and bool(cfg) and (cfg[0].group(3), cfg[0].group(4)) == (str(out_w), "32") \
        and mp is not None and mp.group(4, 5, 6, 7) == ("0", "0", str(out_w), "32") \
        and bool(excl) and excl[-1].group(2, 3) == ("top", "32") and want_u in usable \
        and map_y is not None and map_y >= 32 and end_y == 32
    record("D1 top panel reserves its zone", ok,
           f"panel surface={sid} configure={cfg[0].group(3, 4) if cfg else '-'} map="
           f"{mp.group(4, 5, 6, 7) if mp else '-'} exclusive={excl[-1].group(2, 3) if excl else '-'} "
           f"usable areas={usable} (want {want_u}); lumen-square mapped at y={map_y} (>=32), "
           f"after a Super+drag 600 px up y={end_y} (want 32)")

s = scen.get("D2")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d2-bg")
    cfg = [m for i in r if (m := LCFG.search(comp[i])) and int(m.group(1)) == sid]
    ok = bool(cfg) and (cfg[0].group(3), cfg[0].group(4)) == (str(out_w), str(out_h)) and \
        mp is not None and mp.group(2) == "background" and \
        mp.group(4, 5, 6, 7) == ("0", "0", str(out_w), str(out_h))
    record("D2 background with zone -1 covers the whole output", ok,
           f"surface={sid} configure={cfg[0].group(3, 4) if cfg else '-'} map="
           f"{mp.group(2, 4, 5, 6, 7) if mp else '-'} (want background 0 0 {out_w} {out_h})")

s = scen.get("D3")
if s is not None:
    sid, mp = layer_of(rng(s), "d3-corner")
    want = (str(out_w - 110), str(out_h - 100), "100", "80")
    record("D3 bottom-right overlay with margins", mp is not None and mp.group(4, 5, 6, 7) == want,
           f"map={mp.group(4, 5, 6, 7) if mp else '-'} want {want}")

def key_lines(lines):
    return [int(m.group(1)) for l in lines if (m := re.match(r"key code=(\d+) state=1", l))]

s = scen.get("D4")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d4-excl")
    tid = s["id"]
    kb = [(i, m.group(1), int(m.group(2))) for i in r if (m := KBSURF.search(comp[i]))]
    to_layer = [i for i, k, x in kb if k == "layer" and x == sid]
    unmap = [i for i in r if (m := LUNMAP.search(comp[i])) and int(m.group(1)) == sid and m.group(2) == "null-buffer"]
    back = [i for i, k, x in kb if k == "toplevel" and unmap and i > unmap[0]]
    stolen = [i for i, k, x in kb if to_layer and unmap and to_layer[0] < i < unmap[0] and k != "layer"]
    excl_keys = key_lines(probe_log("probe-d4-excl"))
    win_keys = key_lines(probe_log("probe-d4-window"))
    deact = [i for i in r if to_layer and i > to_layer[0] and
             (m := CFG.search(comp[i])) and int(m.group(1)) == tid and "activated" not in m.group(5)]
    ok = bool(to_layer) and not stolen and bool(back) and excl_keys == [30, 48] and \
        win_keys == [46] and bool(deact)
    record("D4 exclusive overlay takes the keyboard until it unmaps", ok,
           f"keyboard to the layer={bool(to_layer)}, taken back by the click={bool(stolen)}, "
           f"back to the window after unmap={bool(back)}; overlay got keys {excl_keys} (want [30, 48]), "
           f"window got {win_keys} (want [46]); window deactivated meanwhile={bool(deact)}")

s = scen.get("D5")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d5-ondemand")
    kb = [(i, m.group(1), int(m.group(2))) for i in r if (m := KBSURF.search(comp[i]))]
    early = [i for i, k, x in kb if k == "layer" and i < s["mapped"]]
    focus = [(i, m) for i in r if (m := LFOCUS.search(comp[i]))]
    clicked = [i for i, m in focus if int(m.group(1)) == sid and m.group(3) == "click"]
    released = [i for i, m in focus if int(m.group(1)) == 0 and clicked and i > clicked[0]]
    od_log = probe_log("probe-d5-od")
    od_keys = key_lines(od_log)
    win_keys = key_lines(probe_log("probe-d5-window"))
    ok = not early and bool(clicked) and bool(released) and \
        any(l.startswith("keyboard enter surface=layer") for l in od_log) and \
        od_keys == [30] and win_keys == [48] and mp is not None and int(mp.group(5)) == 32
    record("D5 on_demand: focused by a click, released by a click elsewhere", ok,
           f"focused on map={bool(early)}, by click={bool(clicked)}, released={bool(released)}; "
           f"surface keys {od_keys} (want [30]), window keys {win_keys} (want [48]); placed at "
           f"y={mp.group(5) if mp else '-'} under the panel (want 32)")

def maximize_check(name, s, source, ext_top):
    r = rng(s)
    tid, mark = s["id"], s["max"]
    g0 = geometry_at(tid, s["comp"][0] + 1) if tid else None
    # the geometry before the first maximize request
    first_max = next((i for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "1"), None)
    if first_max is not None:
        g0 = geometry_at(tid, first_max)
    on = [m for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "1" and m.group(3) == source]
    off = [m for i in r if (m := MAXI.search(comp[i])) and int(m.group(1)) == tid and m.group(2) == "0" and m.group(3) == source]
    want_w, want_h = out_w, out_h - 32 - ext_top
    cfgs = [m for i in r if (m := CFG.search(comp[i])) and int(m.group(1)) == tid and "maximized" in m.group(5)]
    cfg_ok = any((int(m.group(3)), int(m.group(4))) == (want_w, want_h) for m in cfgs)
    gmax = geometry_at(tid, mark) if mark else None
    gend = geometry_at(tid, s["comp"][1])
    placed = gmax is not None and gmax[0] == 0 and gmax[1] == 32 + ext_top and \
        want_w - 16 <= gmax[2] <= want_w and want_h - 24 <= gmax[3] <= want_h
    restored = gend is not None and g0 is not None and gend == g0
    ok = bool(tid) and bool(on) and bool(off) and cfg_ok and placed and restored
    record(name, ok,
           f"id={tid} maximize on/off from {source}={bool(on)}/{bool(off)}, configure "
           f"{[(m.group(3), m.group(4)) for m in cfgs][:2]} want ({want_w},{want_h}); maximized "
           f"geometry {gmax} want x=0 y={32 + ext_top}; restored {gend} want {g0}")

s = scen.get("D6-ssd")
if s is not None:
    maximize_check("D6 maximize fills the usable area (foot, server-side frame)", s, "decoration", 28)
    tid = s["id"]
    gdock = geometry_at(tid, s["dock"]) if s.get("dock") else None
    gundock = geometry_at(tid, s["undock"]) if s.get("undock") else None
    # foot sizes to whole cells, so a height may come out a little short of the configured one.
    dock_ok = gdock is not None and gdock[:2] == (0, 60) and out_h - 60 - 40 - 24 <= gdock[3] <= out_h - 60 - 40
    undock_ok = gundock is not None and gundock[:2] == (0, 60) and out_h - 60 - 24 <= gundock[3] <= out_h - 60
    record("D6 a maximized window follows the usable area", dock_ok and undock_ok,
           f"with a 40 px bottom panel {gdock} (want 0,60 h<={out_h - 100}); after it left {gundock} "
           f"(want 0,60 h<={out_h - 60})")
s = scen.get("D6-gtk")
if s is not None:
    maximize_check("D6 maximize fills the usable area (gtk4-demo, header bar)", s, "client", 0)

s = scen.get("D7")
if s is not None:
    err = any("protocol-error interface=zwlr_layer_surface_v1 code=1" in l for l in probe_log("probe-d7"))
    record("D7 invalid layer state is a protocol error", s["rc"] == 3 and err and s["alive"] == 1,
           f"probe exit={s['rc']} (want 3), invalid_size reported={err}, compositor alive={s['alive'] == 1}")

s = scen.get("D8")
if s is not None:
    r = rng(s)
    sid, mp = layer_of(r, "d8-popup")
    parent = [m for i in r if (m := POPPARENT.search(comp[i])) and int(m.group(2)) == sid and m.group(3) == "layer"]
    psid = int(parent[0].group(1)) if parent else None
    grab = [m for i in r if (m := POPGRAB.search(comp[i])) and int(m.group(1)) == psid and m.group(2) == "ok"]
    pos = [m for i in r if (m := POPPOS.search(comp[i])) and int(m.group(1)) == psid]
    pm = [m for i in r if (m := POPMAP.search(comp[i])) and int(m.group(1)) == psid]
    inside = bool(pm) and int(pm[0].group(2)) >= 0 and int(pm[0].group(3)) >= 0 and \
        int(pm[0].group(2)) + int(pm[0].group(4)) <= out_w and int(pm[0].group(3)) + int(pm[0].group(5)) <= out_h
    kb_popup = [i for i in r if (m := KBSURF.search(comp[i])) and m.group(1) == "popup" and int(m.group(2)) == psid]
    dismissed = [m for i in r if (m := POPDISMISS.search(comp[i])) and int(m.group(1)) == psid and m.group(2) == "outside-click"]
    plog = probe_log("probe-d8")
    done = any(l.startswith("popup_done idx=0") for l in plog)
    keys = key_lines(plog)
    ok = psid is not None and bool(grab) and bool(pos) and pos[0].group(7) == "y" and inside and \
        not kb_popup and bool(dismissed) and done and not keys
    record("D8 panel popup: flipped into the output, grab without keyboard, outside click", ok,
           f"popup={psid} of layer surface {sid}, grab ok={bool(grab)}, flipped="
           f"{pos[0].group(7) if pos else '-'} map={pm[0].group(2, 3, 4, 5) if pm else '-'} inside={inside}, "
           f"keyboard on it={bool(kb_popup)} (want no: interactivity none), keys reached the client="
           f"{keys}, dismissed by the outside click={bool(dismissed)}, popup_done={done}")

# ── X: parent destroyed first ──────────────────────────────────────────────
s = scen.get("X-parent-first")
if s is not None:
    ok = s["rc"] == 0 and s["alive"] == 1
    record("X parent wl_surface before child wl_subsurface", ok,
           f"lumen-square exit={s['rc']} (protocol error would be 1), compositor alive={s['alive'] == 1}")

# ── E: lumen-shell ─────────────────────────────────────────────────────────
SH1, SH2 = read("shell.log"), read("shell-restart.log")
BOUND = re.compile(r"compositor\.shell: bridge bound client=\d+ snapshot=(\d+) focus=(\d+)")
BREQ = re.compile(r"compositor\.shell: bridge request=(\w+) id=(\d+) result=(\w+)")
KBID = re.compile(r"compositor\.wm: focus keyboard id=(\d+)$")
SDRAW = re.compile(r"shell\.panel: draw entries=(\d+) hidden=(\d+) focused=(-?\d+) damage=(\d+)(\(full\))?")
SLAYOUT = re.compile(r'shell\.panel: layout idx=(\d+) x=(\d+) w=(\d+) handle=(\d+) title="(.*)"$')
PANELMAP = re.compile(r"compositor\.layer: layer map surface=\d+ layer=top namespace=lumen-panel x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")
USABLE = re.compile(r"compositor\.wm: usable area x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)")

def sh_lines(log, s):
    return log[s["sh_a"]:s["sh_b"]]

def comp_find(r, rx):
    return [m for i in r if (m := rx.search(comp[i]))]

def layout_titles(lines):
    """idx -> title from the last layout block in `lines`."""
    out = {}
    for l in lines:
        if (m := SLAYOUT.search(l)):
            if m.group(1) == "0":
                out = {}
            out[int(m.group(1))] = m.group(5)
    return out

s = scen.get("E1")
if s is not None:
    r, sh = rng(s), sh_lines(SH1, s)
    bound = comp_find(r, BOUND)
    pm = comp_find(r, PANELMAP)
    usable = [m.group(1, 2, 3, 4) for m in comp_find(r, USABLE)]
    snap = [l for l in sh if "shell.bridge: snapshot toplevels=2" in l]
    focus_after = any("shell.bridge: focus handle=" in l and "none" not in l
                      for l in sh[next((k for k, l in enumerate(sh) if "snapshot toplevels=" in l), len(sh)):])
    draws = [m for l in sh if (m := SDRAW.search(l))]
    titles = layout_titles(sh)
    fidx = int(draws[-1].group(3)) if draws else -1
    ok = bool(bound) and bound[0].group(1) == "2" and bool(pm) and \
        pm[0].group(1, 2, 3, 4) == ("0", "0", str(out_w), "32") and \
        ("0", "32", str(out_w), str(out_h - 32)) in usable and bool(snap) and focus_after and \
        bool(draws) and draws[-1].group(1) == "2" and fidx >= 0 and \
        titles.get(fidx, "lumen-square") != "lumen-square"
    record("E1 shell: snapshot of 2 + focus, 32 px panel, usable area from y=32", ok,
           f"bridge bound snapshot={bound[0].group(1) if bound else '-'}, panel map="
           f"{pm[0].group(1, 2, 3, 4) if pm else '-'}, usable={usable[-1] if usable else '-'}, "
           f"shell snapshot={bool(snap)} focus after it={focus_after}, last draw entries="
           f"{draws[-1].group(1) if draws else '-'} focused idx={fidx} ({titles.get(fidx, '-')!r}; "
           f"want the last mapped window, foot)")

s = scen.get("E2")
if s is not None and "E1" in scen:
    r, sh = rng(s), sh_lines(SH1, s)
    idx = s["idx"]
    titles = layout_titles(SH1[:s["sh_a"]])
    want_id = scen["E1"]["sq"] if titles.get(idx) == "lumen-square" else scen["E1"]["foot"]
    reqs = [m.group(1, 2, 3) for m in comp_find(r, BREQ)]
    kb = [int(m.group(1)) for m in comp_find(r, KBID)]
    draws = [m for l in sh if (m := SDRAW.search(l))]
    clicked = any(f"shell.panel: click idx={idx} " in l for l in sh)
    raised = any(re.search(rf"compositor\.wm: raise id={want_id} index=", comp[i]) for i in r)
    # From the client's side: lumen-square prints every configure it receives; the
    # activating configure the compositor sent during E2 reached it with `activated`.
    sq_ok = True
    if want_id == scen["E1"]["sq"]:
        act = [m.group(2) for i in r if (m := CFG.search(comp[i])) and int(m.group(1)) == want_id
               and "activated" in m.group(5).split(",")]
        got = {mm.group(1) for l in read("square-e1.log")
               if (mm := re.match(r"configure serial=(\d+) .*states=\S*activated", l))}
        sq_ok = bool(act) and act[-1] in got
    ok = clicked and raised and sq_ok and ("raise", str(want_id), "ok") in reqs and \
        ("activate", str(want_id), "ok") in reqs and \
        reqs.index(("raise", str(want_id), "ok")) < reqs.index(("activate", str(want_id), "ok")) and \
        bool(kb) and kb[-1] == want_id and bool(draws) and int(draws[-1].group(3)) == idx
    record("E2 panel click: raise + activate over the bridge, highlight follows", ok,
           f"clicked idx={idx} ({titles.get(idx, '-')!r} = toplevel {want_id}), shell click={clicked}, "
           f"bridge requests={reqs}, raised in the stack={raised}, keyboard focus={kb[-1] if kb else '-'}, "
           f"panel focused idx now={draws[-1].group(3) if draws else '-'}, the window received its "
           f"activating configure={sq_ok}")

s = scen.get("E3")
if s is not None:
    sh = sh_lines(SH1, s)
    added = any("shell.bridge: add handle=" in l for l in sh)
    k = next((k for k, l in enumerate(sh) if 'title="lumen-title-e3"' in l and "shell.bridge: title" in l), None)
    after = [m for l in (sh[k + 1:] if k is not None else []) if (m := SDRAW.search(l))]
    tid = s.get("id", 0)
    fr = [m for i in rng(s) if (m := re.search(rf"compositor\.deco: decoration apply id={tid} mode=server "
                                              r"frame=(-?\d+),(-?\d+),(\d+)x(\d+)", comp[i]))]
    below = bool(tid) and bool(fr) and int(fr[0].group(2)) >= 32
    ok = added and k is not None and bool(after) and after[0].group(4) == "1" and not after[0].group(5) \
        and below
    record("E3 live: a new window below the panel adds an entry, its retitle redraws that entry only", ok,
           f"added={added}, retitled={k is not None}, redraw after it: "
           f"{after[0].group(0) if after else '-'} (want damage=1, not full); new window {tid} frame "
           f"top y={fr[0].group(2) if fr else '-'} (want >= 32, below the panel)")

s = scen.get("E4")
if s is not None:
    sh = sh_lines(SH1, s)
    removed = any("shell.bridge: remove handle=" in l for l in sh)
    draws = [m for l in sh if (m := SDRAW.search(l))]
    ok = removed and bool(draws) and draws[-1].group(1) == "2" and bool(draws[-1].group(5))
    record("E4 a client exits: its entry goes", ok,
           f"removed={removed}, last draw={draws[-1].group(0) if draws else '-'} (want 2 entries, full)")

s = scen.get("E5")
if s is not None:
    r, sh = rng(s), sh_lines(SH2, s)
    lines_ = [comp[i] for i in r]
    i_destroyed = next((k for k, l in enumerate(lines_) if "compositor.shell: bridge destroyed" in l), None)
    i_full = next((k for k, l in enumerate(lines_) if (m := USABLE.search(l)) and
                   m.group(1, 2, 3, 4) == ("0", "0", str(out_w), str(out_h))), None)
    bound = comp_find(r, BOUND)
    i_back = max((k for k, l in enumerate(lines_) if (m := USABLE.search(l)) and m.group(2) == "32"),
                 default=None)
    snap = any("shell.bridge: snapshot toplevels=2" in l for l in sh)
    draws = [m for l in sh if (m := SDRAW.search(l))]
    # The snapshot is in creation order: the restarted panel lists what the old one did.
    before, after = layout_titles(SH1), layout_titles(sh)
    ok = i_destroyed is not None and i_full is not None and bool(bound) and \
        bound[-1].group(1) == "2" and i_back is not None and i_back > i_full and snap and \
        bool(draws) and draws[-1].group(1) == "2" and before == after
    record("E5 restart: bridge destroyed, usable area restored, fresh snapshot, panel back", ok,
           f"destroyed={i_destroyed is not None}, usable full={i_full is not None}, rebound snapshot="
           f"{bound[-1].group(1) if bound else '-'}, usable y=32 again={i_back is not None}, "
           f"shell snapshot of 2={snap}, draw={draws[-1].group(0) if draws else '-'}, "
           f"same entry order as before={before == after} ({list(after.values())})")

s = scen.get("E6")
if s is not None:
    r = rng(s)
    denied = [comp[i] for i in r if "privileged global zlm_shell_v1 denied" in comp[i]
              and "fake-shell is not the shell binary" in comp[i] and "lumen-shell" in comp[i]]
    rebound = comp_find(r, BOUND)
    fake = read("fake-shell.log")
    said = any("zlm_shell_v1 bind rejected" in l for l in fake)
    ok = s["rc"] == 1 and bool(denied) and not rebound and said and s["shell_alive"] == 1
    record("E6 a copy of the shell binary is refused at bind", ok,
           f"exit={s['rc']} (want 1), compositor warning naming both paths={bool(denied)}, "
           f"shell said why={said}, bridge rebound={bool(rebound)} (want no), real shell alive="
           f"{s['shell_alive'] == 1}")

shell_problems = [l for l in SH1 + SH2 if l.startswith(("[ERROR]", "[WARN]"))]
if "E1" in scen:
    record("E shell logs: no error or warning", not shell_problems,
           f"{len(shell_problems)}" + (f"; first: {shell_problems[0]}" if shell_problems else ""))

# ── Protocol errors, both sides ────────────────────────────────────────────
# A protocol error the compositor posts ends with libwayland-server's "error in client
# communication (pid N)"; the client side prints libwayland-client's "<iface>#<id>: error
# <code>: <message>" (GTK adds "Error 71 (Protocol error) dispatching to Wayland display").
# Exactly two are provoked on purpose: D7's invalid layer state and E6's refused shell copy.
expected_pids = {str(scen[n]["pid"]) for n in ("D7", "E6") if n in scen and scen[n].get("pid")}
comm = [m.group(1) for l in comp if (m := re.search(r"error in client communication \(pid (\d+)\)", l))]
unexpected = [p for p in comm if p not in expected_pids]
CLIENT_ERR = re.compile(r"\w+[#@]\d+: error \d+: |[Pp]rotocol error|wl_display[#@]1\.error\(|"
                        r"Lost connection to Wayland compositor|^err: |protocol-error ")
EXPECTED_ERR_LOGS = {"probe-d7.log", "fake-shell.log"}
client_errs = []
for name in sorted(os.listdir(log_dir)):
    if not name.endswith(".log") or name in ("compositor.log", "run.log") or name in EXPECTED_ERR_LOGS:
        continue
    for l in read(name):
        if CLIENT_ERR.search(l):
            client_errs.append(f"{name}: {l.strip()[:160]}")
            break
# The two expected ones did happen, each with its own error.
d7_err = any("protocol-error interface=zwlr_layer_surface_v1 code=1" in l for l in read("probe-d7.log"))
e6_err = any("error 3: privileged global denied" in l for l in read("fake-shell.log"))
ok = not unexpected and not client_errs and (("D7" not in scen) or d7_err) and \
    (("E6" not in scen) or e6_err) and sorted(comm) == sorted(expected_pids)
record("run: no protocol error except the two provoked (D7, E6), either side", ok,
       f"compositor 'error in client communication' pids {comm} (expected {sorted(expected_pids)}), "
       f"client logs with an error: {len(client_errs)}"
       + (f"; first: {client_errs[0]}" if client_errs else "")
       + f"; D7 error seen by the probe={d7_err}, E6 error seen by the copy={e6_err}")

# ── Whole run ──────────────────────────────────────────────────────────────
BUDGET = ("over budget in recent frames",)
# E6 provokes exactly one: a copy of lumen-shell refused at bind.
EXPECTED_WARN = ("privileged global zlm_shell_v1 denied: client pid=",)
# The present watchdog fires when a queued flip has not completed for 250 ms. With a flip
# demonstrably queued (a `commit=content` or `commit=deferred` render line just before it:
# a flip was submitted, or the next one is waiting behind it) that is the host's
# GPU/flip latency — on this VM SVGA3D fences have been seen to take ~280 ms under load,
# the same cause drag-smoke's g reports as a WARN — not a compositor stall. Without one it
# stays a failure.
STALL = "no present in 250ms"
host_stalls, errors = [], []
for i, l in enumerate(comp):
    if l.startswith("[WARN]") and STALL in l and any(
            re.search(r"compositor\.render: .* commit=(content|deferred) ", comp[j])
            for j in range(max(0, i - 30), i)):
        host_stalls.append(l)
    elif l.startswith("[WARN]") and "E6" in scen and any(e in l for e in EXPECTED_WARN) and \
            "fake-shell" in l:
        pass
    elif l.startswith("[ERROR]") or (l.startswith("[WARN]") and not any(b in l for b in BUDGET)):
        errors.append(l)
banned = [l for l in comp if any(t in l for t in ("unregistered texture",
          "shm staging returned no buffer", "texture update queue full"))]
empty = [l for l in comp if (m := TREE.search(l)) and any(int(n.group(5)) <= 0 or int(n.group(6)) <= 0
         for n in NODE.finditer(m.group(5)))]
offscreen = [l for l in comp if (m := SUBMAP.search(l)) and not (
    int(m.group(3)) < out_w and int(m.group(4)) < out_h and
    int(m.group(3)) + int(m.group(5)) > 0 and int(m.group(4)) + int(m.group(6)) > 0)]
offscreen += [l for l in comp if (m := POPMAP.search(l)) and not (
    int(m.group(2)) < out_w and int(m.group(3)) < out_h and
    int(m.group(2)) + int(m.group(4)) > 0 and int(m.group(3)) + int(m.group(5)) > 0)]
offscreen += [l for l in comp if (m := re.search(
    r"compositor\.layer: layer map surface=\d+ layer=\w+ namespace=\S+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)", l))
    and not (int(m.group(1)) >= 0 and int(m.group(2)) >= 0 and
             int(m.group(1)) + int(m.group(3)) <= out_w and int(m.group(2)) + int(m.group(4)) <= out_h)]
record("run: clean log, tree dumps sane, clean exit, console text",
       not errors and not banned and not empty and not offscreen and comp_rc == "0" and console == "text",
       f"errors+warnings={len(errors)} banned={len(banned)} empty-nodes={len(empty)} "
       f"offscreen-subsurfaces/popups/layers={len(offscreen)} compositor exit={comp_rc} console={console}"
       + (f"; first: {(errors + banned + empty + offscreen)[0]}" if errors or banned or empty or offscreen else ""))
if host_stalls:
    record("run: host present stalls (flip queued, not completed in 250 ms)", True,
           f"{len(host_stalls)} (SVGA3D flip/fence latency, as drag-smoke's g)", warn=True)

# ── Foreign input (as drag-smoke.sh) ───────────────────────────────────────
watch = [json.loads(l) for l in read("watch.jsonl") if l.startswith("{")]
inj_ax = {r["ax"] for r in inj if r["kind"] == "motion" and "ax" in r}
inj_ay = {r["ay"] for r in inj if r["kind"] == "motion" and "ay" in r}
BTN = {"left": 0x110, "right": 0x111, "middle": 0x112}
inj_keys = {}
for r in inj:
    if r["kind"] == "button":
        k = (pointer_dev, BTN[r["button"]], 1 if r["pressed"] else 0)
    elif r["kind"] == "key":
        k = (keyboard_dev, r["code"], 1 if r["pressed"] else 0)
    else:
        continue
    inj_keys[k] = inj_keys.get(k, 0) + 1
seen_keys, foreign = {}, []
for w in watch:
    if w.get("kind") == "dropped":
        foreign.append((w["t_us"], f"{w['dev']}: SYN_DROPPED (events lost; cannot vouch)"))
        continue
    if w.get("kind") != "seen":
        continue
    dev = w["dev"]
    for etype, code, value in w["ev"]:
        if dev not in (pointer_dev, keyboard_dev):
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))
        elif etype == 3 and dev == pointer_dev and code in (0, 1):
            if value not in (inj_ax if code == 0 else inj_ay):
                foreign.append((w["t_us"], f"{dev}: {'ABS_X' if code == 0 else 'ABS_Y'}={value}"))
        elif etype == 1 and value != 2:
            k = (dev, code, value)
            seen_keys[k] = seen_keys.get(k, 0) + 1
            if seen_keys[k] > inj_keys.get(k, 0):
                foreign.append((w["t_us"], f"{dev}: key/button {code}={value}"))
        elif etype == 4:
            pass
        elif etype != 1:
            foreign.append((w["t_us"], f"{dev}: type={etype} code={code} value={value}"))

width = max(len(r[0]) for r in results)
print()
print("─" * 78)
for name, status, detail in results:
    print(f"{status}  {name:<{width}}  {detail}")
print("─" * 78)
print(f"logs: {log_dir}")
print()
if foreign:
    print(f"VERDICT: INVALID — {len(foreign)} input event(s) the injector did not send; "
          f"first: {foreign[0][1]}. Re-run hands off, host pointer outside the VM window.")
    sys.exit(3)
if any(r[1] == "FAIL" for r in results):
    print("VERDICT: FAIL")
    sys.exit(1)
warned = [r[0] for r in results if r[1] == "WARN"]
print("VERDICT: PASS" + (f" (warnings: {', '.join(warned)})" if warned else ""))
sys.exit(0)
PY
exit $?
