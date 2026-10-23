#!/usr/bin/env bash
# The Lumen login session on the real console (design-session-and-wm T.3, Part 1): the
# build installed into a scratch prefix, its units run from the runtime unit directory by
# lumen-session (run-desktop.sh --systemd), and every promise of the session checked:
#
#   ready      lumen-compositor.service active (Type=notify-reload: READY came) with
#              StatusText "Serving …"; graphical-session.target active; the user manager's
#              environment has WAYLAND_DISPLAY, XDG_CURRENT_DESKTOP=Lumen, LUMEN_SOCKET,
#              XDG_SESSION_TYPE=wayland; logind's session Type is wayland
#   shell      lumen-shell.service active and its panel configured
#   autostart  an XDG autostart unit (app-*@autostart.service) started after READY
#   ctl        lumen-ctl status (ok or degraded), health, outputs, windows (the client)
#   restart    the shell killed with SIGKILL: systemd restarts it (NRestarts=1), the
#              compositor replays the same snapshot to it, holds the panel's strip meanwhile
#              (a maximized window keeps its geometry) and the panel is configured again
#   reload     systemctl --user reload lumen-compositor: RELOADING/READY answered
#   logout     lumen-ctl logout ends the session: lumen-session exits 0, logind's session
#              Type is tty again, the console is shown, and run-desktop.sh's own checks pass
#              (the manager's environment and unit files exactly as before, no unit left)
#   watchdog   (--watchdog) a second session with WatchdogSec=4: the compositor stopped
#              with SIGSTOP is killed by systemd's watchdog and the session ends cleanly
#
#   ./scripts/session-smoke.sh                   # from the console or over SSH
#   ./scripts/session-smoke.sh --watchdog
#
# Options:
#   --prefix DIR   where the build is installed for the run (default <build>/session-smoke-prefix)
#   --socket NAME  Wayland socket name (default lumen-session-smoke)
#   --logs DIR     log directory (default <repo>/session-smoke-logs)
#   --watchdog     also run the watchdog group
#   --no-install   use the prefix as it is
# XDG_CONFIG_HOME is a directory under the logs (the user's ~/.config is never written).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
PREFIX=""
SOCKET="lumen-session-smoke"
LOG_DIR="${ROOT}/session-smoke-logs"
WATCHDOG=0
INSTALL=1
while (( $# > 0 )); do
  case "$1" in
    --prefix) PREFIX="${2:?}"; shift ;;
    --socket) SOCKET="${2:?}"; shift ;;
    --logs) LOG_DIR="${2:?}"; shift ;;
    --watchdog) WATCHDOG=1 ;;
    --no-install) INSTALL=0 ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1"; exit 2 ;;
  esac
  shift
done
PREFIX="${PREFIX:-$BUILD/session-smoke-prefix}"
mkdir -p "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"
export XDG_CONFIG_HOME="$LOG_DIR/config"
mkdir -p "$XDG_CONFIG_HOME"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
CONTROL="$XDG_RUNTIME_DIR/$SOCKET.varlink"
export LUMEN_SOCKET="$CONTROL"

failures=()
warnings=()
ok() { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; failures+=("$1"); }
note() { echo "  warn  $*"; warnings+=("$1"); }

if pgrep -a 'lumen-composito|sway|Hyprland|weston|kwin|mutter|niri' >/dev/null; then
  echo "another compositor is running; not taking the display:"
  pgrep -a 'lumen-composito|sway|Hyprland|weston|kwin|mutter|niri'
  exit 2
fi

if (( INSTALL )); then
  echo "Installing $BUILD into $PREFIX"
  if ! cmake --install "$BUILD" --prefix "$PREFIX" >"$LOG_DIR/install.log" 2>&1; then
    echo "install failed (see $LOG_DIR/install.log)"; exit 2
  fi
  outside=$(grep -v "^$PREFIX/" "$BUILD/install_manifest.txt" || true)
  [[ -z "$outside" ]] && ok "install: every file under the prefix" ||
    bad "install: files outside the prefix: $outside"
fi
CTL="$PREFIX/bin/lumen-ctl"

