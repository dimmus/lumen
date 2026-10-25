#!/usr/bin/env bash
# Run a Lumen session on the console: compositor, lumen-shell's panel, and a first client.
#
#   ./scripts/run-desktop.sh                      # foot, from the TTY you are logged into
#   ./scripts/run-desktop.sh -- gtk4-widget-factory
#   ./scripts/run-desktop.sh -t 60 -- alacritty
#   ./scripts/run-desktop.sh --join-seat-session  # from SSH: join the active seat session
#
# The session ends when you log out — the panel's "Log out" button (click, then click
# "Confirm?"), or Ctrl+Alt+Backspace, which works even without the panel — or after the
# time limit, whichever comes first. Ctrl+Alt+F<n> switches to another VT and back (the
# compositor executes it, so it works without the panel too); the session pauses while away
# and its windows keep running. Closing a window, the first client's included, closes
# only that window: the session, and anything started from it, keeps running. More
# windows: the panel's Apps button or a Super tap opens the launcher, Super+Return a
# terminal; what the launcher starts runs as its own systemd user service, not as a child of
# this script's client or of the shell, and is told WAYLAND_DISPLAY from the shell (apps
# still end with the session, when the compositor goes). Logging out
# asks every window to close and then stops the compositor. The limit is a safety net: the
# compositor owns the console while it runs, so a session whose way out broke would
# otherwise leave no way back to the text console. Teardown stops everything with SIGTERM,
# which is what restores the console's text mode.
#
# Output goes to log files, because the console is not readable while the compositor
# holds it; a summary is printed once the console is back.
#
# Portals: the session exports WAYLAND_DISPLAY, XDG_CURRENT_DESKTOP=Lumen and
# XDG_SESSION_TYPE=wayland to the systemd user manager, starts xdg-desktop-portal-wlr
# (ScreenCast) and the build's xdg-desktop-portal-lumen (Screenshot, PickColor, the cast
# source chooser), starts WirePlumber if no PipeWire session manager runs, and restarts
# xdg-desktop-portal so browsers, OBS and screenshot tools find the session; all of it is
# undone when the session ends (scripts/portal-session.sh). Their logs: xdpw.log,
# portal-lumen.log.
#
# Options:
#   -t SECONDS            time limit (default 600; 0 = none — only when you are sure you
#                         can log out: the panel's button or Ctrl+Alt+Backspace)
#   --socket NAME         Wayland socket name (default lumen-0)
#   --decorations=MODE    server (Lumen draws window frames) or client; passed to the
#                         compositor
#   --scale S             output scale, fractional allowed (1.25, 1.5, 2); passed to the
#                         compositor
#   --no-shell            do not start lumen-shell
#   --no-portals          leave xdg-desktop-portal and the user manager's environment alone
#   --blank-after S       lumen-shell powers the outputs off after S seconds without input
#                         (default 600; 0 = never); any input wakes them
#   --idle-policy=none    lumen-shell leaves blanking to an external daemon (swayidle)
#   --lock-command CMD    lumen-shell's screen locker (an Exec line, e.g. 'swaylock -f'):
#                         Super+L, loginctl lock-session, before sleep and the blank timeout
#                         start it (lock first, then blank); none by default
#   --blank-locked-after S  outputs off after S seconds without input while locked (60)
#   --background SPEC     lumen-shell's desktop background: lumen (default), #RRGGBB,
#                         gradient:#RRGGBB:#RRGGBB[:DEG], image:PATH[:fill|fit|center|
#                         stretch|tile] (PNG, JPEG, WebP, SVG); --no-background for none
#   --no-lock-on-blank    the blank timeout blanks without locking first
#   --debug               LUMEN_LOG=debug for the compositor and the shell (default info)
#   --logs DIR            log directory (default <repo>/run-desktop-logs)
#   --join-seat-session   move the compositor into the active seat0 logind session's
#                         cgroup, so logind lets libinput open input devices; needed when
#                         started from SSH (uses sudo)
#   --no-restart-shell    leave lumen-shell dead when it crashes (default: restarted, at most
#                         5 times in 30 s, as lumen-shell.service does)
#   --systemd             the real session path instead (design-session-and-wm S.10): the
#                         session's units are written to $XDG_RUNTIME_DIR/systemd/user for
#                         this run only and lumen-session starts them, so the compositor runs
#                         in the user manager (Type=notify-reload, graphical-session.target,
#                         the shell restarted by systemd, XDG autostart). From SSH it joins
#                         the active seat0 session by XDG_SESSION_ID. Afterwards the units
#                         are removed and the manager's environment and unit files are
#                         checked against what they were (FAIL when they differ). The
#                         compositor and the shell log to LOG_DIR through a runtime drop-in.
#   --prefix DIR          with --systemd: an installed tree (cmake --install --prefix DIR)
#                         instead of the build tree
#   --watchdog-sec S      with --systemd: WatchdogSec= of this run's compositor unit
#   -- CMD [ARGS...]      the client to run (default: foot)
#
# Needs a build (LUMEN_BUILD_DIR, default build/release) with lumen-compositor and
# lumen-shell. lumen-shell must sit next to lumen-compositor: the compositor accepts
# zlm_shell_v1 only from that binary.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${LUMEN_BUILD_DIR:-${ROOT}/build/release}"
COMP="${BUILD}/lumen-compositor"
SHELL_BIN="${BUILD}/lumen-shell"

