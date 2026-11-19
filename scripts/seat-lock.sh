#!/usr/bin/env bash
# The Lumen seat broker: one lock, one priority queue, one preflight.
#
# Every job that starts a compositor, injects input, or measures on the real
# seat (the smoke scripts, ui-bench, vkmark-bench, run-desktop, the screenshot
# agent, ~/lumen-compare's compare.sh) must hold a ticket for the whole job.
# Two seat jobs on one host contaminate each other's evidence — the memory
# archive carries three separate incident families from exactly that
# ("concurrent post-close smokes re-wrote the predecessor gate-workflow
# namespace", "the gate-time step overwrote the at-level seat evidence twice",
# the concurrent-session WARN family). Seat evidence produced without the
# broker is INVALID, not a warning.
#
#   scripts/seat-lock.sh acquire  <class> <owner> [--wait S] [--ttl-min N]
#                                  → prints "ticket <id>"; exit 0 = held,
#                                    3 = stale lock (owner decides via
#                                    force-release), 4 = wait timeout, 2 = misuse
#   scripts/seat-lock.sh release  <ticket-id>
#   scripts/seat-lock.sh force-release <ticket-id>   (owner decision, logged)
#   scripts/seat-lock.sh renew    <ticket-id>         (refresh the TTL heartbeat)
#   scripts/seat-lock.sh status                       (holder, queue, preflight)
#   scripts/seat-lock.sh preflight                    (the quiet-seat triple alone)
#   scripts/seat-lock.sh with <class> <owner> [--wait S] [--no-preflight] -- <cmd...>
#                                  acquire + preflight + run + release-on-trap;
#                                  exports LUMEN_SEAT_TICKET/LOCKED/CLASS to the job
#
# Classes (priority order; the user's interactive tty session outranks all and
# is never brokered — it is what preflight detects): bench > smoke > screenshot.
# Within a class FIFO by enqueue time. A running job is never preempted.
#
# Tickets live in XDG_RUNTIME_DIR/lumen-seat/ — per-host by construction (the
# two hosts sharing the name "iritur" have separate runtime dirs); each ticket
# records the detected DRM driver so reports name the host. A ticket whose
# holder pid is dead and whose TTL expired is STALE: reported with the exact
# force-release command, never auto-broken — the owner decides.
#
# Exit codes follow the smoke convention where they can: 0 ok, 2 could not
# run (misuse, contamination), 3 invalid/conflicting seat state.
set -u

RUN_BASE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/lumen-seat"
CTRL="$RUN_BASE/seat.lock"
HELD="$RUN_BASE/held"
QUEUE_DIR="$RUN_BASE/queue"
DEFAULT_WAIT=900          # seconds a queued job waits before giving up (exit 4)
DEFAULT_TTL_MIN=25        # heartbeat window; long jobs renew

usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }
now() { date +%s; }

class_rank() { # class → rank (0 best) or fail
  case "$1" in
    bench) echo 0 ;;
    smoke) echo 1 ;;
    screenshot) echo 2 ;;
    *) return 1 ;;
  esac
}

host_driver() { # vmwgfx → VM, i915/nouveau → notebook (the lumen-build-run rule)
  local d
  for d in /sys/class/drm/card*/device/driver; do
    readlink -f "$d" 2>/dev/null | awk -F/ '{print $NF}'
  done | sort -u | tr '\n' ' '
}

field() { # FILE KEY → value from a key=value ticket file
  sed -n "s/^$2=//p" "$1" 2>/dev/null | head -1
}

pid_alive() { [[ -n "${1:-}" && -d "/proc/$1" ]]; }

# Run a critical section under the control lock. The section communicates by
# printing to stdout; the caller parses. fd 9 lives for the section's duration.
with_ctrl() {
  (
    flock -w 20 9 || { echo "LOCKTIMEOUT"; exit 2; }
    "$@"
  ) 9>"$CTRL"
}

