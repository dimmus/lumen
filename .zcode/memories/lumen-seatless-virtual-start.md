---
name: lumen-seatless-virtual-start
description: "2026-11-24 notebook task (worktree seatless-virtual-start, base 87327dd, HEAD 6d8d5e8, 3 commits, gate READY, branch zcode/task/seatless-virtual-start): compositor::start() gained a seat-less mode for virtual-outputs instances — closes the invariant-gap-tests finding (d)/TODO row .zcode/TODO.md:98. Base ordering trap: logind_session::open() ran BEFORE want_virtual was computed. Fix: seat_less_start() predicate gates the session open and begin_input()'s input_manager::open, keymap still compiled; confirm probe tty→wayland→tty under ticket, red LastTestsFailed '147:test_seatless_virtual_start', green un-ticketed; ctest 142/142, both smokes PASS, vulkan A/B clean (first-run S2 2× FAIL = the host wandering mode); the first no-system-bus assert was dead code (/proc/net/unix client sockets carry no Path) → getpeername; noise 3/3/0/1 — gate chronology + forensics in lumen-seatless-virtual-start-closeout"
summary: "Seat-less start() for virtual outputs (83f99ac..6d8d5e8): no logind session, no input seat, keymap kept; red artifact-backed; ctest 142/142, smokes PASS, vulkan A/B clean; noise 3/3/0/1"
modules: lx.compositor, lx.session, tests
topics: seat-less, virtual-outputs, logind, libinput, hermetic-tests, seat-broker
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-24 on the **notebook** (iritur i915/nouveau, [[lumen-notebook-hw]]).
Task `seatless-virtual-start` (worktree `/home/dimmus/lumen-wt/seatless-virtual-start`,
branch `zcode/task/seatless-virtual-start`), base 87327dd, HEAD **6d8d5e8** (3 commits:
83f99ac the fix, 80e0325 the assert fix, 6d8d5e8 the predicate hoist; tree clean), gate
READY, not yet merged to main when written. Closes [[lumen-invariant-gap-tests]]
finding (d) (TODO row `.zcode/TODO.md:98`, amended by the landing stage).

- **The defect** (base 87327dd): `logind_session::open()` (compositor_impl.cpp:9592
  base) and `begin_input()` (:9744 base) were unconditional, and `want_virtual` was
  computed AFTER the session open (:9601) — the session was already taken by the time
  the code knew it did not want one. Every in-process compositor ctest on virtual
  outputs took the user's logind session and opened the real libinput seat. Confirmed
  under ticket ticket-20261124193129-758241-30480: one test_memory_pressure_cascade
  run flipped `/tmp/seatless-probe.log` `Type=tty` → `wayland` (19:31:30.210) → `tty`.
- **The fix** (83f99ac + 6d8d5e8): one `[[nodiscard]] seat_less_start(const config&)`
  helper (compositor_impl.cpp:68) = `cfg.virtual_outputs && *cfg.virtual_outputs &&
  !cfg.session_mode`; in `start()` the `want_virtual` decision is hoisted ABOVE the
  session open (:9618) and `seat_less` gates `logind_session::open()` (:9622-9625) —
  `want_virtual` stays separate for the card-open decision (:9631); in `begin_input()`
  the same predicate (:2472) skips `input_manager::open` (no logind round trips, no
  udev walk, no /dev/input node) but **still compiles the keymap** — clients get the
  layout at bind time and protocol tests bind keyboards (test_window_protocols.cpp:415
  et al), so the card's literal "skip begin_input()" was deliberately narrowed to
  "skip the seat/device opening". `logind_` stays default-constructed — identical
  state to a session-less host — so the listener/sleep-inhibitor/logind_source guards
  (behind `bus_fd() >= 0`, -1 when `bus_` is null, lx.session.cppm:1006-1010) hold
  unchanged. Derived from existing config; no new key/env (AGENTS.md §1.2);
  lx.compositor.cppm changed only its doc comment.
- **Regression test** (`tests/test_seatless_virtual_start.cpp`, ctest #147, TIMEOUT 90):
  an in-process virtual-outputs compositor asserting on its own process — no fd into
  /dev/input (:169), no socket fd whose peer is the D-Bus system bus (:173), seat
  keymap still valid (:178). Red pre-fix under ticket ticket-20261124193700-759442-20717
  (`LastTestsFailed.log` = `147:test_seatless_virtual_start`, 19:37, pre-commit;
  `CHECK(open_input_fd().empty()) failed`, `seat open took 98 ms … 16 device opens`);
  green post-fix with **no ticket** — that is the hermeticity point. Reproduced this
  ask: build current at HEAD, `ctest -R test_seatless_virtual_start` → Passed 0.03 s,
  `loginctl show-session 1 -p Type` = tty before AND after. Gives
  test_memory_pressure_cascade a hermetic fixture too.
- **Gate, condensed** (full numbers in [[lumen-seatless-virtual-start-closeout]]):
  ctest 142/142 (147 registered / base 146, +1) run un-ticketed; both smokes
  `--join-seat-session` VERDICT: PASS, compositor.log proving the full seat path
  undegraded (`session 1 … found=process`, Vulkan BDW GT2, medians 9.4/11.3 ms); the
  vulkan A/B (interleaved base 87327dd vs changed 83f99ac ×3, six distinct bench
  tickets, /tmp/ab-logs) is clean — the first changed run's S2 ~2× FAIL follows the
  host's wandering mode ([[lumen-ui-bench-noise-and-g7]]; base-r2 FAILs +82.9% in the
  same era changed-r3 reads 31.0 vs base 31.2 µs), not the binary.