SOCKET="lumen-0"
LIMIT=600
LOG_DIR="${ROOT}/run-desktop-logs"
LOG_LEVEL=info
WITH_SHELL=1
WITH_PORTALS=1
JOIN_SEAT=0
RESTART_SHELL=1
SYSTEMD_MODE=0
PREFIX=""
WATCHDOG_SEC=""
COMP_ARGS=()
SHELL_ARGS=()
CLIENT=()
while (( $# > 0 )); do
  case "$1" in
    -t) LIMIT="${2:?-t needs seconds}"; shift ;;
    --socket) SOCKET="${2:?--socket needs a name}"; shift ;;
    --decorations=server|--decorations=client) COMP_ARGS+=("$1") ;;
    --scale) COMP_ARGS+=(--scale "${2:?--scale needs a value}"); shift ;;
    --no-shell) WITH_SHELL=0 ;;
    --no-portals) WITH_PORTALS=0 ;;
    --blank-after) SHELL_ARGS+=(--blank-after "${2:?--blank-after needs seconds}"); shift ;;
    --idle-policy=shell|--idle-policy=none) SHELL_ARGS+=("$1") ;;
    --lock-command) SHELL_ARGS+=(--lock-command "${2:?--lock-command needs a command}"); shift ;;
    --blank-locked-after) SHELL_ARGS+=(--blank-locked-after "${2:?--blank-locked-after needs seconds}"); shift ;;
    --background) SHELL_ARGS+=(--background "${2:?--background needs a spec}"); shift ;;
    --no-background) SHELL_ARGS+=(--no-background) ;;
    --no-lock-on-blank) SHELL_ARGS+=(--no-lock-on-blank) ;;
    --debug) LOG_LEVEL=debug ;;
    --logs) LOG_DIR="${2:?--logs needs a directory}"; shift ;;
    --join-seat-session) JOIN_SEAT=1 ;;
    --no-restart-shell) RESTART_SHELL=0 ;;
    --systemd) SYSTEMD_MODE=1 ;;
    --prefix) PREFIX="${2:?--prefix needs a directory}"; shift ;;
    --watchdog-sec) WATCHDOG_SEC="${2:?--watchdog-sec needs seconds}"; shift ;;
    --) shift; CLIENT=("$@"); break ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done