# The quiet-seat triple — one implementation (was copy-pasted through the
# smoke scripts and the studio workflow; grep of /proc/*/comm because comm is
# 15-char truncated and pgrep -x lumen-compositor never matches).
preflight() {
  local bad=0 p
  for p in $(grep -l -e lumen-composito -e '^sway$' -e '^weston$' -e '^hyprland$' \
               /proc/[0-9]*/comm 2>/dev/null | head -5); do
    echo "FAIL: compositor running ($(dirname "$p" | xargs basename 2>/dev/null): $(cat "$p"))"
    bad=1
  done
  # A smoke script is "running" when a real one executes — not this broker's
  # own wrapper cmdline (it names the job it is about to start), not our own
  # ancestors (their cmdlines carry the very command text that matched), and
  # not transient pipeline forks (alive for milliseconds). A real smoke runs
  # for tens of seconds: candidates are re-verified after a grace period.
  local smoke="" pid self_ancestors
  self_ancestors=" $$"
  local walk=$$
  while [[ "$walk" -gt 1 ]]; do
    walk=$(awk '{print $4}' "/proc/$walk/stat" 2>/dev/null) || break
    [[ -n "$walk" ]] || break
    self_ancestors+=" $walk"
  done
  local candidates=""
  for pid in $(pgrep -f '^bash .*-smoke\.sh' | head -8); do
    [[ " $self_ancestors " == *" $pid "* ]] && continue
    grep -aq seat-lock.sh "/proc/$pid/cmdline" 2>/dev/null && continue
    candidates+=" $pid"
  done
  [[ -n "$candidates" ]] && sleep 0.4
  for pid in $candidates; do
    [[ -d "/proc/$pid" ]] || continue
    tr "\0" " " <"/proc/$pid/cmdline" 2>/dev/null | grep -aq -- '-smoke\.sh' || continue
    smoke+="$pid "
  done
  [[ -n "${smoke// /}" ]] && {
    echo "FAIL: a smoke session is running:"
    for pid in $smoke; do
      echo "  pid $pid: $(tr "\0" " " <"/proc/$pid/cmdline" 2>/dev/null | head -c 120)"
    done
    bad=1
  }
  echo "loadavg: $(cat /proc/loadavg 2>/dev/null)"
  if (( bad )); then return 1; fi
  echo "ok: no compositor, no smoke"
}

# --- critical sections (called via with_ctrl) ---------------------------------