prop() { systemctl --user show -p "$2" --value "$1" 2>/dev/null; }
mono_us() { awk '{printf "%d", $1 * 1000000}' /proc/uptime; }
console_shown() {
  sudo -n modetest -M vmwgfx -p 2>/dev/null | awk '/^Planes:/{p=1;next} p && $1 ~ /^[0-9]+$/ && NF>=3 {print $3; exit}'
}
restore_console() {
  local fb
  fb=$(console_shown)
  if [[ -z "$fb" || "$fb" == 0 ]]; then
    echo "  (console blank: restoring fbcon's mode)"
    head -1 /sys/class/graphics/fb0/modes | sudo -n tee /sys/class/graphics/fb0/mode >/dev/null
  fi
}

# ── One session ──────────────────────────────────────────────────────────────
run_session() { # LOGS EXTRA_ARGS...
  local logs=$1; shift
  "$ROOT/scripts/run-desktop.sh" --systemd --prefix "$PREFIX" -t 120 --socket "$SOCKET" \
    --logs "$logs" "$@" -- foot >"$logs.out" 2>&1 &
  RD_PID=$!
}
wait_active() { # seconds
  local i
  for (( i = 0; i < $1 * 20; i++ )); do
    [[ "$(prop lumen-compositor.service ActiveState)" == active ]] && return 0
    kill -0 "$RD_PID" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}
wait_rd() { # seconds
  local i
  for (( i = 0; i < $1 * 10; i++ )); do
    kill -0 "$RD_PID" 2>/dev/null || { wait "$RD_PID"; RD_RC=$?; return 0; }
    sleep 0.1
  done
  return 1
}

echo "Session run 1: ready, shell, autostart, lumen-ctl, shell restart, reload, logout"
S1="$LOG_DIR/run1"
rm -rf "$S1"; mkdir -p "$S1"
sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null)
type_before=$(loginctl show-session "$sid" -p Type --value 2>/dev/null)
t_start=$(mono_us)
run_session "$S1"
if ! wait_active 15; then
  bad "ready: lumen-compositor.service never became active"
  cat "$S1.out"; tail -20 "$S1/compositor.log" 2>/dev/null
  wait_rd 30
