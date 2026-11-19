#!/usr/bin/env bash
# The studio run lock: one lumen-development-studio run per worktree root.
# Two concurrent studio runs collide on <WT_ROOT>/<task-id> worktrees and on
# the main checkout (memory commits) — the run lock fails the second run fast
# instead of letting them corrupt each other's trees.
#
#   scripts/run-lock.sh acquire  <owner>     # 0 = held by this run; 3 = held
#                                            # by a live run (details printed);
#                                            # 3 STALE = dead run past TTL,
#                                            # owner decides (force-release)
#   scripts/run-lock.sh heartbeat [<owner>]  # refresh (the workflow touches it
#                                            # at every task boundary)
#   scripts/run-lock.sh release  <owner>
#   scripts/run-lock.sh status
#   scripts/run-lock.sh force-release        # owner decision, logged in the report
#
# The lock file records pid/owner/started. Stale = no heartbeat for TTL
# (default 6 h — a live run heartbeats per task and no single silent step
# lasts that long). Stale is REPORTED with the exact force-release command,
# never auto-broken — the owner decides, same policy as the seat broker.
set -u

WT_ROOT="${LUMEN_WT_ROOT:-$(cd "$(dirname "$0")/.." && pwd | xargs dirname)/lumen-wt}"
LOCK="$WT_ROOT/.studio-run.lock"
DEFAULT_TTL_S=$(( 6 * 3600 ))

now() { date +%s; }
usage() { sed -n '2,/^set -u$/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit "${1:-0}"; }

write_lock() { # OWNER PID
  mkdir -p "$WT_ROOT"
  printf 'owner=%s\npid=%s\nstarted=%s\nhost=%s\n' "$1" "$2" "$(now)" "$(uname -n)" >"$LOCK"
}

stale() { # → 0 when the lock's heartbeat is older than the TTL
  local mtime ttl
  mtime=$(stat -c %Y "$LOCK" 2>/dev/null || echo 0)
  ttl=$(( ${LUMEN_RUN_LOCK_TTL_S:-$DEFAULT_TTL_S} ))
  local t; t=$(date +%s)
  (( t - mtime > ttl ))
}

describe() {
  local owner pid started
  owner=$(sed -n 's/^owner=//p' "$LOCK" 2>/dev/null)
  pid=$(sed -n 's/^pid=//p' "$LOCK" 2>/dev/null)
  started=$(sed -n 's/^started=//p' "$LOCK" 2>/dev/null)
  local alive=NO; [[ -n "$pid" && -d "/proc/$pid" ]] && alive=yes
  echo "run lock held by '$owner' (pid $pid, alive $alive, started $(date -d "@${started:-0}" +%FT%T 2>/dev/null), last heartbeat $(( $(date +%s) - $(stat -c %Y "$LOCK" 2>/dev/null || echo 0) ))s ago)"
}

cmd_acquire() {
  [[ -n "${1:-}" ]] || usage 2
  local owner=$1
  if [[ -f "$LOCK" ]]; then
    if stale; then
      describe >&2
      echo "STALE: no heartbeat past the TTL — owner decision required, never auto-broken:" >&2
      echo "  scripts/run-lock.sh force-release" >&2
      return 3
    fi
    describe >&2
    echo "another studio run holds the run lock; let it finish or force-release by owner decision" >&2
    return 3
  fi
  write_lock "$owner" "$$"
  echo "run-lock: acquired by '$owner' ($WT_ROOT/.studio-run.lock)"
}

cmd_heartbeat() {
  [[ -f "$LOCK" ]] || return 2
  touch "$LOCK"
  echo "run-lock: heartbeat"
}

cmd_release() {
  [[ -n "${1:-}" ]] || usage 2
  if [[ -f "$LOCK" ]] && [[ "$(sed -n 's/^owner=//p' "$LOCK")" == "$1" ]]; then
    rm -f "$LOCK"
    echo "run-lock: released by '$1'"
  else
    echo "run-lock: not held by '$1' — leaving it alone" >&2
    return 3
  fi
}

cmd_status() {
  if [[ -f "$LOCK" ]]; then
    describe
    stale && { echo "STALE — force-release is an owner decision:"; echo "  scripts/run-lock.sh force-release"; return 1; }
    return 0
  fi
  echo "no studio run lock in $WT_ROOT"
}

cmd_force_release() {
  [[ -f "$LOCK" ]] || { echo "no lock held"; return 0; }
  describe >&2
  rm -f "$LOCK"
  echo "run-lock: FORCE-released (owner decision — record why in the session/report)"
}

[[ $# -ge 1 ]] || usage 2
cmd=$1; shift
case "$cmd" in
  -h|--help) usage 0 ;;
  acquire) cmd_acquire "$@" ;;
  heartbeat) cmd_heartbeat "$@" ;;
  release) cmd_release "$@" ;;
  status) cmd_status "$@" ;;
  force-release) cmd_force_release "$@" ;;
  *) echo "unknown command: $cmd (see --help)" >&2; exit 2 ;;
esac
