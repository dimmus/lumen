# Portal environment of a Lumen session (design-app-compat-capture E.4). Sourced by
# run-desktop.sh and app-smoke.sh; not run on its own.
#
#   portal_session_start SOCKET BUILD LOG_DIR [XDPW_CONFIG]
#   portal_session_stop
#
# What a desktop session must do so xdg-desktop-portal serves its applications (sway's
# documented setup, done reversibly):
#   1. Export WAYLAND_DISPLAY, XDG_CURRENT_DESKTOP=Lumen and XDG_SESSION_TYPE=wayland to the
#      systemd user manager — the environment D-Bus-activated portal backends start with
#      (every portal service file names its systemd unit, so the manager's environment is
#      the one that counts, and it can be restored exactly; dbus-broker's own activation
#      environment cannot be unset again, so it is left alone).
#   2. For a build-tree run, point the frontend at a directory holding the system's
#      .portal files plus the build's lumen.portal and lumen-portals.conf
#      (XDG_DESKTOP_PORTAL_DIR; nothing under ~/.config is written).
#   3. Start the backends from the build as this script's children, replacing any running
#      instance (-r): xdg-desktop-portal-wlr with Lumen's config (ScreenCast; its source
#      chooser is xdg-desktop-portal-lumen --choose) and xdg-desktop-portal-lumen
#      (Screenshot, PickColor). Their logs go to LOG_DIR.
#   4. Start a PipeWire session manager (WirePlumber) if none runs: without one a cast's
#      stream is never linked to its consumer.
#   5. Restart xdg-desktop-portal.service so it reads the Lumen configuration.
# portal_session_stop undoes each step: the backends stopped by PID, units that came up
# because of the session (the gtk backend, WirePlumber if started here) stopped, the
# manager's environment restored to what it was, the frontend restarted once more so it
# leaves the Lumen configuration (or stopped, if it was not running before).

PORTAL_VARS=(WAYLAND_DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_TYPE XDG_DESKTOP_PORTAL_DIR)
PORTAL_ACTIVE=0
PORTAL_XDPW_PID=""
PORTAL_LUMEN_PID=""
PORTAL_DIR=""
PORTAL_STARTED_WP=0
PORTAL_PREV_ENV=()
PORTAL_PREV_UNITS=()

portal__log() { echo "portal: $*"; }

portal__unit_active() { systemctl --user is-active --quiet "$1" 2>/dev/null; }

portal__owner_pid() { # bus name → owner PID (empty when none)
  busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus \
    GetConnectionUnixProcessID s "$1" 2>/dev/null | awk '{print $2}'
}

portal__wait_owner() { # name pid seconds — the name is owned by pid
  local deadline=$(( $(date +%s%N) + ${3}000000000 ))
  while (( $(date +%s%N) < deadline )); do
    [[ "$(portal__owner_pid "$1")" == "$2" ]] && return 0
    kill -0 "$2" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}