else
  ready_us=$(prop lumen-compositor.service ActiveEnterTimestampMonotonic)
  status=$(prop lumen-compositor.service StatusText)
  [[ "$status" == Serving* ]] && ok "ready: active, StatusText \"$status\" ($(( (ready_us - t_start) / 1000 )) ms after the start)" ||
    bad "ready: StatusText \"$status\""
  [[ "$(prop graphical-session.target ActiveState)" == active ]] && ok "graphical-session.target active" ||
    bad "graphical-session.target not active"
  env_now=$(systemctl --user show-environment)
  missing=""
  for kv in "WAYLAND_DISPLAY=$SOCKET" "XDG_CURRENT_DESKTOP=Lumen" "XDG_SESSION_TYPE=wayland" \
            "LUMEN_SOCKET=$CONTROL" "XDG_SESSION_DESKTOP=lumen" "XDG_CONFIG_HOME=$XDG_CONFIG_HOME"; do
    grep -qxF "$kv" <<<"$env_now" || missing+=" $kv"
  done
  [[ -z "$missing" ]] && ok "environment exported to the user manager" || bad "environment: missing$missing"
  t=$(loginctl show-session "$sid" -p Type --value)
  [[ "$t" == wayland ]] && ok "logind session $sid Type=wayland (was $type_before)" ||
    bad "logind session Type=$t during the session"
  grep -q 'ready: Serving' "$S1/compositor.log" && grep -q 'session environment exported' "$S1/compositor.log" &&
    ok "compositor log: environment exported, then ready" || bad "compositor log lacks the export/ready lines"

  for _ in $(seq 1 60); do grep -q "shell.panel: configure" "$S1/shell.log" 2>/dev/null && break; sleep 0.1; done
  shell_pid=$(prop lumen-shell.service MainPID)
  [[ "$(prop lumen-shell.service ActiveState)" == active && "$shell_pid" != 0 ]] &&
    grep -q "shell.panel: configure" "$S1/shell.log" &&
    ok "shell: lumen-shell.service active (pid $shell_pid), panel configured" ||
    bad "shell: not active or no panel"

  # XDG autostart: generated units this desktop runs, started after READY.
  sleep 1
  started=""
  for u in $(systemctl --user list-units --all --no-legend --plain 'app-*@autostart.service' | awk '{print $1}'); do
    t_u=$(prop "$u" ExecMainStartTimestampMonotonic)
    [[ -n "$t_u" && "$t_u" != 0 && "$t_u" -ge "$ready_us" ]] && started+=" $u"
  done
  [[ -n "$started" ]] && ok "autostart: started after READY:$started" || bad "autostart: no app-*@autostart.service started"

  # lumen-ctl
  "$CTL" status >"$S1/ctl-status.txt" 2>&1; st=$?
  (( st <= 1 )) && ok "lumen-ctl status: exit $st ($(head -1 "$S1/ctl-status.txt"))" ||
    bad "lumen-ctl status exit $st: $(head -3 "$S1/ctl-status.txt")"
  "$CTL" --json health >"$S1/ctl-health.json" 2>&1 # exit 0/1/2 by level
  grep -q '"systemd"' "$S1/ctl-health.json" &&
    grep -q 'ready, environment exported, watchdog on' "$S1/ctl-health.json" &&
    ok "lumen-ctl health: systemd part ready, environment exported, watchdog on" ||
    bad "lumen-ctl health: $(head -c 300 "$S1/ctl-health.json")"
  "$CTL" outputs >"$S1/ctl-outputs.txt" 2>&1 && grep -q 'Virtual-1' "$S1/ctl-outputs.txt" &&
    ok "lumen-ctl outputs: $(head -1 "$S1/ctl-outputs.txt")" || bad "lumen-ctl outputs: $(cat "$S1/ctl-outputs.txt")"
  for _ in $(seq 1 50); do "$CTL" windows 2>/dev/null | grep -q foot && break; sleep 0.1; done
  "$CTL" windows >"$S1/ctl-windows.txt" 2>&1
  grep -q foot "$S1/ctl-windows.txt" && ok "lumen-ctl windows: $(grep foot "$S1/ctl-windows.txt" | head -1 | tr -s ' ')" ||
    bad "lumen-ctl windows: no foot"

  # Shell restart with its state: a maximized window keeps its place (zone hold).
  "$CTL" action window.maximize-toggle >/dev/null 2>&1
  sleep 0.5
  geo_before=$("$CTL" windows | awk '/foot/ {print $4, $5}')
  snap_before=$("$CTL" windows | grep -c .)
  shell_pid=$(prop lumen-shell.service MainPID)
  kill -KILL "$shell_pid"
  new_pid=0
  for _ in $(seq 1 40); do
    new_pid=$(prop lumen-shell.service MainPID)
    [[ "$new_pid" != 0 && "$new_pid" != "$shell_pid" && "$(prop lumen-shell.service ActiveState)" == active ]] && break
    sleep 0.05
  done
  restarts=$(prop lumen-shell.service NRestarts)
  # (the old shell's windows: the snapshot the new one must get)
  [[ "$new_pid" != 0 && "$new_pid" != "$shell_pid" && "$restarts" == 1 ]] &&
    ok "restart: SIGKILLed shell $shell_pid back as $new_pid (NRestarts=$restarts)" ||
    bad "restart: shell not back (pid $new_pid, NRestarts=$restarts)"
  for _ in $(seq 1 60); do grep -q 'binds=2 snapshot=' "$S1/compositor.log" && break; sleep 0.1; done
  snap_after=$(grep 'bridge bound client=.* binds=2$' "$S1/compositor.log" | tail -1 | sed 's/.*snapshot=\([0-9]*\).*/\1/')
  [[ -n "$snap_after" && "$snap_after" == "$snap_before" ]] &&
    ok "restart: the new shell got the same snapshot ($snap_after window(s))" ||
    bad "restart: snapshot before=$snap_before after=$snap_after"
  grep -q 'zone hold on output' "$S1/compositor.log" && ok "restart: the panel's zone was held" ||
    bad "restart: no zone hold logged"
  for _ in $(seq 1 60); do (( $(grep -c "shell.panel: configure" "$S1/shell.log") >= 2 )) && break; sleep 0.1; done
  (( $(grep -c "shell.panel: configure" "$S1/shell.log") >= 2 )) && ok "restart: panel configured again" ||
    bad "restart: no second panel"
  sleep 0.5
  geo_after=$("$CTL" windows | awk '/foot/ {print $4, $5}')
  [[ -n "$geo_before" && "$geo_before" == "$geo_after" ]] &&
    ok "restart: the maximized window kept its geometry ($geo_after)" ||
    bad "restart: window geometry $geo_before -> $geo_after"
  grep -q 'zone hold released' "$S1/compositor.log" && ok "restart: zone hold released" || note "zone hold release not logged yet"

  # Reload
  # systemctl waits for READY=1, which comes once the configuration's new generation is
  # applied (Part 2): a second generation is logged before it returns.
  if timeout 10 systemctl --user reload lumen-compositor.service; then
    grep -q 'reload (SIGHUP)' "$S1/compositor.log" &&
      grep -qE 'config: config: generation [2-9][0-9]* from' "$S1/compositor.log" &&
      ok "reload: systemctl reload answered (RELOADING/READY), configuration generation 2" ||
      bad "reload: no reload line or no new configuration generation"
  else
    bad "reload: systemctl --user reload failed"
  fi

  # Logout
  "$CTL" logout && ok "lumen-ctl logout accepted" || bad "lumen-ctl logout failed"
  if wait_rd 40; then
    cat "$S1.out" | sed -n '/^Session ended/,$p' | sed 's/^/    /'
    grep -q 'Session ended: session ended (lumen-session exit 0)' "$S1.out" &&
      ok "logout: lumen-session exited 0" || bad "logout: $(grep 'Session ended' "$S1.out")"
    grep -q 'FAIL' "$S1.out" && bad "run-desktop.sh checks failed" ||
      ok "run-desktop.sh: environment, unit files as before; no unit left"
  else
    bad "logout: the session did not end within 40 s"
  fi
  t=$(loginctl show-session "$sid" -p Type --value)
  [[ "$t" == "$type_before" ]] && ok "logind session Type back to $t" || bad "logind session Type=$t after"
