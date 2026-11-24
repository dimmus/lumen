---
name: lumen-event-loop-slot-leak
description: "2026-11-24 notebook task (worktree event-loop-slot-leak, base 87327dd = main tip, HEAD fe97caf, 1 commit, +144 lines over exactly 3 files, gate READY): ~event_loop() leaked the process-wide event-source table's slots — k_max_event_sources=32, the destructor only unregistered the strand and closed fds — so every destroyed loop's sources stayed, later dispatch walked them, and the table filled across loop lifetimes until add_source silently dropped, which is the third compositor::start()-in-one-process failure (compositor_impl.cpp:10272-10275 'no event source slot for theme completion'). Fix: an owners[] array beside sources[] — add_source writes it (:536), remove_source swaps it in lockstep (:553), and the destructor reclaims exactly its own slots by owner match, never dereferencing a possibly-dangling source, before the descriptors close so no EPOLL_CTL_DEL is wanted there. Confirm-first artifact-backed: pre-fix red 4/5 (LastTestsFailed.log '61:test_event_loop', 17:30:54 < commit 17:42:52), post-fix 5/5, tests-enabled ctest 142/142; the two join-seat smokes passed but keep no ticket line → unbrokered extras only; review noise 3/3/0/3"
metadata:
  node_type: memory
  type: project
summary: "Slot leak fixed (fe97caf): ~event_loop() reclaims own source-table slots via owners[]; pre-fix red 4/5 artifact-backed; ctest 142/142; closes the 3rd-start() bug; noise 3/3/0/3"
modules: lx.runtime, lx.compositor, tests
topics: event-loop, leak, destructor, source-table, compositor-restart

---