portal__stop_pid() {
  local pid=$1
  [[ -n "$pid" ]] || return 0
  kill -TERM "$pid" 2>/dev/null
  for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

portal_session_start() { # SOCKET BUILD LOG_DIR [XDPW_CONFIG]
  local socket=$1 build=$2 logs=$3 xdpw_config=${4:-}
  local frontend=/usr/lib/xdg-desktop-portal xdpw=/usr/lib/xdg-desktop-portal-wlr
  local backend="$build/xdg-desktop-portal-lumen"
  if ! command -v systemctl >/dev/null || ! command -v busctl >/dev/null ||
     ! systemctl --user show-environment >/dev/null 2>&1; then
    portal__log "no systemd user manager: portals not set up"
    return 1
  fi
  if [[ ! -x "$frontend" ]]; then
    portal__log "xdg-desktop-portal is not installed: portals not set up"
    return 1
  fi
  if [[ ! -x "$backend" ]]; then
    portal__log "$backend is missing (LUMEN_BUILD_PORTAL): portals not set up"
    return 1
  fi
  mkdir -p "$logs"

  # What to restore afterwards.
  local env v
  env=$(systemctl --user show-environment 2>/dev/null)
  PORTAL_PREV_ENV=()
  for v in "${PORTAL_VARS[@]}"; do
    if grep -q "^${v}=" <<<"$env"; then
      PORTAL_PREV_ENV+=("$(grep -m1 "^${v}=" <<<"$env")")
    else
      PORTAL_PREV_ENV+=("-$v")
    fi
  done
  PORTAL_PREV_UNITS=()
  local u
  for u in xdg-desktop-portal.service xdg-desktop-portal-gtk.service xdg-desktop-portal-wlr.service; do
    portal__unit_active "$u" && PORTAL_PREV_UNITS+=("$u")
  done
  PORTAL_ACTIVE=1

  # 2. The build tree's portal files, beside the system's.
  local set_env=("WAYLAND_DISPLAY=$socket" "XDG_CURRENT_DESKTOP=Lumen" "XDG_SESSION_TYPE=wayland")
  if [[ -f "$build/share/xdg-desktop-portal/lumen-portals.conf" ]]; then
    PORTAL_DIR="${XDG_RUNTIME_DIR}/lumen-portals-${socket}"
    rm -rf "$PORTAL_DIR"
    mkdir -m 0700 "$PORTAL_DIR"
    local f
    for f in /usr/share/xdg-desktop-portal/portals/*.portal; do
      [[ -e "$f" && "$(basename "$f")" != lumen.portal ]] && ln -s "$f" "$PORTAL_DIR/"
    done
    ln -s "$build/share/xdg-desktop-portal/portals/lumen.portal" "$PORTAL_DIR/lumen.portal"
    ln -s "$build/share/xdg-desktop-portal/lumen-portals.conf" "$PORTAL_DIR/lumen-portals.conf"
    set_env+=("XDG_DESKTOP_PORTAL_DIR=$PORTAL_DIR")
  fi

  # 1. The session environment.
  systemctl --user set-environment "${set_env[@]}"

  # 3. The backends, as children.
  if [[ -x "$xdpw" ]]; then
    [[ -n "$xdpw_config" ]] || xdpw_config="$build/share/xdg/xdg-desktop-portal-wlr/Lumen"
    [[ -f "$xdpw_config" ]] || xdpw_config=""
    WAYLAND_DISPLAY="$socket" XDG_CURRENT_DESKTOP=Lumen \
      "$xdpw" -r -l DEBUG ${xdpw_config:+-c "$xdpw_config"} >"$logs/xdpw.log" 2>&1 &
    PORTAL_XDPW_PID=$!
    if ! portal__wait_owner org.freedesktop.impl.portal.desktop.wlr "$PORTAL_XDPW_PID" 5; then
      portal__log "xdg-desktop-portal-wlr did not come up (see $logs/xdpw.log)"
    fi
  else
    portal__log "xdg-desktop-portal-wlr is not installed: no ScreenCast"
  fi
  WAYLAND_DISPLAY="$socket" XDG_CURRENT_DESKTOP=Lumen "$backend" -r >"$logs/portal-lumen.log" 2>&1 &
  PORTAL_LUMEN_PID=$!
  if ! portal__wait_owner org.freedesktop.impl.portal.desktop.lumen "$PORTAL_LUMEN_PID" 5; then
    portal__log "xdg-desktop-portal-lumen did not come up (see $logs/portal-lumen.log)"
  fi

  # 4. A session manager for PipeWire.
  if portal__unit_active pipewire.service || portal__unit_active pipewire.socket; then
    if ! portal__unit_active wireplumber.service && ! portal__unit_active pipewire-media-session.service; then
      if systemctl --user start wireplumber.service 2>/dev/null; then
        PORTAL_STARTED_WP=1
      else
        portal__log "no PipeWire session manager could be started: casts will not link"
      fi
    fi
  fi

  # 5. The frontend, with the Lumen configuration.
  systemctl --user restart xdg-desktop-portal.service
  if ! portal__wait_owner org.freedesktop.portal.Desktop \
       "$(systemctl --user show -p MainPID --value xdg-desktop-portal.service)" 5; then
    portal__log "xdg-desktop-portal did not come back"
  fi
  portal__log "ready (frontend pid $(systemctl --user show -p MainPID --value xdg-desktop-portal.service)," \
              "xdpw ${PORTAL_XDPW_PID:-none}, lumen ${PORTAL_LUMEN_PID})"
  return 0
}

portal__stop_session_units() { # backends not running before the session: stopped, failures cleared
  local u
  for u in xdg-desktop-portal-gtk.service xdg-desktop-portal-wlr.service; do
    [[ " ${PORTAL_PREV_UNITS[*]} " == *" $u "* ]] && continue
    portal__unit_active "$u" && systemctl --user stop "$u"
    systemctl --user reset-failed "$u" 2>/dev/null
  done
}

portal_session_stop() {
  (( PORTAL_ACTIVE )) || return 0
  PORTAL_ACTIVE=0
  portal__stop_pid "$PORTAL_LUMEN_PID"; PORTAL_LUMEN_PID=""
  portal__stop_pid "$PORTAL_XDPW_PID"; PORTAL_XDPW_PID=""
  portal__stop_session_units
  if (( PORTAL_STARTED_WP )); then
    systemctl --user stop wireplumber.service
    PORTAL_STARTED_WP=0
  fi
  local e unset_vars=() set_vars=()
  for e in "${PORTAL_PREV_ENV[@]}"; do
    if [[ "$e" == -* ]]; then unset_vars+=("${e#-}"); else set_vars+=("$e"); fi
  done
  (( ${#unset_vars[@]} )) && systemctl --user unset-environment "${unset_vars[@]}"
  (( ${#set_vars[@]} )) && systemctl --user set-environment "${set_vars[@]}"
  if [[ " ${PORTAL_PREV_UNITS[*]} " == *" xdg-desktop-portal.service "* ]]; then
    systemctl --user restart xdg-desktop-portal.service
    # The restarted frontend may activate backends against the old environment (a display
    # that is gone): they fail and are cleared again, so the units end as they began.
    sleep 1
  else
    systemctl --user stop xdg-desktop-portal.service
  fi
  portal__stop_session_units
  [[ -n "$PORTAL_DIR" ]] && rm -rf "$PORTAL_DIR"
  PORTAL_DIR=""
}