fi
sleep 0.5
fb=$(console_shown)
[[ -n "$fb" && "$fb" != 0 ]] && ok "console shown (fb $fb)" || { bad "console blank after the session"; restore_console; }

if (( WATCHDOG )); then
  echo "Session run 2: the watchdog"
  S2="$LOG_DIR/run2"
  rm -rf "$S2"; mkdir -p "$S2"
  run_session "$S2" --watchdog-sec 4
  if ! wait_active 15; then
    bad "watchdog: the session did not start"
    wait_rd 30
  else
    [[ "$(prop lumen-compositor.service WatchdogUSec)" == 4s ]] || note "WatchdogUSec=$(prop lumen-compositor.service WatchdogUSec)"
    sleep 6 # pings keep it alive past the period
    [[ "$(prop lumen-compositor.service ActiveState)" == active ]] && ok "watchdog: pinged (alive after 6 s with WatchdogSec=4)" ||
      bad "watchdog: died while healthy"
    comp=$(prop lumen-compositor.service MainPID)
    kill -STOP "$comp"
    if wait_rd 40; then
      res=$(grep -o 'Session ended:.*' "$S2.out")
      grep -q 'Lumen exited: watchdog' "$S2/session.log" &&
        ok "watchdog: a stopped compositor was aborted by systemd; the session ended ($res)" ||
        bad "watchdog: $(cat "$S2/session.log") $res"
      grep -q 'FAIL' "$S2.out" && bad "watchdog: run-desktop.sh checks failed" ||
        ok "watchdog: environment and units as before"
    else
      kill -CONT "$comp" 2>/dev/null
      bad "watchdog: the session did not end"
      wait_rd 30
    fi
  fi
  sleep 1
  fb=$(console_shown)
  [[ -n "$fb" && "$fb" != 0 ]] && ok "console shown after the watchdog (fb $fb)" ||
    { bad "console blank after the watchdog"; restore_console; }
fi

echo
if (( ${#failures[@]} )); then
  echo "VERDICT: FAIL (${#failures[@]}: ${failures[*]})"
  exit 1
fi
echo "VERDICT: PASS${warnings[*]:+ (warnings: ${warnings[*]})}"
