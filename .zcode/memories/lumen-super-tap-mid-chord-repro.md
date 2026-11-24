---
name: lumen-super-tap-mid-chord-repro
description: "2026-11-24 notebook seat repro of the 'Super tap grab fires mid-chord' TODO row: 45 injected Super+Shift+y chords (20 ms + human-like cadences) under a µs CLOCK_MONOTONIC timeline, one class-smoke ticket — 0 launcher fires; lone-Super control 6/6, each 161–182 µs after the release of a Super pressed alone; no grab fire at all (the configured \"Super\" = \"shell.launcher\" tap binding shadows the shell grab); the 2026-11-17 sighting explained by two by-design mechanisms (the session's own lone-meta tap + the just-saved Super+Shift+y chord binding matching later chords at match_binding); no product change, TODO row unticked."
summary: "Super tap mid-chord NOT_REPRODUCED (notebook 2026-11-24): 45 chords, 0 fires, control 6/6 at 161-182 us; 2026-11-17 sighting = lone-meta tap + saved chord binding; no product change; noise 2/2/0/0."
modules: lx.compositor, scripts
topics: keybindings, tap, injection, seat, repro
metadata:
  node_type: memory
  type: project

---

Verified 2026-11-24 on the notebook `iritur` (i915+nouveau), quiet serialized seat session
(worktree `zcode/task/super-tap-mid-chord` @ base a89cbad; the only commit is the TODO-row
amendment `4e51791`, tests-enabled build): the TODO row "Super tap grab fires mid-chord
(compositor tap-arming corner)" did **NOT** reproduce — verdict NOT_REPRODUCED, no product
change. This is the follow-up to [[lumen-settings-key-capture-seat]]'s trap bullet; the
row is amended in place and left unticked (owner call whether to close as misattributed).

- **Run discipline** (each verified this session): one class-smoke broker ticket for the
  whole job — `run/job.err:1` = `seat-lock: acquired class smoke owner
  super-tap-mid-chord ticket ticket-20261124072438-467078-9737`; seat-status
  before/after both quiet; `LUMEN_SMOKE_SCALE=1`; owner config replaced by the deploy
  config for the run and restored byte-identical (sha256s in `run/`).
- **Injection facts:** keys go through the internal AT node `/dev/input/event3` via
  `scripts/inject-input.py`; the running "Lumen Inject Mouse" (event16) is buttons-only,
  no key codes. `scripts/inject-input.py watch` watched every node: 282 kernel frames,
  all on event3, each 26–54 µs behind its injector record, 0 SYN_DROPPED, nothing on any
  other node — no foreign input could have manufactured the negative.
- **µs timeline:** compositor and shell stderr piped through `bin/ts.py`, stamping every
  line in CLOCK_MONOTONIC µs — the injector's clock and the kernel's; the full
  injection→kernel→action chain measured <200 µs.
- **Numbers** (re-counted from raw artifacts, not quoted): `run/summary.json` — 51
  chords: A=20 at the original 20 ms member spacing, B=25 human-like (80–200 ms gaps,
  100–300 ms holds; both roll orders, both release orders, plus B5 meta-first release
  with y still down), C=6 lone Super taps; 6 opens, all `kind=tap-binding`, all inside
  control windows; `delivery_gaps=[]`, `dropped=0`. `run/compositor-ts.log` — exactly 6
  `action shell.launcher` lines, 0 `grab activated`, 0 real ERROR/WARN. Control latency
  161–182 µs from the Super *release of a Super pressed alone* to the action line — the
  only shape that ever fires, matching `observe_tap` (fires on release of the armed key,
  `keybindings.cppm:454-458`) and its unit tests (`tests/test_keybindings.cpp:222-256`,
  `:358-375`).
- **Why no grab ever fires:** the configured `"Super" = "shell.launcher"` tap binding
  (`deploy/lumen/config.toml:165`) shadows the shell's tap grab in `observe_tap`
  (`keybindings.cppm:426-441`, configured taps arm first) — `grab activated` cannot
  appear in this configuration, and the 2026-11-17 session had the same binding
  (`config-before-save.toml:164`), so the tap grab was never the live path there either.
- **The 2026-11-17 sighting, explained** (misattribution, not a defect): (1) the
  session's own lone-meta tap (`inj-s2.jsonl:137-138`, t11-meta) opens the launcher by
  design; (2) the just-saved `"Super+Shift+y" = "shell.launcher"` chord binding matched
  every later chord at `match_binding` (`compositor_impl.cpp:1788`), logging the
  identical action line a tap fire would. The "~20 ms after the meta press" claim has no
  timestamped artifact. The key path is unchanged since that tree:
  `git log 99e9950..a89cbad -- keybindings.cppm compositor_impl.cpp lx.shell.cppm`
  returns only the logging refactors 96e336a/8e925e5 — it was not silently fixed.
