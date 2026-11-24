---
name: lumen-seatless-virtual-start-closeout
description: "Gate chronology + forensics for the seat-less start() task (2026-11-24, notebook, 83f99ac..6d8d5e8): the full ctest/smoke/bench-A-B numbers, the dead /proc/net/unix assertion's forensics and the live getpeername proof, the reviewer findings detail, and the honest gaps — companion to lumen-seatless-virtual-start, which carries the durable facts"
summary: "Closeout: gate numbers (142/142, smokes PASS, vulkan A/B rounds), dead-assert forensics + getpeername proof, review findings detail, honest gaps"
modules: lx.compositor, tests
topics: seat-less, bench, seat-broker, gate
metadata:
  node_type: memory
  type: project
---

Companion to [[lumen-seatless-virtual-start]] — the numbers and forensics that record
outgrew. Verified 2026-11-24 on the notebook, worktree
`/home/dimmus/lumen-wt/seatless-virtual-start`, base 87327dd, HEAD 6d8d5e8, gate READY.

- **ctest leg**: full tests-enabled `ctest --test-dir build/test --output-on-failure
  -LE bench` → 142/142 passed, exit 0, 79.63 s, run UN-TICKETED — itself proof the
  virtual-outputs fixtures no longer touch the seat. Counts computed from the tree:
  `ctest -N` = 147 registered (#147 = test_seatless_virtual_start), base 146 (+1
  add_test in the CMakeLists diff). Two independent stage attestations of the 142/142
  (reviewer 77 s, gatekeeper spot-check of #147 Passed 0.03 s + layer_graph green).
- **Smoke legs** (each under its own seat-lock ticket; binaries stamped 20:35:50 >
  HEAD commit 20:35:21): `LUMEN_SMOKE_SCALE=1 LUMEN_BUILD_DIR=<worktree>/build/test
  scripts/drag-smoke.sh --join-seat-session` → VERDICT: PASS, all rows PASS; compositor.log
  proves the full seat path undegraded — `session 1 vt=1 can_switch_vt=1 active=1
  drm=226:2 found=process type=tty`, 16 devices opened via TakeDevice, card 226:2
  (i915), `Vulkan device Intel(R) HD Graphics 5500 (BDW GT2) … Vulkan composite path
  active`, drag latency medians 9.4/11.3 ms inside the historic spread.
  app-smoke `--join-seat-session` → VERDICT: PASS with warnings B6×3 / G11×1 —
  pre-existing across ≥18 unrelated worktrees' app-smoke logs (the documented
  "GTK started no drag" flake family), no same-day base run (gap, below).
- **Bench leg**: `scripts/ui-bench.sh --backend vulkan`, interleaved A/B, base binary
  built at 87327dd (/tmp/lumen-ab-base, since removed) vs changed at 83f99ac, 3
  alternating rounds 19:56→20:06, each invocation under a distinct bench-class ticket
  (`seat-lock: acquired class bench` lines in /tmp/ab-logs base-r1..r3/changed-r1..r3).
  The first changed run FAILED on S2 (~+63..103% p50) — a §1.2 stop, adjudicated by
  the skill's interleaved discipline instead of accepted: base-r2 FAILs the same way
  (`FAIL S2 tick p50 35.538 -> 64.941 us +82.7%`) while changed-r3 in the same era
  reads S2 tick 29.888 vs base-r3 30.110 µs; the script itself refused to measure one
  slow-era run ("that is the host's mode, not the code's"); S3/S4 frame-path stages
  agree within -1.1%..+0.7%; changed-r3 is a full PASS (0 failures). The only gate
  warning (G5, S2/S4) is an open gate failing identically in run, baseline and both
  binaries (the driver's own 1072 B growth). The HEAD run itself (6d8d5e8, engine/none
  vs tracked baseline 7c3e3a1) passed all gates, worst stage delta +22% on a 0.013 µs
  stage, far under ABS_FLOOR_US=1.0. No regression attributable to the change.
