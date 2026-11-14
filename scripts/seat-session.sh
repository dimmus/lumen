#!/usr/bin/env bash
# The generic half of an interactive seat session (the run/finish helpers task evidence
# dirs hand-rolled until now): prechecks, the owner's configuration backed up and restored
# byte-identical, the logout key, the pointer parked. The task-specific half — which
# config edits to make, which client to launch — stays in the task's own launch/finish
# scripts, and every command here takes the run dir as an explicit argument:
#
#   scripts/seat-session.sh precheck  RUN_DIR           compositor/smoke down? load?
#   scripts/seat-session.sh postcheck RUN_DIR           the same, after the session
#   scripts/seat-session.sh config-backup RUN_DIR       copy config.toml + sha256
#   scripts/seat-session.sh config-restore RUN_DIR      put it back, prove it identical
#   scripts/seat-session.sh logout RUN_DIR              Ctrl+Alt+Backspace into inject-input
#   scripts/seat-session.sh park WXH [X Y]              move the pointer to a corner
#
# A session's artifacts belong in its own run dir, never one inferred from this script's
# location: the close-ghost gate's second session reused a finish script that hard-coded
# the first session's dir and trampled its teardown. The logout and park commands find
# the injection nodes by device name (scripts/inject-devices.sh), so a re-enumerated
# virtual mouse does not make them write to yesterday's node.
#
# Needs passwordless sudo (the injector writes to evdev nodes as root). Run the launch
# half before touching the seat, the finish half from where the session was started.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="${XDG_CONFIG_HOME:-$HOME/.config}/lumen/config.toml"

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }
[[ $# -ge 1 ]] || usage 2
cmd=$1
shift
case "$cmd" in
  -h|--help) usage 0 ;;
esac

# shellcheck source=inject-devices.sh
source "$ROOT/scripts/inject-devices.sh"

# One pass of host state into a file, with a verdict: a compositor holding DRM master or
# another smoke mid-run makes everything after it lie.
check_state() { # FILE LABEL
  local file=$1
  {
    echo "== $2 $(date -Is) =="
    echo "loadavg: $(cat /proc/loadavg)"
    if grep -l lumen-composito /proc/[0-9]*/comm 2>/dev/null; then
      echo "FAIL: lumen-compositor running (DRM master held)"
    else
      echo "ok: no lumen-compositor"
    fi
    if pgrep -f "^bash .*-smoke\.sh" >/dev/null; then   # plain -f smoke.sh matches this shell
      echo "FAIL: a smoke is running"
    else
      echo "ok: no smoke running"
    fi
    for c in /sys/class/drm/card*/device/driver; do readlink -f "$c"; done
    loginctl list-sessions --no-pager 2>/dev/null | head -6
  } >"$file"
  ! grep -q "^FAIL" "$file"
}

case "$cmd" in
  precheck)
    [[ $# -ge 1 ]] || usage 2
    mkdir -p "$1"
    check_state "$1/precheck.txt" precheck || { cat "$1/precheck.txt"; exit 1; }
    ;;
  postcheck)
    [[ $# -ge 1 ]] || usage 2
    check_state "$1/postcheck.txt" postcheck || { cat "$1/postcheck.txt"; exit 1; }
    ;;
  config-backup)
    [[ $# -ge 1 ]] || usage 2
    mkdir -p "$1"
    cp "$CFG" "$1/config-BEFORE.toml"
    sha256sum "$CFG" | tee "$1/config-sha256-BEFORE.txt"
    ;;
  config-restore)
    [[ $# -ge 1 ]] || usage 2
    sha256sum "$CFG" | tee "$1/config-sha256-AFTER-TEST.txt"
    cp "$1/config-BEFORE.toml" "$CFG"
    cmp "$1/config-BEFORE.toml" "$CFG" && echo "config restored byte-identical"
    sha256sum "$CFG" | tee "$1/config-sha256-RESTORED.txt"
    ;;
  logout)
    # Ctrl+Alt+Backspace: lumen-compositor's own logout binding, works without the panel.
    [[ $# -ge 1 ]] || usage 2
    inject_pick_devices >/dev/null
    sudo -n python3 -I "$ROOT/scripts/inject-input.py" --keyboard "$KEYBOARD_DEV" \
      key KEY_LEFTCTRL+KEY_LEFTALT+KEY_BACKSPACE tap >"$1/inject-logout.jsonl" 2>&1
    ;;
  park)
    # WXH is the output the compositor runs at; the default corner is (W-100, H-100).
    # The injector's record goes to stdout (redirect it into the run dir).
    [[ $# -ge 1 ]] || usage 2
    size=$1
    px=${2:-$(( ${size%x*} - 100 ))}
    py=${3:-$(( ${size#*x} - 100 ))}
    inject_pick_devices >/dev/null
    sudo -n python3 -I "$ROOT/scripts/inject-input.py" --pointer "$POINTER_DEV" \
      --output "$size" move "$px" "$py"
    ;;
  *) echo "unknown command: $cmd (see --help)"; exit 2 ;;
esac