- **Disproven, do not re-litigate:** event coalescing/reordering of press pairs (the
  row's own suspicion) — the kernel saw every press, in order, 0 delivery gaps; "the
  compositor leaves stale client modifier state" — the refocus keymap re-send is by
  design (see [[lumen-settings-key-capture-seat]]).
- **Fuckups (two aborted tickets before the evidence run):** ticket-…070441 died
  pre-seat on `KEYBOARD_DEV: unbound variable` — the var was set inside a pipeline, i.e.
  a subshell; pipeline assignments do not survive the pipe, so set them before it.
  ticket-…070502 ran the session but hung at teardown on an inherited pipe fd (a
  collector kept the read end open; unblocked by killing the lone `tee`), then aborted
  on a wrong control-close grep — the layer-unmap line carries no `namespace=` field
  (only the map line does), so grep the line shape the code actually emits. Neither
  attempt produced cadence evidence; the evidence job ran under its own ticket. The
  run-prep-staged file-following stampers (`tools/stamp-log.py`, `latency-test.py`)
  were never needed — piping through `bin/ts.py` is simpler and bounded the whole chain.
- **`ctest -N` is not read-only:** enumeration overwrites
  `build/test/Testing/Temporary/LastTest.log`, destroying the previous run's pass/fail
  artifact (both the gate analyst and this session's gatekeeper did it). To inspect the
  suite without losing evidence: copy `LastTest.log` first, or read `CTestCostData.txt`
  (records runs, not outcomes).
- **Outcome and bookkeeping:** NOT_REPRODUCED branch of the card followed exactly — no
  product change, no new test (the unit tests above already pin the disarm), TODO row
  amended in place (`4e51791`, only `.zcode/TODO.md`, +28/−7) and left unticked.
  Baseline ctest 141/141 `-LE bench` (supplementary; nothing was required for a
  docs-only diff). The supplementary drag/app smokes' `seat-lock: acquired` broker lines
  are not preserved in any artifact — per the seat contract they count as not
  evidenced, and nothing required them. Evidence mirrored to
  `/home/dimmus/lumen/evidence/2026-11-24-super-tap-mid-chord/` (`diff -r` identical,
  re-verified this session).
- **Review noise (cross-run false-positive tracking): raised 2 / confirmed 2 / refuted
  0 / preexisting 0.** Both low, both fixed by evidence-doc edits: (1) the control ran
  6 taps, not 20 — the card's "≥20 chords each" binds the two chord cadences; C is a
  liveness control by the card's own definition (reading documented at
  `REPRO.md:56-59`); (2) the first aborted ticket (…070441) has no archived artifact —
  its failure mode stands on the run session's own report (`VERDICT.md:100-110`).

**Why:** a NOT_REPRODUCED with a µs timeline is worth more than a fix would have been:
it converts a week-old "fires mid-chord" row into "two by-design log-line lookalikes",
and it shows untimestamped logs are the difference between an hour of diagnosis and a
week of a phantom defect.

**How to apply:** before attributing any seat event in time, pipe the compositor/shell
logs through `bin/ts.py` (injector's clock) and keep `inject-input.py watch` running —
delivery gaps, reordering and foreign input then have no window to hide in. Never read
an `action shell.launcher` line as "the tap grab fired": check which path emitted it
(configured tap binding vs grab vs `match_binding` on a saved chord binding,
`compositor_impl.cpp:1788/:1833-1837`) and whether a lone-meta tap ran in the session.
Repro procedure, cadences and artifacts: `evidence/2026-11-24-super-tap-mid-chord/`
(REPRO.md, VERDICT.md, run/TIMELINE.md).

Rule proposal: (second recorded occurrence of the untimestamped-evidence trap —
2026-11-17 the misattribution itself, 2026-11-24 the original logs could not place the
fire in time) any seat repro/diagnose procedure that attributes events in time MUST
stamp all process logs on the injector's clock and grep control lines by their exact
emitted shape; add these as checklist items to the seat/repro skill.

Cross-refs: [[lumen-settings-key-capture-seat]] (the original trap),
[[lumen-host-build-run-env]] (hosts, injection), [[lumen-uinput-key-bitmap-format]],
[[lumen-smoke-harness-broker-fixture]] (broker discipline).