- **Dead-assert forensics** (the first `holds_system_bus()`, dead code, fixed 80e0325):
  two independent defects. (1) The `/proc/net/unix` parse skipped 5 whitespace tokens
  (Num, RefCount, Protocol, Flags, Type) then read `inode` from token 6 — the St
  column — and `path` from token 7 — the Inode column (`head -4 /proc/net/unix`:
  `Num RefCount Protocol Flags Type St Inode Path`; awk NF=8 with t6=03 t7=1637827
  t8=/run/dbus/system_bus_socket for named rows); a python replica produced
  `named map sample: {3: '9906', 1: '6314'}` and `system-bus path present as a VALUE
  anywhere: False`. (2) Even with the parse fixed, a connected CLIENT socket's
  /proc/net/unix row carries no Path — only bound/listening sockets are named (live
  holder probe: the holding process's row is
  `00000000187bacea: 00000003 00000000 00000000 0001 03 1681444` — no Path) — and
  logind_session::open()'s sd-bus connection is a client, so the inode→path join
  cannot detect it at all. Consistently, the original red named only
  `CHECK(open_input_fd().empty()) failed`, never a system-bus check.
- **The replacement and its live proof** (80e0325): getpeername() on each
  `/proc/<pid>/fd/socket:[N]` fd, AF_UNIX-family-checked, sun_path clamped via a min()
  bound into a zero-filled +1 buffer (no over-read), early-break with closedir. Proved
  live rather than by assertion: `s.connect('/run/dbus/system_bus_socket');
  s.getpeername()` → `/run/dbus/system_bus_socket` — the kernel names the peer TO the
  client, which is precisely what the table join could not see — and a replica of the
  new algorithm flags the holding fd. The improved assert was NOT itself shown red on
  an unfixed tree (the unfixed tree takes the real session, needing a seat ticket);
  the demonstrated reds are the /dev/input-fd assert (LastTestsFailed.log 19:37) and
  the 19:31 loginctl probe — the mechanism proof stands in its place.
- **Review findings detail** (noise raised 3 / confirmed 3 / refuted 0 / preexisting 1):
  confirmed finding 1 = the dead assert above (medium, tests/test_seatless_virtual_start.cpp:90
  pre-fix); confirmed finding 2 = the seat-less predicate written out twice —
  `const bool seat_less = cfg.virtual_outputs && *cfg.virtual_outputs &&
  !cfg.session_mode;` in begin_input (:2462 pre-hoist) vs the want_virtual/seat_less
  pair in start() (:9607-9608 pre-hoist) — no runtime drift possible (same `cfg`
  member, begin_input's only caller start() :9762), fixed by the hoisted
  `seat_less_start()` (:68) in 6d8d5e8, `want_virtual` (:9618) correctly kept separate
  for the card-open decision. Preexisting = the compositor_impl.cpp whole-file
  clang-format debt (base and HEAD both 307 distinct violation lines; the change's
  added lines carry zero — TODO row on whole-file format debt extended 2026-11-24).
- **Honest gaps** (named, non-blocking): (1) broker-ticket proof for the two smokes
  and the HEAD ui-bench run is not in any artifact — the smoke scripts re-exec under
  the broker before their tee redirect (drag-smoke.sh:67-69), so run.log cannot carry
  the ticket line; provenance rests on structural self-brokering plus the broker
  preflight (the five measured A/B bench invocations DO carry distinct ticket lines).
  (2) The tester's own post-fix full-ctest log was overwritten by a later `ctest -N`
  (LastTest.log stub) — corroborated by CTestCostData.txt (post-HEAD) and the
  reviewer's independent run. (3) The decisive vulkan A/B ran at 83f99ac, not HEAD —
  the intervening commits (test-only assert fix; predicate hoist) are outside
  ui-bench's measured surface, and the smokes exercise HEAD's start path live.
  (4) Pre-existence of the five protocol tests' session takeovers was established by
  code reading + the analyst's live poller, not by a clean-base reproduction (the A/B
  base tree was cleaned; a repro needs a fresh ~15 min build). (5) The stored vulkan
  baseline was recorded on `7c3e3a1-dirty` (uncommitted content unknown) with same-day
  host drift — only the interleaved rounds decide.
- **Disproven / out of scope here:** the session's sleep-inhibitor risk from the card
  ("verify the guards hold when logind_ is absent") closed by reading
  lx.session.cppm:1006-1010 — `bus_fd()` returns -1 when `bus_` is null, and every
  listener/inhibitor/logind_source guard sits behind `bus_fd() >= 0`; no guard needed
  changing. A full `begin_input()` skip (the card's literal wording) was rejected — it
  would break the documented device-less-compositor-still-answers-wl_keyboard
  contract.

Related: [[lumen-seatless-virtual-start]] (the durable record),
[[lumen-smoke-harness-broker-fixture]] (broker contract),
[[lumen-ui-bench-noise-and-g7]] (bench noise discipline).
