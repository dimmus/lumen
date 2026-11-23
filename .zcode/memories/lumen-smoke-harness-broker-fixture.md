---
name: lumen-smoke-harness-broker-fixture
description: "2026-11-23 notebook, task smoke-harness-hermetic-broker (3f317c8..f936cda, gate READY, landed on main via zcode/memory-spool ff): smoke_harness's perfetto fixture stages every script its ui-bench.sh copy references and writes its own seat-lock.sh wrapper — refuses any XDG_RUNTIME_DIR but the fixture's private one (exit 2), records argv, execs the tree's real broker with --no-preflight ahead of --; prun env -u's the seat vars so the re-exec at ui-bench.sh:106-109 runs; 20 sub-checks (suite 62→82); baseline 49 ok/13 FAIL exit-127 (deterministic since ba80f18) → 82 ok/0 FAIL PASS 1.98 s, ctest -LE bench 137/137; mutations flip it; traps: a mutation took a real smoke ticket in /run/user/1000/lumen-seat, known_red=[smoke_harness] hid the red from every gate; noise 6/6/0 + 2 preexisting"
summary: "smoke_harness perfetto fixture carries the seat broker hermetically (wrapper→real broker --no-preflight, private lock dir); 49/13→82/0; mutations flip it; review noise 6/6/0, 2 preexisting"
modules: scripts, harness
topics: smoke, seat-broker, hermeticity
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-23 on the **notebook** (i915+nouveau); worktree
`/home/dimmus/lumen-wt/smoke-harness-hermetic-broker`, branch
`zcode/task/smoke-harness-hermetic-broker`, 4 commits over base 0796540: 3f317c8 (fixture
+ 20 checks + TODO tick), review fixes 9579cdc, bc33b6a, f936cda — tests only. Landed on
main 2026-11-23 by `git merge --ff-only zcode/memory-spool` (tip f836bc6 = the
[[lumen-harness-evals-green]] correction riding the task tip; ffable only once landed).

