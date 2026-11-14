# The evdev nodes the smoke scripts inject into, found at start. Sourced by drag-smoke.sh,
# app-smoke.sh, ui-bench.sh (--e2e) and seat-session.sh; not run on its own.
#
#   inject_pick_devices [SYSFS_ROOT [DEV_ROOT]]
#       sets POINTER_DEV KEYBOARD_DEV RELATIVE_DEV and prints one line saying what it
#       chose and why
#   inject_node_has_rel_motion NODE [SYSFS_ROOT]
#       0 when the node's device has REL_X and REL_Y
#
# An explicit LUMEN_INJECT_POINTER / LUMEN_INJECT_KEYBOARD / LUMEN_INJECT_RELATIVE always
# wins (the documented override; see .zcode/skills/lumen-build-run). Without it the nodes
# are located by device name, so a re-enumerated device is found wherever the kernel put
# it: a virtual mouse re-created as event18 after being event16 must not turn a whole smoke
# into "FAIL: /dev/input/event16 is not an input device node" (the close-ghost gate died
# exactly there — the node number changes on every re-creation, and a number written into
# an invocation goes stale). Names looked up:
#
#   "Lumen Inject Mouse"             scripts/uinput-mouse.py's virtual mouse: the absolute
#                                    pointer, and the relative node (REL_X/REL_Y and the
#                                    wheel ride on the same device)
#   "AT Translated Set 2 keyboard"   the internal keyboard (the same name on the dev VM,
#                                    where it is event1, and on the notebook, event3)
#
# Several devices may carry the mouse's name (a holder started by an older
# uinput-mouse.py next to a current one): one that can do everything — absolute position
# and relative motion — is preferred, then the newest node. A device made before
# uinput-mouse.py declared REL_X/REL_Y still serves as the pointer and carries the wheel;
# relative motion written to it is dropped by the kernel, and the scenarios that need it
# say so. Nothing found: the dev VM's nodes (event2 absolute, event3 relative, event1
# keyboard), as before.

INJECT_POINTER_NAME="Lumen Inject Mouse"
INJECT_KEYBOARD_NAME="AT Translated Set 2 keyboard"

# inject__cap FILE — the low two bits of a sysfs capability mask (X and Y), 0 when absent.
inject__cap() {
  local mask
  mask=$(cat "$1" 2>/dev/null) || mask=0
  mask=${mask##* }  # the last word holds the lowest bits
  [[ "$mask" =~ ^[0-9a-fA-F]+$ ]] || mask=0
  echo $(( (16#$mask) & 3 ))
}

# inject_nodes_for_name NAME SYSFS_ROOT DEV_ROOT — "node number abs rel" for every input
# device named NAME, newest event node first; abs and rel are 3 when the device has both
# the X and the Y axis of that kind.
inject_nodes_for_name() {
  local name=$1 root=$2 devroot=$3 d e ev
  for d in "$root"/class/input/input[0-9]*; do
    [[ -f "$d/name" && "$(<"$d/name")" == "$name" ]] || continue
    ev=-1
    for e in "$d"/event[0-9]*; do
      [[ -e "$e" ]] || continue
      (( ${e##*event} > ev )) && ev=${e##*event}
    done
    (( ev >= 0 )) || continue
    printf '%s %d %d %d\n' "$devroot/input/event$ev" "$ev" \
      "$(inject__cap "$d/capabilities/abs")" "$(inject__cap "$d/capabilities/rel")"
  done | sort -t' ' -k2,2nr
}

inject_node_has_rel_motion() { # NODE [SYSFS_ROOT]
  (( $(inject__cap "${2:-/sys}/class/input/${1##*/}/device/capabilities/rel") == 3 ))
}

inject_pick_devices() { # [SYSFS_ROOT [DEV_ROOT]]
  local root=${1:-/sys} devroot=${2:-/dev} node ev abs rel
  local any="" full="" note="injection:"
  while read -r node ev abs rel; do
    (( abs == 3 )) || continue
    if [[ -z "$any" ]]; then any=$node; fi
    if (( rel == 3 )) && [[ -z "$full" ]]; then full=$node; fi
    true   # never let a non-matching device end the loop with a failure (set -e callers)
  done < <(inject_nodes_for_name "$INJECT_POINTER_NAME" "$root" "$devroot")
  local mouse=${full:-$any}

  POINTER_DEV="${LUMEN_INJECT_POINTER:-}"
  RELATIVE_DEV="${LUMEN_INJECT_RELATIVE:-}"
  KEYBOARD_DEV="${LUMEN_INJECT_KEYBOARD:-}"
  if [[ -n "$POINTER_DEV" ]]; then
    note+=" pointer $POINTER_DEV (LUMEN_INJECT_POINTER)"
  elif [[ -n "$mouse" ]]; then
    POINTER_DEV=$mouse
    note+=" pointer $POINTER_DEV ('$INJECT_POINTER_NAME')"
  else
    POINTER_DEV=/dev/input/event2   # the VMware console's absolute mouse
    note+=" pointer $POINTER_DEV (default: no '$INJECT_POINTER_NAME')"
  fi
  if [[ -n "$RELATIVE_DEV" ]]; then
    note+=", relative $RELATIVE_DEV (LUMEN_INJECT_RELATIVE)"
  elif [[ -n "$mouse" ]]; then
    RELATIVE_DEV=$mouse
    note+=", relative $RELATIVE_DEV (the same device"
    [[ -n "$full" ]] && note+=")" || note+=": wheel only, no REL_X/REL_Y)"
  else
    RELATIVE_DEV=/dev/input/event3  # the VMware mouse's relative node
    note+=", relative $RELATIVE_DEV (default)"
  fi
  if [[ -n "$KEYBOARD_DEV" ]]; then
    note+=", keyboard $KEYBOARD_DEV (LUMEN_INJECT_KEYBOARD)"
  else
    # `|| true`: no such device is not an error (ui-bench.sh sources this under set -e).
    read -r KEYBOARD_DEV _ < <(inject_nodes_for_name "$INJECT_KEYBOARD_NAME" "$root" "$devroot") || true
    if [[ -n "$KEYBOARD_DEV" ]]; then
      note+=", keyboard $KEYBOARD_DEV ('$INJECT_KEYBOARD_NAME')"
    else
      KEYBOARD_DEV=/dev/input/event1  # the VMware VM's AT keyboard
      note+=", keyboard $KEYBOARD_DEV (default: no '$INJECT_KEYBOARD_NAME')"
    fi
  fi
  echo "     $note"
}