cs_try_grant() { # ID RANK TTL_MIN — GRANTED | QUEUED | BUSY | STALE <id> <pid>
  local id=$1 ttl_min=$3
  local t
  t=$(now)
  if [[ -f "$HELD" ]]; then
    local hpid mtime httl
    hpid=$(field "$HELD" pid)
    mtime=$(stat -c %Y "$HELD" 2>/dev/null || echo 0)
    # The HOLDER's requested TTL governs staleness, not whoever is asking —
    # a holder that asked --ttl-min 90 must not be judged stale at 25.
    httl=$(field "$HELD" ttl); [[ -n "$httl" ]] || httl=$ttl_min
    if (( t - mtime > httl * 60 )) && ! pid_alive "$hpid"; then
      echo "STALE $(field "$HELD" id) $hpid"
      return 0
    fi
    echo "BUSY"
    return 0
  fi
  # Seat free: grant only to the best queued ticket (rank, then FIFO).
  # Waiting jobs whose pid died are swept first — they can never take it.
  local f best="" best_key="" key
  for f in "$QUEUE_DIR"/*; do
    [[ -f "$f" ]] || continue
    pid_alive "$(field "$f" pid)" || { rm -f "$f"; continue; }
    key="$(field "$f" rank) $(field "$f" enqueued)"
    if [[ -z "$best_key" || "$key" < "$best_key" ]]; then best="$f"; best_key="$key"; fi
  done
  if [[ "$(field "$best" id 2>/dev/null)" == "$id" ]]; then
    printf 'id=%s\nclass=%s\nowner=%s\npid=%s\nstarted=%s\nttl=%s\ndriver=%s\n' \
      "$id" "$(field "$best" class)" "$(field "$best" owner)" "$$" "$(now)" "${LUMEN_SEAT_TTL_MIN:-$DEFAULT_TTL_MIN}" "$(host_driver)" >"$HELD.tmp" && mv "$HELD.tmp" "$HELD"
    rm -f "$best"
    echo "GRANTED $id"
  else
    echo "QUEUED"
  fi
}

cs_release() { # ID [force]
  local id=$1 force=${2:-}
  if [[ -f "$HELD" ]]; then
    if [[ "$(field "$HELD" id)" == "$id" || "$force" == force ]]; then
      rm -f "$HELD"
      echo "RELEASED"
    else
      echo "MISMATCH $(field "$HELD" id)"
    fi
  else
    echo "RELEASED"
  fi
  rm -f "$QUEUE_DIR/$id"
}

cs_renew() { # ID
  [[ -f "$HELD" && "$(field "$HELD" id)" == "$1" ]] && touch "$HELD"
  true
}

# --- subcommands ---------------------------------------------------------------

cmd_acquire() {
  [[ $# -ge 2 ]] || usage 2
  local class=$1 owner=$2 wait=$DEFAULT_WAIT
  shift 2
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --wait) wait=$2; shift 2 ;;
      --ttl-min) export LUMEN_SEAT_TTL_MIN=$2; shift 2 ;;
      *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
  done
  class_rank "$class" >/dev/null || { echo "unknown class: $class (bench|smoke|screenshot)" >&2; exit 2; }
  mkdir -p "$QUEUE_DIR"

  local id="ticket-$(date +%Y%m%d%H%M%S)-$$-$RANDOM"
  # Atomic publish (tmp + mv): a ctrl-lock sweep that reads a half-written
  # ticket sees pid= and deletes a LIVE waiter's queue slot.
  printf 'id=%s\nclass=%s\nowner=%s\nrank=%s\nenqueued=%s\npid=%s\n' \
    "$id" "$class" "$owner" "$(class_rank "$class")" "$(now)" "$$" >"$QUEUE_DIR/$id.tmp" && mv "$QUEUE_DIR/$id.tmp" "$QUEUE_DIR/$id"

  local deadline t out
  t=$(now)
  deadline=$(( t + wait ))
  while :; do
    out=$(with_ctrl cs_try_grant "$id" "$(class_rank "$class")" "${LUMEN_SEAT_TTL_MIN:-$DEFAULT_TTL_MIN}")
    case "$out" in
      GRANTED*)
        echo "ticket $id"
        echo "seat-lock: acquired class $class owner $owner ticket $id" >&2
        return 0
        ;;
      STALE*)
        rm -f "$QUEUE_DIR/$id"
        # shellcheck disable=SC2086
        set -- $out
        echo "STALE: seat held by dead ticket $2 (pid $3) past its TTL — owner decision required, never auto-broken:" >&2
        echo "  scripts/seat-lock.sh force-release $2" >&2
        return 3
        ;;
      BUSY|QUEUED) : ;;
      *)
        rm -f "$QUEUE_DIR/$id"
        echo "acquire: unexpected broker state: $out" >&2
        return 2
        ;;
    esac
    t=$(now)
    if (( t >= deadline )); then
      rm -f "$QUEUE_DIR/$id"
      echo "seat-lock: waited ${wait}s without getting the seat — COULD NOT RUN" >&2
      return 4
    fi
    sleep 2
  done
}

cmd_release() {
  [[ -n "${1:-}" ]] || { echo "release: ticket id required" >&2; return 2; }
  local out
  out=$(with_ctrl cs_release "$1")
  case "$out" in
    RELEASED) echo "seat-lock: released $1" >&2 ;;
    MISMATCH*) echo "release: held ticket is $out — refusing; force-release it if the owner decided so" >&2; return 3 ;;
    *) return 2 ;;
  esac
}

cmd_force_release() {
  [[ -n "${1:-}" ]] || { echo "force-release: ticket id required" >&2; return 2; }
  with_ctrl cs_release "$1" force >/dev/null || return 2
  echo "seat-lock: FORCE-released $1 (owner decision — record why in the session/report)" >&2
}

cmd_renew() {
  [[ -n "${1:-}" ]] || { echo "renew: ticket id required" >&2; return 2; }
  with_ctrl cs_renew "$1" || return 2
}

cmd_status() {
  mkdir -p "$QUEUE_DIR"
  echo "host drivers: $(host_driver)"
  if [[ -f "$HELD" ]]; then
    local id class owner pid started age
    id=$(field "$HELD" id); class=$(field "$HELD" class); owner=$(field "$HELD" owner)
    pid=$(field "$HELD" pid); started=$(field "$HELD" started)
    local mtime ttl t
    mtime=$(stat -c %Y "$HELD" 2>/dev/null || echo "$started")
    t=$(now)
    age=$(( t - mtime ))
    echo "holder: ticket $id class $class owner $owner pid $pid age ${age}s alive $(pid_alive "$pid" && echo yes || echo NO)"
    local httl; httl=$(field "$HELD" ttl); [[ -n "$httl" ]] || httl=${LUMEN_SEAT_TTL_MIN:-$DEFAULT_TTL_MIN}
    ttl=$(( httl * 60 ))
    if (( t - mtime > ttl )) && ! pid_alive "$pid"; then
      echo "STALE: holder pid is dead and the TTL passed — owner decision required:"
      echo "  scripts/seat-lock.sh force-release $id"
      return 1
    fi
  else
    echo "holder: none"
  fi
  local q=0 f
  for f in "$QUEUE_DIR"/*; do
    [[ -f "$f" ]] || continue
    echo "queued: $(field "$f" class) $(field "$f" owner) (since $(date -d "@$(field "$f" enqueued)" +%H:%M:%S 2>/dev/null))"
    q=$((q + 1))
  done
  echo "queue: $q job(s)"
  preflight || return 1
}

cmd_with() {
  [[ $# -ge 2 ]] || usage 2
  local class=$1 owner=$2
  shift 2
  local wait=$DEFAULT_WAIT no_preflight=0
  while [[ $# -gt 0 && "$1" != "--" ]]; do
    case "$1" in
      --wait) wait=$2; shift 2 ;;
      --ttl-min) export LUMEN_SEAT_TTL_MIN=$2; shift 2 ;;
      --no-preflight) no_preflight=1; shift ;;
      *) echo "with: unknown option $1" >&2; exit 2 ;;
    esac
  done
  [[ ${1:-} == "--" ]] || { echo "with: missing -- before the command" >&2; exit 2; }
  shift
  [[ $# -gt 0 ]] || { echo "with: no command" >&2; exit 2; }
  class_rank "$class" >/dev/null || { echo "unknown class: $class (bench|smoke|screenshot)" >&2; exit 2; }

  local out id rc
  out=$(cmd_acquire "$class" "$owner" --wait "$wait") || { rc=$?; echo "$out" >&2; return "$rc"; }
  id=${out##*ticket }
  export LUMEN_SEAT_TICKET=$id LUMEN_SEAT_LOCKED=1 LUMEN_SEAT_CLASS=$class
  # ${id:-}: at script exit the locals are gone; the trap must not crash then.
  trap 'cmd_release "${id:-}" >/dev/null 2>&1' EXIT INT TERM

  # Preflight inside the lock (one implementation of the triple). Screenshots
  # shoot a session that is supposed to be running — they skip it.
  if (( ! no_preflight )) && [[ "$class" != screenshot ]]; then
    if ! preflight; then
      echo "seat-lock: preflight failed inside the lock — the seat is contaminated by an unbrokered job" >&2
      cmd_release "$id" >/dev/null 2>&1
      trap - EXIT INT TERM
      return 2
    fi
  fi
  "$@"
  rc=$?
  cmd_release "$id" >/dev/null 2>&1
  trap - EXIT INT TERM
  return $rc
}

[[ $# -ge 1 ]] || usage 2
cmd=$1; shift
case "$cmd" in
  -h|--help) usage 0 ;;
  acquire) cmd_acquire "$@" ;;
  release) cmd_release "$@" ;;
  force-release) cmd_force_release "$@" ;;
  renew) cmd_renew "$@" ;;
  status) cmd_status "$@" ;;
  preflight) preflight ;;
  with) cmd_with "$@" ;;
  *) echo "unknown command: $cmd (see --help)" >&2; exit 2 ;;
esac