(( ${#CLIENT[@]} > 0 )) || CLIENT=(foot)
[[ "$LIMIT" =~ ^[0-9]+$ ]] || { echo "-t needs a whole number of seconds"; exit 2; }

# ── --systemd: the session the way a login starts it ─────────────────────────
run_systemd_session() {
  local s_bin s_units s_dbus s_portals f u
  if [[ -n "$PREFIX" ]]; then
    PREFIX=$(cd "$PREFIX" 2>/dev/null && pwd) || { echo "--prefix: no such directory"; return 2; }
    s_bin="$PREFIX/bin"; s_units="$PREFIX/lib/systemd/user"
    s_dbus="$PREFIX/share/dbus-1/services"; s_portals="$PREFIX/share/xdg-desktop-portal"
  else
    s_bin="$BUILD"; s_units="$BUILD/share/systemd/user"
    s_dbus="$BUILD/share/dbus-1/services"; s_portals="$BUILD/share/xdg-desktop-portal"
  fi
  for f in "$s_bin/lumen-session" "$s_bin/lumen-ctl" "$s_bin/lumen-compositor" \
           "$s_bin/lumen-shell" "$s_units/lumen-compositor.service" \
           "$s_units/lumen-shell.service" "$s_units/lumen-shutdown.target"; do
    [[ -e "$f" ]] || { echo "$f is missing (build, or cmake --install --prefix)"; return 2; }
  done
  command -v "${CLIENT[0]}" >/dev/null || { echo "client '${CLIENT[0]}' not found"; return 2; }
  systemctl --user show-environment >/dev/null 2>&1 || { echo "no systemd user manager"; return 2; }
  if systemctl --user -q is-active graphical-session.target; then
    echo "a graphical session is already running (graphical-session.target is active)"
    return 2
  fi
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  local socket_path="$XDG_RUNTIME_DIR/$SOCKET" control="$XDG_RUNTIME_DIR/$SOCKET.varlink"
  if [[ -e "$socket_path.lock" ]] && ! flock -n "$socket_path.lock" true; then
    echo "socket '$SOCKET' is held by a running compositor — pick another with --socket"
    return 2
  fi
  local unit_dir="$XDG_RUNTIME_DIR/systemd/user" units=(lumen-compositor.service
    lumen-shell.service lumen-shutdown.target xdg-desktop-portal-lumen.service)
  for u in "${units[@]}"; do
    [[ -e "$unit_dir/$u" ]] && { echo "$unit_dir/$u exists already (another run?)"; return 2; }
  done
  # The seat session to take over: this one on a console, the active seat0 one over SSH.
  local sid="${XDG_SESSION_ID:-}"
  if [[ -z "$sid" || -z "$(loginctl show-session "$sid" -p Seat --value 2>/dev/null)" ]]; then
    sid=$(loginctl show-seat seat0 -p ActiveSession --value 2>/dev/null || true)
  fi
  if [[ -z "$sid" || "$(loginctl show-session "$sid" -p User --value 2>/dev/null)" != "$(id -u)" ]]; then
    echo "no active seat0 session of $(id -un) to run the session in"
    return 2
  fi

  mkdir -p "$LOG_DIR"
  local comp_log="$LOG_DIR/compositor.log" shell_log="$LOG_DIR/shell.log"
  local client_log="$LOG_DIR/client.log" session_log="$LOG_DIR/session.log"
  : >"$comp_log"; : >"$shell_log"; : >"$client_log"; : >"$session_log"

  # What must be exactly as it was afterwards.
  systemctl --user show-environment >"$LOG_DIR/manager-env.before"
  systemctl --user list-unit-files --state=enabled,linked --no-legend 2>/dev/null |
    sort >"$LOG_DIR/unit-files.before"
  local portals_before=() failed_before
  for u in xdg-desktop-portal.service xdg-desktop-portal-gtk.service xdg-desktop-portal-wlr.service; do
    systemctl --user -q is-active "$u" && portals_before+=("$u")
  done
  failed_before=$(systemctl --user list-units --state=failed --no-legend --plain 2>/dev/null |
                  awk '{print $1}')

  # This run's units: copies in the runtime unit directory (gone at reboot), plus drop-ins
  # sending the logs to LOG_DIR and passing this run's options.
  local made_unit_dir=0 made_dbus_dir=0 dbus_dir="$XDG_RUNTIME_DIR/dbus-1/services"
  local portal_dir="$XDG_RUNTIME_DIR/lumen-portals-$SOCKET"
  [[ -d "$unit_dir" ]] || { mkdir -p "$unit_dir"; made_unit_dir=1; }
  for u in "${units[@]}"; do
    [[ -e "$s_units/$u" ]] && cp "$s_units/$u" "$unit_dir/$u"
  done
  if [[ -n "$WATCHDOG_SEC" ]]; then
    sed -i "s/^WatchdogSec=.*/WatchdogSec=${WATCHDOG_SEC}s/" "$unit_dir/lumen-compositor.service"
  fi
  mkdir -p "$unit_dir/lumen-compositor.service.d" "$unit_dir/lumen-shell.service.d"
  local q_comp_args="" q_shell_args="" a
  for a in "${COMP_ARGS[@]}"; do q_comp_args+=" \"${a//\"/\\\"}\""; done
  for a in "${SHELL_ARGS[@]}"; do q_shell_args+=" \"${a//\"/\\\"}\""; done
  cat >"$unit_dir/lumen-compositor.service.d/run-desktop.conf" <<EOF
# run-desktop.sh --systemd: this run's logs and options (removed at its end).
[Service]
Environment=LUMEN_LOG=$LOG_LEVEL
StandardOutput=append:$comp_log
StandardError=append:$comp_log
EOF
  # Options change the command line; without them the unit's own ExecStart= runs as installed.
  [[ -n "$q_comp_args" ]] && printf 'ExecStart=\nExecStart=%s\n' \
    "$s_bin/lumen-compositor --session \$LUMEN_DISPLAY$q_comp_args" \
    >>"$unit_dir/lumen-compositor.service.d/run-desktop.conf"
  cat >"$unit_dir/lumen-shell.service.d/run-desktop.conf" <<EOF
# run-desktop.sh --systemd: this run's logs and options (removed at its end).
[Service]
Environment=LUMEN_LOG=$LOG_LEVEL
StandardOutput=append:$shell_log
StandardError=append:$shell_log
EOF
  [[ -n "$q_shell_args" ]] && printf 'ExecStart=\nExecStart=%s\n' "$s_bin/lumen-shell$q_shell_args" \
    >>"$unit_dir/lumen-shell.service.d/run-desktop.conf"
  # The portal backend's D-Bus service (a standard session service directory) and the
  # frontend's portal list with lumen.portal and lumen-portals.conf beside the system's.
  if [[ -e "$s_dbus/org.freedesktop.impl.portal.desktop.lumen.service" ]]; then
    [[ -d "$dbus_dir" ]] || { mkdir -p "$dbus_dir"; made_dbus_dir=1; }
    ln -sf "$s_dbus/org.freedesktop.impl.portal.desktop.lumen.service" "$dbus_dir/"
    busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
      ReloadConfig >/dev/null 2>&1
  fi
  local set_portal_dir=0
  if [[ -f "$s_portals/lumen-portals.conf" ]]; then
    rm -rf "$portal_dir"
    mkdir -m 0700 "$portal_dir"
    for f in /usr/share/xdg-desktop-portal/portals/*.portal; do
      [[ -e "$f" && "${f##*/}" != lumen.portal ]] && ln -s "$f" "$portal_dir/"
    done
    ln -s "$s_portals/portals/lumen.portal" "$portal_dir/lumen.portal"
    ln -s "$s_portals/lumen-portals.conf" "$portal_dir/lumen-portals.conf"
    systemctl --user set-environment "XDG_DESKTOP_PORTAL_DIR=$portal_dir"
    set_portal_dir=1
  fi
  systemctl --user daemon-reload

  local session_pid="" client_pid="" ended="" rc=0
  sd_cleanup() {
    [[ -n "$client_pid" ]] && { kill -TERM "$client_pid" 2>/dev/null; wait "$client_pid" 2>/dev/null; }
    client_pid=""
    if [[ -n "$session_pid" ]] && kill -0 "$session_pid" 2>/dev/null; then
      systemctl --user stop lumen-compositor.service
      for _ in $(seq 1 150); do kill -0 "$session_pid" 2>/dev/null || break; sleep 0.1; done
      if kill -0 "$session_pid" 2>/dev/null; then
        echo "WARN: lumen-session did not end; killing it and cleaning up after it"
        kill -9 "$session_pid" 2>/dev/null
        "$s_bin/lumen-session" --cleanup
      fi
    fi
    [[ -n "$session_pid" ]] && wait "$session_pid" 2>/dev/null
    session_pid=""
    # Units gone: failed states cleared first (a failed unit stays listed), then the files.
    # Units that failed because of the session (an autostart program that needs X11, a
    # D-Bus-activated helper such as the accessibility bus losing its display at the end)
    # are cleared too, and named: the manager ends as it began.
    systemctl --user reset-failed "${units[@]}" 2>/dev/null
    local fu
    sleep 0.5
    for fu in $(systemctl --user list-units --state=failed --no-legend --plain 2>/dev/null |
                awk '{print $1}'); do
      grep -qxF "$fu" <<<"$failed_before" && continue
      echo "  (reset-failed $fu: it failed during the session)"
      systemctl --user reset-failed "$fu"
    done
    for u in "${units[@]}"; do rm -f "$unit_dir/$u"; done
    rm -rf "$unit_dir/lumen-compositor.service.d" "$unit_dir/lumen-shell.service.d"
    (( made_unit_dir )) && rmdir "$unit_dir" 2>/dev/null
    rm -f "$dbus_dir/org.freedesktop.impl.portal.desktop.lumen.service"
    (( made_dbus_dir )) && rmdir "$dbus_dir" "${dbus_dir%/services}" 2>/dev/null
    busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
      ReloadConfig >/dev/null 2>&1
    if (( set_portal_dir )); then
      local prev
      prev=$(grep '^XDG_DESKTOP_PORTAL_DIR=' "$LOG_DIR/manager-env.before" || true)
      if [[ -n "$prev" ]]; then systemctl --user set-environment "$prev"
      else systemctl --user unset-environment XDG_DESKTOP_PORTAL_DIR; fi
      rm -rf "$portal_dir"
    fi
    systemctl --user daemon-reload
    for u in "${portals_before[@]}"; do systemctl --user start "$u" 2>/dev/null; done
    if (( ${#portals_before[@]} )); then
      # The restarted frontend may activate a backend against the environment from before
      # the session (a display that is gone): it fails, and is cleared so the manager ends
      # as it began (portal-session.sh does the same).
      sleep 1
      for fu in $(systemctl --user list-units --state=failed --no-legend --plain 2>/dev/null |
                  awk '{print $1}'); do
        grep -qxF "$fu" <<<"$failed_before" ||
          { echo "  (reset-failed $fu: it failed when the portals restarted)"; systemctl --user reset-failed "$fu"; }
      done
    fi
    rm -f "$socket_path" "$socket_path.lock" "$control"
  }
  trap 'ended=${ended:-signal}; sd_cleanup; exit 130' INT TERM

  echo "Starting a Lumen systemd session on $SOCKET (seat session $sid) with ${CLIENT[*]}$( (( LIMIT )) && echo " (time limit ${LIMIT}s)")."
  echo "Units from $s_units; logs go to $LOG_DIR."
  env LUMEN_DISPLAY="$SOCKET" XDG_SESSION_ID="$sid" "$s_bin/lumen-session" >"$session_log" 2>&1 &
  session_pid=$!

  # Ready: the unit is active (READY=1 came, after the environment export).
  local ready=0
  for _ in $(seq 1 300); do
    if [[ "$(systemctl --user show -p ActiveState --value lumen-compositor.service)" == active ]]; then
      ready=1; break
    fi
    kill -0 "$session_pid" 2>/dev/null || break
    sleep 0.05
  done
  if (( ! ready )); then
    ended="compositor did not start"
  else
    # The panel first, as in the direct mode: a window placed before it sits under it.
    for _ in $(seq 1 60); do
      grep -q "shell.panel: configure" "$shell_log" 2>/dev/null && break
      sleep 0.05
    done
    WAYLAND_DISPLAY="$SOCKET" LUMEN_SOCKET="$control" XDG_CURRENT_DESKTOP=Lumen \
      XDG_SESSION_TYPE=wayland GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland \
      SDL_VIDEODRIVER=wayland "${CLIENT[@]}" >"$client_log" 2>&1 &
    client_pid=$!
    local deadline=$(( $(date +%s) + LIMIT ))
    while kill -0 "$session_pid" 2>/dev/null; do
      if (( LIMIT > 0 && $(date +%s) >= deadline )); then
        ended="time limit reached"
        "$s_bin/lumen-ctl" --socket "$control" logout >/dev/null 2>&1
        for _ in $(seq 1 150); do kill -0 "$session_pid" 2>/dev/null || break; sleep 0.1; done
        break
      fi
      sleep 0.2
    done
    if ! kill -0 "$session_pid" 2>/dev/null; then
      wait "$session_pid"; rc=$?
      session_pid=""
      ended="${ended:-session ended (lumen-session exit $rc)}"
    fi
  fi
  sd_cleanup
  trap - INT TERM

  # ── Summary: the manager must be as it was ──
  echo "Session ended: $ended."
  systemctl --user show-environment >"$LOG_DIR/manager-env.after"
  systemctl --user list-unit-files --state=enabled,linked --no-legend 2>/dev/null |
    sort >"$LOG_DIR/unit-files.after"
  local verdict=0 left
  if diff -u "$LOG_DIR/manager-env.before" "$LOG_DIR/manager-env.after" >"$LOG_DIR/manager-env.diff"; then
    echo "  PASS manager environment as before"
  else
    echo "  FAIL manager environment differs:"; sed 's/^/    /' "$LOG_DIR/manager-env.diff"; verdict=1
  fi
  if diff -q "$LOG_DIR/unit-files.before" "$LOG_DIR/unit-files.after" >/dev/null; then
    echo "  PASS enabled/linked unit files as before"
  else
    echo "  FAIL unit files differ"; diff "$LOG_DIR/unit-files.before" "$LOG_DIR/unit-files.after"; verdict=1
  fi
  left=$(systemctl --user list-units --all --no-legend --plain 'lumen-*' 'app-*@autostart.service' 2>/dev/null |
         awk '$3 != "inactive" || $4 != "dead" {print $1}' | grep -v '^$' || true)
  local failed_now
  failed_now=$(systemctl --user list-units --state=failed --no-legend --plain 2>/dev/null | awk '{print $1}')
  if [[ "$failed_now" == "$failed_before" ]]; then
    echo "  PASS failed units as before"
  else
    echo "  FAIL failed units differ: before [${failed_before//$'\n'/ }] after [${failed_now//$'\n'/ }]"; verdict=1
  fi
  if [[ -z "$left" ]]; then
    echo "  PASS no session unit left"
  else
    echo "  FAIL units left: $left"; verdict=1
  fi
  echo "  logs: $comp_log"
  echo "        $shell_log"
  echo "        $session_log"
  echo "        $client_log"
  [[ "$ended" == "session ended"* || "$ended" == "time limit reached" ]] || verdict=1
  return $verdict
}
if (( SYSTEMD_MODE )); then
  run_systemd_session
  exit $?
fi

# ── Preconditions ────────────────────────────────────────────────────────────
for bin in "$COMP" $( (( WITH_SHELL )) && echo "$SHELL_BIN"); do
  if [[ ! -x "$bin" ]]; then
    echo "$bin is missing. Build first:  cmake --build --preset release"
    exit 2
  fi
done
if ! command -v "${CLIENT[0]}" >/dev/null; then
  echo "client '${CLIENT[0]}' not found"
  exit 2
fi

# A binary older than its sources runs yesterday's code; say so, but let it run.
if command -v git >/dev/null && git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  newest=$( { git -C "$ROOT" ls-files -z -- modules executables
              git -C "$ROOT" ls-files -z --others --exclude-standard -- modules executables; } |
            (cd "$ROOT" && xargs -0 ls -t 2>/dev/null) | head -1)
  if [[ -n "$newest" && "$ROOT/$newest" -nt "$COMP" ]]; then
    echo "NOTE: $COMP is older than $newest — rebuild to run the current code:"
    echo "      cmake --build --preset release"
  fi
fi

if [[ -n "${WAYLAND_DISPLAY:-}" || -n "${DISPLAY:-}" ]]; then
  echo "NOTE: another graphical session is running; the compositor needs DRM master and"
  echo "      the console, so run this from a text console (or over SSH with"
  echo "      --join-seat-session)."
fi

# The compositor needs atomic modesetting, which Linux before 7.4 leaves off on nouveau: the
# card would be refused and the session would run with nothing on screen.
# shellcheck source=nouveau-atomic.sh
source "$ROOT/scripts/nouveau-atomic.sh"
if [[ -n "$(nouveau_cards)" ]]; then
  case "$(nouveau_atomic_state)" in
    off)
      echo "NOTE: nouveau has atomic modesetting off (Linux before 7.4): the compositor will"
      echo "      refuse $(nouveau_cards | paste -sd' ') and nothing reaches the screen."
      echo "      TODO: ./scripts/install-deps.sh --nouveau-atomic, then reboot." ;;
    pending)
      echo "NOTE: nouveau atomic modesetting is configured but needs a reboot to take effect." ;;
  esac
fi

# libinput opens devices through logind TakeDevice, which only the active seat session
# may do. Find that session's cgroup and compare it with ours.
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
      echo "--join-seat-session: no active seat0 session owned by $(id -un)"
      exit 2
    fi
    if ! sudo -n true 2>/dev/null; then
      echo "--join-seat-session needs passwordless sudo to move into the seat session"
      exit 2
    fi
  else
    echo "NOTE: not running inside the active seat session (${SEAT_SCOPE:-none found});"
    echo "      input devices will likely be refused. From SSH use --join-seat-session."
  fi
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCKET_PATH="$XDG_RUNTIME_DIR/$SOCKET"
LOCK_PATH="${SOCKET_PATH}.lock"
# libwayland flocks <socket>.lock while a compositor serves that name; removing a live
# compositor's socket would strand it, so only clear a lock nobody holds.
if [[ -e "$LOCK_PATH" ]] && ! flock -n "$LOCK_PATH" true; then
  echo "socket '$SOCKET' is held by a running compositor — pick another with --socket"
  exit 2
fi
rm -f "$SOCKET_PATH" "$LOCK_PATH"

mkdir -p "$LOG_DIR"
COMP_LOG="$LOG_DIR/compositor.log"
SHELL_LOG="$LOG_DIR/shell.log"
CLIENT_LOG="$LOG_DIR/client.log"
: >"$COMP_LOG"; : >"$SHELL_LOG"; : >"$CLIENT_LOG"

# ── Processes and cleanup ────────────────────────────────────────────────────
# shellcheck source=portal-session.sh
source "$ROOT/scripts/portal-session.sh"
COMP_PID=""; SHELL_PID=""; CLIENT_PID=""
COMP_RC=""; CLIENT_RC=""; ENDED_BY=""
SHELL_RESTARTS=()

start_shell() { # appends to the shell log: a restart keeps the crash's lines above it
  LUMEN_LOG="$LOG_LEVEL" "$SHELL_BIN" "${SHELL_ARGS[@]}" >>"$SHELL_LOG" 2>&1 &
  SHELL_PID=$!
}

stop() { # pid — SIGTERM, wait up to 5 s, then SIGKILL
  local pid=$1
  [[ -n "$pid" ]] || return 0
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

cleanup() {
  stop "$CLIENT_PID"; CLIENT_PID=""
  # The portal backends are clients of the session: they go before the compositor, and the
  # user manager's environment and xdg-desktop-portal are put back as they were.
  portal_session_stop >>"$LOG_DIR/portals.log" 2>&1
  stop "$SHELL_PID"; SHELL_PID=""
  if [[ -n "$COMP_PID" ]]; then
    # SIGTERM, never SIGKILL first: the compositor's teardown restores text mode.
    kill -TERM "$COMP_PID" 2>/dev/null
    for _ in $(seq 1 50); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$COMP_PID" 2>/dev/null; then
      echo "WARN: compositor ignored SIGTERM for 5 s; killing it (console may stay graphical)"
      kill -9 "$COMP_PID" 2>/dev/null
    fi
    wait "$COMP_PID" 2>/dev/null
    COMP_RC=$?
    COMP_PID=""
  fi
  rm -f "$SOCKET_PATH" "$LOCK_PATH" "${SOCKET_PATH}.varlink"
}
trap cleanup EXIT
trap 'ENDED_BY=${ENDED_BY:-signal}; cleanup; exit 130' INT TERM

# ── Start ────────────────────────────────────────────────────────────────────
echo "Starting Lumen on $SOCKET with ${CLIENT[*]}$( (( LIMIT )) && echo " (time limit ${LIMIT}s)")."
echo "The console switches to the compositor now; logs go to $LOG_DIR."

if (( ! IN_SEAT && JOIN_SEAT )); then
  # The shell moves itself into the seat session's cgroup, then becomes the compositor:
  # same PID, so $! is the compositor and SIGTERM reaches it.
  bash -c 'echo $$ | sudo -n tee "$1/cgroup.procs" >/dev/null && shift && exec env LUMEN_LOG="$1" "${@:2}"' \
    _ "$SEAT_SCOPE" "$LOG_LEVEL" "$COMP" "$SOCKET" "${COMP_ARGS[@]}" >"$COMP_LOG" 2>&1 &
else
  LUMEN_LOG="$LOG_LEVEL" "$COMP" "$SOCKET" "${COMP_ARGS[@]}" >"$COMP_LOG" 2>&1 &
fi
COMP_PID=$!

# Ready when the control socket answers (it listens once the Wayland socket is served), not
# after a fixed sleep; a compositor that died on startup is reported instead of waited on.
CONTROL_PATH="${SOCKET_PATH}.varlink"
CTL="${BUILD}/lumen-ctl"
for _ in $(seq 1 100); do
  if [[ -x "$CTL" ]]; then
    "$CTL" --socket "$CONTROL_PATH" version >/dev/null 2>&1 && break
  else
    [[ -S "$SOCKET_PATH" ]] && break
  fi
  kill -0 "$COMP_PID" 2>/dev/null || break
  sleep 0.05
done
if [[ ! -S "$SOCKET_PATH" ]]; then
  ENDED_BY="compositor did not start"
else
  export WAYLAND_DISPLAY="$SOCKET" LUMEN_SOCKET="$CONTROL_PATH"
  # Clients go to Lumen, not to whatever else the environment points at.
  unset DISPLAY
  export GDK_BACKEND=wayland QT_QPA_PLATFORM=wayland SDL_VIDEODRIVER=wayland
  export XDG_CURRENT_DESKTOP=Lumen XDG_SESSION_TYPE=wayland

  if (( WITH_SHELL )); then
    start_shell
    # Let the panel reserve its strip before the first window is placed, or that window
    # is placed against the whole output and sits under the panel. Bounded: a shell that
    # fails to come up must not stop the session.
    for _ in $(seq 1 60); do
      grep -q "shell.panel: configure" "$SHELL_LOG" 2>/dev/null && break
      kill -0 "$SHELL_PID" 2>/dev/null || break
      sleep 0.05
    done
  fi
  if (( WITH_PORTALS )); then
    portal_session_start "$SOCKET" "$BUILD" "$LOG_DIR" >"$LOG_DIR/portals.log" 2>&1
  fi
  "${CLIENT[@]}" >"$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!

  # Until a logout, the compositor dies, or the limit passes. The first client exiting is
  # not an ending: it is one window of the session, and closing it (its close button,
  # `exit` in foot) must not take the other windows and the panel down with it.
  deadline=$(( $(date +%s) + LIMIT ))
  while :; do
    if grep -q "compositor.session: logout requested" "$COMP_LOG" 2>/dev/null; then
      # The compositor is closing every window and will stop on its own (5 s grace).
      # Stopping it here would cut that short for windows still saving or closing.
      for _ in $(seq 1 100); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
      ENDED_BY="logged out"
      break
    fi
    if [[ -n "$CLIENT_PID" ]] && ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null; CLIENT_RC=$?; CLIENT_PID=""
    fi
    # A crashed shell comes back, as lumen-shell.service restarts it in a systemd session:
    # the compositor replays its state and has held the panel's strip meanwhile.
    if [[ -n "$SHELL_PID" ]] && ! kill -0 "$SHELL_PID" 2>/dev/null; then
      wait "$SHELL_PID" 2>/dev/null; shell_rc=$?; SHELL_PID=""
      if (( shell_rc != 0 && RESTART_SHELL )) && kill -0 "$COMP_PID" 2>/dev/null &&
         ! grep -q "compositor.session: logout requested" "$COMP_LOG" 2>/dev/null; then
        now=$(date +%s)
        recent=()
        for t in "${SHELL_RESTARTS[@]}"; do (( now - t < 30 )) && recent+=("$t"); done
        SHELL_RESTARTS=("${recent[@]}" "$now")
        if (( ${#SHELL_RESTARTS[@]} <= 5 )); then
          echo "[run-desktop] lumen-shell exited ($shell_rc); restarting it" >>"$SHELL_LOG"
          sleep 0.3
          start_shell
        else
          echo "[run-desktop] lumen-shell keeps failing; not restarted again" >>"$SHELL_LOG"
        fi
      fi
    fi
    if ! kill -0 "$COMP_PID" 2>/dev/null; then
      ENDED_BY="compositor exited"
      break
    fi
    if (( LIMIT > 0 && $(date +%s) >= deadline )); then
      ENDED_BY="time limit reached"
      break
    fi
    sleep 0.2
  done
fi

cleanup
trap - EXIT

# ── Summary (the console is back) ────────────────────────────────────────────
echo "Session ended: $ENDED_BY."
echo "  compositor exit=${COMP_RC:-?}${CLIENT_RC:+  ${CLIENT[0]} exit=$CLIENT_RC}"
errors=$(grep -cE '^\[(ERROR|WARN)\]' "$COMP_LOG" 2>/dev/null || true)
(( errors > 0 )) && { echo "  compositor errors/warnings: $errors (first ones below)"; grep -m5 -E '^\[(ERROR|WARN)\]' "$COMP_LOG" | sed 's/^/    /'; }
[[ "$ENDED_BY" == "compositor did not start" ]] && tail -15 "$COMP_LOG" | sed 's/^/    /'
echo "  logs: $COMP_LOG"
(( WITH_SHELL )) && echo "        $SHELL_LOG"
(( WITH_PORTALS )) && echo "        $LOG_DIR/portals.log (xdpw.log, portal-lumen.log)"
echo "        $CLIENT_LOG"
[[ "$ENDED_BY" == "logged out" && "${COMP_RC:-1}" == "0" ]]