Verified 2026-11-24 on the **notebook** (iritur, [[lumen-notebook-hw]]). Task
`event-loop-slot-leak`, worktree `/home/dimmus/lumen-wt/event-loop-slot-leak`, branch
`zcode/task/event-loop-slot-leak`, base 87327dd (= main tip when written), HEAD **fe97caf**
(one commit, tree clean, +144 lines over exactly the card's 3 files), gate READY. Closes
the defect [[lumen-invariant-gap-tests]] recorded as finding (e) (TODO row
`.zcode/TODO.md:60-70`; the landing stage ticks it — the card kept TODO.md out of scope).

- **The defect** (base 87327dd): the source table is process-wide and fixed
  (`k_max_event_sources = 32`, lx.runtime.cppm:287; `loop_state()` static local) and
  outlives any one loop — the compositor builds one per `start()`. `~event_loop()` (base
  :333-341) unregistered the strand and closed fds only, so a destroyed loop's
  `event_source*` stayed: later dispatch walked dead slots and a later `add_source`
  silently dropped once the fixed table filled (base :512-516) — a third
  `compositor::start()` in one process fails with `io_err::broken_pipe` "no event source
  slot for theme completion" (compositor_impl.cpp:10272-10275).
- **Confirm-first, artifact-backed**: the new test failed 4/5 on the unmodified module —
  `Testing/Temporary/LastTestsFailed.log` holds `61:test_event_loop`, mtime 17:30:54,
  before the fix commit 17:42:52 (`ctest -N` confirms #61 is test_event_loop); the
  commit message records the failing command and output (CHECK
  `a_destroyed_loop_returns_its_source_table_slots`, tests/test_event_loop.cpp:46). The
  reviewer reproduced independently by relinking the base module (`git show 87327dd:…`
  with the build's flags, LD_LIBRARY_PATH ldd-confirmed): `5 tests, 4 failed`, exit 1,
  at :41/:57/:83/:101; the passing fifth is the remove_source primitive guard — the
  live-removal path was never broken, only the destructor.
- **The fix** (fe97caf): each slot records its owning loop — `owners[]` beside
  `sources[]` (lx.runtime.cppm:294); `add_source` writes it (:536), `remove_source`
  swaps it with the pointer in lockstep (:553) — and `~event_loop()` reclaims exactly
  its own slots matched by owner (:347-354), **never dereferencing the source** (sources
  declared after their loop die first — test 2 builds that order), **before the
  descriptors close** (:355): the close auto-deregisters from epoll, so skipping
  `EPOLL_CTL_DEL` there is correct. `remove_source` keeps its semantics; no new surface
  on `event_source`; `event_loop` is non-copyable/non-movable (:118-119 + the declared
  destructor), so the `this` owner key is stable.
- **Regression test** (`tests/test_event_loop.cpp`, ctest `test_event_loop`,
  CMakeLists.txt:1131-1139), 5 cases: remove_source frees a slot while the loop lives
  (passed pre-fix); a destroyed loop returns its slots (the card's loop-A/loop-B
  acceptance); a dying loop spares a live loop's sources; capacity exhaustion across 8
  loop lifetimes with a fresh loop's add landing (failed pre-fix — the silent drop); an
  fd_source over an eventfd outliving its loop, joining a later one without a duplicate
  slot (failed pre-fix). Deliberate (header comment): no case runs a loop — the
  bookkeeping is the behavior under test.
- **Gate**: full tests-enabled suite green — `cmake --build build/test -j2` + `ctest
  --test-dir build/test --output-on-failure -LE bench` = 142/142 (implementer 81.55 s,
  reviewer 79.78 s — two independent attestations). Counts computed, not quoted: 147
  registered / 142 with `-LE bench`, base 146 (+1 registration). classify-change
  (build+logic) derived only the ctest; render_path false.
- **Honest residuals**: no surviving stdout capture of the green summary line (green
  stands on the 142 post-fix stamps at 17:59-18:00, CTestCostData's test_event_loop 5×
  vs 4× runs, LastTestsFailed.log untouched since the 17:30 red, plus the attestations);
  the 146-test baseline green is attested only; the suite ran unsanitized — the
  no-stale-dereference claim rests on the owner-matching design, not ASan; the
  motivating in-process compositor restart past compositor_impl.cpp:10271 is untested
  end-to-end (app-smoke's E5 restarts the shell, not the compositor).
- **The two join-seat smokes** (drag-smoke 18:05:18 scenarios a-j PASS exit 0;
  app-smoke 18:14:56 178 PASS / 0 FAIL — i915 card2 / eDP-1 / hasvk, uinput
  event16/event3) ran the fixed build/test compositor, but no
  `seat-lock: acquired ... ticket` line survives anywhere (only `$XDG_RUNTIME_DIR/`
  lumen-seat mtimes match each completion) — INVALID as brokered evidence per the seat
  contract, unbrokered extras; nothing gates on them.
- **Decisions**: (1) owner-pointer bookkeeping over the card's alternative (an owner
  hook on `event_source`) — zero exported-surface change; (2) reclaim before fd close,
  no `EPOLL_CTL_DEL` on that path; (3) owner match only, never a source dereference;
  (4) TODO.md untouched by the task (outside the card's scope list).
- **Mimosa**: the pre-commit hook could not complete a scan (`scanner_enobufs`;
  compatibility policy let the commit through) and declined the reviewer's first
  base-repro approach (writing the base module into the worktree) — replaced by /tmp
  object substitution. The gatekeeper closed the audit with a real deep scan
  (scan-2026-11-24T13-57-56, seal sha256:7e2bf42b…): 3 findings, all pre-existing
  CWE-22 statics in scripts/flex-fixtures.py:152 and scripts/gen-memory-index.py:128/158
  — zero in or caused by the diff.
- **Host/toolchain (notebook)**: clang-format 23.1.5 reports 45 violations on
  lx.runtime.cppm, all pre-existing at base (lines 238-762, disjoint from the added
  ranges 291-294/340-355/536/553 — zero new violations; older-clang-format drift), so
  diff-hygiene's format leg fails here exactly as at HEAD and its `line 92: local` error
  is the known script defect (TODO row). Disclosure: the analyst's two `ctest -N` runs
  rewrote the empty-content LastTest.log markers — proven contentless, nothing lost.

**Review noise (false-positive-rate trend):** raised 3, confirmed 3, refuted 0,
preexisting 3 — nothing introduced by this diff, none blocking. The three preexisting,
all TODO rows now: add_source's silent full-table no-op (lx.runtime.cppm:534-535);
loop_state() unsynchronized under the single-live-UI-loop assumption (:298-301); and
remove_source matching by pointer only, ignoring owners[] (:546-557).

**Why:** a fixed-capacity process-wide registry handed out by a per-instance object must
be returned by that object's death — the leak was invisible until a third `start()` in
one process, and the symptom it produced (a silent add drop) pointed everywhere but at
the destructor. Keying reclamation on the owner pointer, never on the possibly-dangling
pointee, is what makes destruction safe when sources can outlive their loop.

**How to apply:** touch the source table only through the three invariants —
`add_source` writes `owners[]`, `remove_source` swaps both arrays in lockstep,
`~event_loop()` reclaims only `owners[i] == this` before closing fds; never remove
another loop's source by pointer. Any new per-instance registration in a process-wide
table gets the same owner column + destructor reclaim, with a create/destroy-then-readd
test like tests/test_event_loop.cpp:66-91 (the capacity-exhaustion case bites). To
reproduce a pre-fix red, substitute the base object in /tmp and relink (the Mimosa hook
blocks writing base source files into a worktree).

Rule proposal: third recorded strike of the silent-overflow-cap class, all surfaced
2026-11-24 (text_layout's line cap ignores reject; container_node::add never warns; now
add_source silently drops on a full table). Mechanical check: a reviewer checklist item
— grep any new fixed-capacity structure or registry for a silent `return` at its cap; a
hit demands an overflow_action parameter or a TODO row naming the departure, and the
structure joins test_overflow_caps' sweep (driven from the module tree, not a literal
inventory).

Related: [[lumen-invariant-gap-tests]] (finding (e), the discovery), [[lumen-notebook-hw]]
(host), [[lumen-unit-pair-green]] (suite-count discipline).
