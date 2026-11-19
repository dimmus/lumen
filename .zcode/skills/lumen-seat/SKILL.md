---
name: lumen-seat
description: The Lumen seat broker contract — scripts/seat-lock.sh acquire/release/with/status, the ticket model (one lock, priority queue bench>smoke>screenshot, TTL + never-auto-broken stale policy), which jobs must hold a ticket (every smoke, bench, run-desktop session, screenshot, compare.sh), the preflight triple it owns, and why seat evidence without a ticket is INVALID. Load before any seat work, before judging seat evidence, or when a smoke/bench reports the seat busy or stale.
---

# The seat broker (lumen-seat)

> **Scope:** mixed — the broker model (tickets, priority, stale=owner decision) is general; `scripts/seat-lock.sh` and its preflight are current-project.

One host, one seat, one DRM master, one broker. Every job that starts a
compositor, injects input, or measures on the real seat holds a ticket for its
whole run. Two seat jobs on one host contaminate each other's evidence — the
memory archive carries three incident families from exactly that
(`lumen-smoke-bench-harness-green.md`, `lumen-notebook-full-green-audit.md`,
the concurrent-session WARN family in `.zcode/TODO.md`). **Seat evidence
produced without a ticket is INVALID, not a warning.**

## The commands

```bash
scripts/seat-lock.sh status                      # holder, queue, preflight triple
scripts/seat-lock.sh preflight                   # the quiet-seat triple alone
scripts/seat-lock.sh with <class> <owner> [--wait S] -- <cmd...>
scripts/seat-lock.sh acquire <class> <owner> [--wait S] [--ttl-min N]
scripts/seat-lock.sh release  <ticket>           # or force-release (owner decision)
scripts/seat-lock.sh renew   <ticket>            # refresh the TTL heartbeat
```

- `with` acquires, runs the preflight **inside the lock**, runs the command,
  and releases on exit/interrupt (trap). It exports `LUMEN_SEAT_TICKET`,
  `LUMEN_SEAT_LOCKED=1` and `LUMEN_SEAT_CLASS` to the job, and prints
  `seat-lock: acquired class <c> owner <o> ticket <id>` to stderr — that line
  in a log is the proof the run was brokered; a gate asked to judge seat
  evidence looks for it.
- Classes, priority order: **bench > smoke > screenshot**. The user's
  interactive tty session outranks everything and is never brokered — it is
  what the preflight detects. Within a class, FIFO. A running job is never
  preempted; higher class only jumps the queue.
- Exit codes: 0 held/ok, 2 could not run (misuse, or preflight found the seat
  contaminated by an unbrokered job), 3 invalid seat state — a **stale**
  ticket (holder pid dead past its TTL) is reported with the exact
  `force-release` command and is **never auto-broken**; the owner decides,
  4 queue wait timed out.
- Tickets live in `$XDG_RUNTIME_DIR/lumen-seat/` — per-host by construction
  (the two `iritur` hosts have separate runtime dirs); each ticket records the
  detected DRM driver so reports name the host.

## Who holds a ticket

Already wired (they re-exec under the broker themselves — do not double-wrap;
nested calls see `LUMEN_SEAT_LOCKED=1` and proceed): `drag-smoke.sh`,
`app-smoke.sh`, `tty-smoke.sh`, `session-smoke.sh`, `scale-smoke.sh`,
`monitor-smoke.sh`, `input-smoke.sh`, `run-desktop.sh` (class smoke);
`ui-bench.sh`, `vkmark-bench.sh` (class bench).

Calls the broker by hand:

- **agent-screenshoter**: `scripts/seat-lock.sh with screenshot <owner> --
  bash -c '<grim/gdbus/probe commands>'` — screenshots coexist with the
  session being shot (preflight is skipped for the class) but must not
  interleave with a smoke or bench.
- **`~/lumen-compare/compare.sh`** (outside the repo): wrap it —
  `scripts/seat-lock.sh with bench compare -- ~/lumen-compare/compare.sh …`.
- Any ad-hoc compositor run, injection experiment or measurement: same shape.

Rules:

1. **Only the broker takes the seat.** A seat job started outside the broker
   is contamination for everyone else; its own output proves nothing at a gate.
2. **One ticket per job, held for the whole job.** The studio workflow never
   holds a run-wide ticket — each smoke/bench invocation gets its own, so a
   long run cannot sit on the seat between tasks.
3. **Long jobs renew.** TTL defaults to 25 min; a ticket whose holder is alive
   is never stale regardless of age — the TTL only bounds *dead* holders.
4. **Stale is an owner decision.** `status` names the dead holder and the
   force-release command; nothing reclaims it automatically.
5. The preflight triple (compositor via `/proc/*/comm` — never `pgrep -x
   lumen-compositor`, comm is 15-char truncated; foreign smoke via a
   pattern that excludes broker wrappers, ancestors and transient forks;
   `/proc/loadavg`) has exactly one implementation: here. Do not re-create it
   in scripts or workflows — call `status`/`preflight`.