- **The defect (closed):** the perfetto fixture staged ui-bench.sh + bench-warmup.sh
  only; ui-bench.sh re-execs `$ROOT/scripts/seat-lock.sh` when LUMEN_SEAT_LOCKED is
  unset (ui-bench.sh:106-109) since broker merge ba80f18 (2026-11-19) — all 13 perfetto
  rows FAIL, traced passes exit 127 (`seat-lock.sh: No such file`); deterministic
  49 ok / 13 FAIL on unmodified 0796540 (/tmp/*_smoke_harness_baseline.log).
- **Design — why not the TODO row's literal "stage seat-lock.sh too":** a plain copy
  runs the real preflight (seat-lock.sh:85-127, /proc for compositors/smokes; failure
  path :327-333 exit 2) — red whenever anything brokered runs on the host, and with
  XDG_RUNTIME_DIR unset it takes tickets in the host's REAL lock dir (RUN_BASE, :41).
  The fixture's `seat-lock.sh` is a wrapper instead: refuses any XDG_RUNTIME_DIR but
  the fixture's private 0700 `<fixture>/runtime` (exit 2, named message), records its
  argv, execs the tree's real broker with `--no-preflight` (:309) ahead of the `--`;
  prun runs it with LUMEN_SEAT_LOCKED/TICKET/CLASS removed (`env -u`) and
  XDG_RUNTIME_DIR inside the fixture root — the re-exec is exercised and an outer
  brokered env changes nothing (one pass proves it: foreign ticket/class in).
- **The 20 sub-checks** (--perfetto section 14 → 34 rows, suite 62 → 82): mechanism
  guard — every `$ROOT/scripts/<name>` the copied ui-bench.sh references is staged
  (bench-warmup.sh, inject-devices.sh, inject-input.py, seat-lock.sh; a miss is named),
  the three verbatim copies byte-identical to the tree's; broker contract per pass —
  argv `with bench ui-bench.sh -- …`, all 4 scenarios LUMEN_SEAT_LOCKED=1 /
  LUMEN_SEAT_CLASS=bench / the ticket held in the private dir (the stand-in bench logs
  locked/class/held/ticket; `with` exports at seat-lock.sh:321), one ticket per pass,
  the broker's proof line `seat-lock: acquired class bench owner ui-bench.sh`
  (seat-lock.sh:218), `held=absent queued=0` after every pass; preflight-off row; a
  direct job through the wrapper→real broker (held while running, released after); the
  two XDG refusals, a refused call reaching no broker.
- **Evidence:** after the fix `ctest --test-dir build/test -R smoke_harness -V` →
  `smoke_harness: PASS`, exit 0, 82 ok / 0 FAIL, 13/13 former FAIL rows ok, 1.98 s
  against TIMEOUT 60 (CMakeLists.txt:1259) — reviewer runs (/tmp/review2-direct.log et
  al) plus the gatekeeper's targeted run (2.06 s; real lock dir mtimes identical
  before/after). Gate at f936cda: tests-enabled build + full
  `ctest --test-dir build/test --output-on-failure -LE bench` = 137/137 passed
  (79.06 s / 79.22 s); `scripts/diff-hygiene.sh 0796540` clean; classify-change →
  classes [logic], ctest only.
- **Mutations flip the harness** (line-filtered stream copies of the committed file
  from /tmp scratch, scripts/ symlinked — nothing to revert; all ran against the
  committed file's sha 3d63a8a203b3c9c1): remove the wrapper → 53 ok / 29 FAIL; plain
  broker without --no-preflight → 74/8; LUMEN_SEAT_LOCKED=1 into prun → 74/8 (in
  3f317c8's message). Reviewer re-runs at HEAD: wrapper removed 55/27 (the two
  unset-XDG rows moved to the twin), locked-in-prun 74/8, no --no-preflight 80/2,
  no `-u` 80/2, stand-in-only wrapper 75/7, control 82/0.
- **Review** (noise 6 raised / 6 confirmed / 0 refuted / 2 preexisting) — fixes
  9579cdc (argv log appends; `>` truncated per call), bc33b6a (the row "runs under the
  tree's real broker" asserted only exit 0 — green with no broker; renamed, the next
  row's held-ticket check is the proof), f936cda (the unset-XDG probe re-homed onto a
  twin wrapper behind a stand-in broker — with the REAL broker behind it a broken
  guard sent the probe into the host's lock dir: proven under
  `unshare -U --map-current-user --keep-caps -m` + tmpfs mask, real `queue/`+`seat.lock`
  created, a real smoke ticket for `true`). The 4th confirmed finding — the dismissal
  guidance in [[lumen-harness-evals-green]] inverting once this lands — is fixed by
  spool commit f836bc6 ("How to apply" superseded in place, dated).
- **Fuckup — mutation (b) touched the real lock dir** (disclosed): the card's mutation
  run as a plain-copy broker sent the unset-XDG probe through the real broker against
  /run/user/1000/lumen-seat — a real smoke ticket for `true` taken and released within
  the second (seat free before/after, only mtimes moved; the committed harness leaves
  the dir byte-identical). Second strike of the class — see Rule proposal.
- **The known_red trap (owner sign-off item):** the card's known_red=[smoke_harness]
  filtered the ONE test under repair out of every workflow gate (studio gates at
  lumen-development-studio.dwf.ts:529, :672, :793 and the landing re-gate :1172 all
  drop known-red names — verified in the script); the pass rests on the reviewer's and
  gatekeeper's unfiltered runs only. Card-scoped; do not carry it forward.
- **Host findings:** bash 5.3.20, coreutils 9.12 — the harness needs GNU `env -u` and
  bash ≥ 4.4 empty-array semantics under set -u (present here; not exercised on the
  vmware VM); shellcheck not installed. **ui_bench_e2e_dry_run is not hermetic
  (pre-existing, TODO row added):** CMakeLists.txt:1250-1253 (TIMEOUT 30) runs
  `scripts/ui-bench.sh --e2e --dry-run --no-build`, which re-execs the real broker —
  real bench ticket in the real lock dir + real /proc preflight (exit 2 when a
  compositor or smoke runs; TIMEOUT 30 expires if a seat job holds — the broker waits
  DEFAULT_WAIT 900, seat-lock.sh:45): the `-LE bench` suite's green depends on host
  state, the dependence this task removed from smoke_harness. Also TODO'd (low): the
  registration comment CMakeLists.txt:1255-1257 still describes a three-leg harness.
  Mechanism-guard residual: it scans the copied ui-bench.sh only — a new
  `$ROOT/scripts/` reference inside bench-warmup.sh (which it sources) would be
  neither staged nor named.
- **Disproven / closed:** "stage seat-lock.sh too" — wrong shape, see Design. "Just set
  LUMEN_SEAT_LOCKED=1 in prun" — skips the re-exec, contract checks fail (74/8); a
  stand-in that sidesteps `with` is not acceptable. The ba80f18 red signature: closed
  2026-11-23 — a perfetto red on any tree carrying 3f317c8 is the change under test or
  host noise, not to be dismissed ([[lumen-harness-evals-green]] updated by f836bc6).

**Why:** a hermetic fixture must carry every script its copy reaches for and refuse the
host's real seat state — a fixture that execs the real broker unguarded is
host-dependent by exactly the mechanism it copied the script to avoid; and a tolerated
known-red named after the test a task fixes blinds every gate of that run.

**How to apply:** when a fixture stages a tree script that references other
`$ROOT/scripts/` files, stage them verbatim (byte-compare) and give host-touching ones
a wrapper that pins the fixture-private env and refuses anything else; `env -u` the
seat vars in the runner so the re-exec path is what gets exercised; never mutate
seat-touching harness code in place. Triage: a perfetto-row red after 2026-11-23 is the
change under test or host noise — the ba80f18 signature (13 FAILs, exit 127 at
ui-bench.sh:107) identifies only trees older than 3f317c8.

Rule proposal: any mutation or probe that can fall through to the real seat broker runs
inside `unshare -U --map-current-user --keep-caps -m` with a tmpfs over
`/run/user/$(id -u)/lumen-seat` — the mechanical check that catches both strikes of the
real-lock-dir class in this task (mutation (b)'s ticket, the probe hazard) — and the
fixture mechanism guard must scan sourced scripts too, not only the copied entry
script.

Related: [[lumen-harness-evals-green]] (the gap this closes + the spool correction),
[[lumen-smoke-bench-harness-green]] (the harness's original hermetic checks).