- **Fuckup — the dead assertion** (fixed 80e0325): the first no-system-bus assert
  joined `/proc/net/unix` inode→path and could NEVER fire — the parse read the St
  column as inode and the Inode column as path, and a connected CLIENT socket's row
  carries no Path at all (only bound/listening sockets are named) while
  logind_session::open()'s sd-bus connection is a client: an absent test passing as
  coverage (the red named only `open_input_fd`). The replacement asks the kernel —
  `getpeername()` on each `socket:[N]` fd names the peer TO the client (forensics +
  live proof in the closeout). Bound: the substring assumes the standard bus address
  (neither host sets it otherwise); the /dev/input + keymap asserts still guard.
- **Review noise (false-positive-rate trend):** raised 3, confirmed 3, refuted 0,
  preexisting 1. Confirmed: (1) the dead assert (medium, fixed 80e0325); (2) the
  seat-less predicate written twice (low, fixed 6d8d5e8 by the hoisted helper — truth
  table unchanged, begin_input's only caller is start()). Preexisting (TODO row
  extended): compositor_impl.cpp whole-file clang-format debt — measured this ask:
  307 distinct violation lines at base AND HEAD alike, ∩ added lines = ∅; diff-hygiene
  exit 1 there is the debt, not this change (whole-file reformat rejected).
- **Host findings.** (a) The smoke scripts re-exec under the seat broker BEFORE their
  own tee redirect (drag-smoke.sh:67-69), so a smoke run.log can NEVER contain the
  `seat-lock: acquired` line and the broker keeps no journal — smoke ticket provenance
  rests on the scripts' unconditional self-brokering plus the preflight; bench
  invocations DO log tickets (/tmp/ab-logs, six distinct). Less harsh than
  [[lumen-event-loop-slot-leak]]'s "INVALID as brokered evidence" note: smokes
  self-broker by construction. (b) **Five non-virtual protocol tests still take the
  user's logind session** on a free-display host (test_protocol_bind,
  test_shell_bridge, test_protocol_conformance, test_window_protocols,
  test_pointer_protocols — the gate ctest's live poller saw six wayland episodes,
  0.1–1.3 s each, Type back to tty): pre-existing base behavior, out of this diff;
  TODO row added. A full `ctest -LE bench` on a free-display host is still not
  session-safe. (c) The wandering ~2× S2 GPU bench mode can FAIL a clean binary's
  first round — adjudicate interleaved, never accept the first FAIL.
- **Honest gaps:** the vulkan A/B was measured at 83f99ac, not HEAD (the intervening
  commits are test-only/predicate-hoist, outside ui-bench's surface; the smokes
  exercise HEAD's start path live); the getpeername assert was never itself shown red
  on an unfixed tree (the demonstrated reds are the /dev/input-fd assert and the 19:31
  loginctl probe; the live mechanism proof stands in its place); smoke ticket
  provenance is structural, not in-artifact (finding (a)).

**Why:** a test compositor that takes the real seat is a defect regardless of the tests
passing; the fix's shape — derive the mode from existing config, hoist the decision
above the FIRST side effect, keep the parts protocol contracts need (keymap) — is the
template for any "skip the host touch" mode. And an assertion that cannot fail is not
coverage: it manufactured green on exactly the behavior the test existed to catch.

**How to apply:** new in-process compositor tests configure virtual_outputs and need no
broker ticket; five protocol tests still ride the real-display path (TODO row) — do not
assume the whole suite is seat-safe yet. Touch the start path only through
`seat_less_start()` — never re-inline the predicate; `session_mode` always keeps the
full seat path. To assert "no host touch" live, inspect the started process's fds with
getpeername()/fdinfo — /proc/net/unix cannot see client sockets (only bound/listening
rows carry a Path).

Rule proposal: second recorded strike of the assertion-that-cannot-fail class
([[lumen-invariant-gap-tests]]' tautological `sizeof(rest)/sizeof(rest[0]) == 4` report
assert; now the dead /proc/net/unix join). Mechanical check, a reviewer checklist item:
every assertion added in a diff must be named in the pre-fix red artifact
(LastTestsFailed/CHECK line), not merely some red in the same test file; and any
negative "assert-nothing" assertion must be validated against a live mechanism probe —
can it EVER fire on the defect it claims to catch? — before it counts as coverage.

Related: [[lumen-invariant-gap-tests]] (finding (d), the discovery), [[lumen-seatless-virtual-start-closeout]]
(gate chronology + forensics), [[lumen-notebook-hw]] (host),
[[lumen-ui-bench-noise-and-g7]] (the wandering bench mode),
[[lumen-smoke-harness-broker-fixture]] (the seat-broker contract),
[[lumen-event-loop-slot-leak]] (sibling 2026-11-24 task; stricter smoke-ticket note).
