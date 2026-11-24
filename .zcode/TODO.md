# Lumen open work

Extracted 2026-11-02 from the archived agent memories (`.zcode/memories/`), the ADRs' own
status sections, and the working tree. The ADRs are authoritative for detail; keep this list
current when an item closes.

## Now

- [ ] **Push/merge policy flipped — the pre-2026-11-19 "never pushed without asking"
  notes elsewhere in this file are historical** (owner standing rule, 2026-11-19): all
  positive work (gate-READY) now ends committed, merged into main by the run
  (topological order, merged tree re-gated; ff-only on the light path) and pushed to
  origin over SSH. Below-READY outcomes still go to the owner. `gh` is not installed
  and HTTPS has no credential helper, so the PR of record is the PR-text artifact +
  the merge commit — owner may install `gh` if real PR objects are wanted (then the
  workflows gain a PR step).
- [ ] **A spool commit parented on a task tip lands that tip by fast-forward — a bypassed
  landing** (found 2026-11-24 with run dwfrun-e4cd1b91; high; source:
  `.zcode/memories/lumen-todo-fixes-round.md`): the memory-writer spool recipe falls back
  to `git rev-parse HEAD` for its parent (lumen-memory-writer.md:31), so f836bc6 was
  spooled on the unlanded tip f936cda; the next memory-writer's contract step
  `git merge --ff-only zcode/memory-spool` then moved main 0796540 → f836bc6 at
  2026-11-23 23:37:46, landing 3f317c8, 9579cdc, bc33b6a, f936cda ahead of the landing
  phase — regardless of the gate, without the merged-tree re-gate. The same commit rule
  rides the workflow prompts (lumen-development-studio.dwf.ts:350,
  lumen-compositor-bench.dwf.ts:232). Fix shape, both halves: a spool commit's parent is
  main's tip (or the spool tip when main is its ancestor), never a task tip; and the
  landing asserts main moved only by its own merges since the base its tasks were cut
  from — `preMerge` as read at :1134 is too late to see an earlier bypass.
- [ ] **Studio landing counts untracked tool state as a dirty main** (found 2026-11-24
  with run dwfrun-e4cd1b91; medium; source:
  `.zcode/memories/lumen-todo-fixes-round.md`): the landing's clean-main check is
  `git status --porcelain` (lumen-development-studio.dwf.ts:1123-1124), which lists
  untracked files — Mimosa's `.mimosa/` at the repo root made a clean main read "dirty"
  and the phase declined with "merge by hand or re-run". In this clone only: `.mimosa/`
  in `.git/info/exclude`. Owner decision: change the check or ignore `.mimosa/` in
  `.gitignore`. For that decision: the merge-gate configures `-S MAIN_WS` (:1162-1163)
  and the module build globs `*.cppm`/`*.cpp` from the working tree
  (cmake/LumenCXXModules.cmake:51-56), so a blanket `--untracked-files=no` would let an
  untracked source be gated but not pushed — ignore the tool directory and keep the
  check strict, or scope the relaxation to non-source paths.
- [ ] **known_red filters the test under repair out of every workflow gate** (found
  2026-11-24 with run dwfrun-e4cd1b91 — card `known_red: ["smoke_harness"]` for the task
  fixing smoke_harness; medium; source:
  `.zcode/memories/lumen-todo-fixes-round.md`): lumen-development-studio.dwf.ts:529,
  :672, :793 and the landing re-gate :1172 all drop known-red names before judging, so
  the orchestrator's "green" said nothing about the one test the task existed to fix —
  its pass rested on the reviewer's unfiltered run and the gatekeeper's targeted run;
  both gatekeepers flagged it. Fix shape: a task whose diff touches a known-red test
  shows that test green UNFILTERED at its gate (run by name, its `Passed` line recorded
  in the gate reasons); READY is refused without it.
- [ ] **lumen-studio skill: AmendWorkflow drops a run's arguments — restate them in the
  draft before amending** (found 2026-11-24, two amended runs of dwfrun-e4cd1b91; source:
  `.zcode/memories/lumen-todo-fixes-round.md`): `.zcode/skills/lumen-studio/SKILL.md`
  (:36-39, :110) sends a lead to repair a run with `AmendWorkflow` + `path` edits; the
  first amended run (dwfrun-13a56143) exited at once with "No task was given". Also to
  say there: an amended run does not replay finished stages — `world.run` steps execute
  live again (dwfrun-ed413453 re-took the run lock and re-ran the Planner) — so amending
  repairs a run, it never re-issues a landing. A skill edit owes an eval rerun.
- [ ] **~event_loop() leaks its source-table slots** (found 2026-11-24, review of
  invariant-gap-tests; pre-existing; medium): the destructor
  (modules/lx.runtime/lx.runtime.cppm:333-341) unregisters the strand and closes fds but
  never removes the loop's sources from the process-wide table (`loop_state()`,
  k_max_event_sources 32, :287-297; `add_source` returns silently when full, :512-516;
  dispatch walks every remaining slot, :391-396), so a destroyed compositor leaves
  dangling `event_source*` there and a THIRD `compositor::start()` in one process fails
  with "no event source slot" (compositor_impl.cpp:10272-10275) — only four
  `loop.remove_source` calls exist (compositor_impl.cpp:3968, 3983, 3990, 9362). This is
  why test_memory_pressure_cascade runs all four scenarios inside one ctest case.
  Fix shape: remove sources in ~event_loop(), or hand each source an owner hook.
- [ ] **compositor::start() has no seat-less mode** (found 2026-11-24, same review;
  pre-existing; medium): `logind_session::open()` (compositor_impl.cpp:9592) and
  `begin_input()` (:9744) are unconditional — a compositor built for virtual outputs
  still takes the user's logind session (found via sd_uid_get_display) and opens the
  real libinput seat; observed during the invariant-gap-tests suite,
  `loginctl show-session 1 -p Type` read `wayland` mid-run and `tty` after. Every
  in-process compositor ctest (~20 files) therefore touches the seat without a broker
  ticket. Fix shape: a start option that skips logind/input for virtual-outputs
  instances (or broker coordination for test runs); would also give the cascade test a
  hermetic fixture.
- [ ] **text_input_node::insert treats drop_oldest as reject** (found 2026-11-24 with
  test_overflow_caps; pre-existing; low): modules/lx.ui/text_input.cppm:934 refuses the
  insertion when `overflow_` is `drop_oldest`, but the enum says "Evict oldest, accept
  new" (lx.runtime/memory.cppm:42) and the partition's own contract
  (text_input.cppm:70-76) names only reject / drop_newest / warn_and_drop. Decide:
  honour drop_oldest or narrow the contract; the test prints the departure and pins
  only "no growth".
- [ ] **text_layout's line cap ignores overflow_action::reject** (found 2026-11-24 with
  test_overflow_caps; pre-existing since f97a813; medium): under `reject` the pass
  succeeds with the lines that fit and `truncated()` set, where the partition's contract
  (modules/lx.text/lines.cppm:55-57) says reject fails the pass with an empty layout —
  the wrap pass calls `over_cap`, which only warns (:1246-1248, :1274-1278), while the
  byte/glyph/run caps do honour reject (:835-840, :933-938). Also named by the
  "Invariant GAPs" overflow-caps marker.
- [ ] **container_node::add never warns under warn_and_drop** (found 2026-11-24 with
  test_overflow_caps; pre-existing since 1a44c01; low): modules/lx.scene/lx.scene.cppm:547-558
  has no log call on any path, but the enum defines warn_and_drop as "Log warn, then
  drop_newest" (lx.runtime/memory.cppm:43). Also named by the "Invariant GAPs"
  overflow-caps marker.
- [ ] **scripts/diff-hygiene.sh prints a `local` error on every run and leaks its
  format log** (found 2026-11-24, same review; pre-existing; low): line 92 uses `local`
  at top level ("local: can only be used in a function" before the summary line) and the
  clang-format temp file is removed only in the failure branch (:96) — 7
  /tmp/diff-hygiene-fmt.* files left from clean runs. One-line fix each; fold into the
  next scripts/ touch.
- [ ] **settings_ui: after one successful write, an edit that cannot be written raises no
  banner** (found 2026-11-24 with settings-no-writable-banner; pre-existing; medium):
  `session::repaint()` escalates only from `untouched`
  (executables/lumen-settings/settings_ui.cppm:1258-1262) and the five write-failure
  tails raise through it — so once a write has succeeded (`state == written`) a later
  edit that cannot reach the file is applied to the session silently: no banner, in
  exactly the state the banner exists for. Probe in the unit harness (write OK, directory
  made read-only, `set_enum` applies in memory, state stays `written`, nothing shown);
  not exercised on the seat, no test pins it; until now tracked only inside the ticked
  banner row. Fix shape: escalate from any state that is not `memory_only` (`written`
  included), with the regression test's (f)/(g) legs extended to write-then-fail.
- [ ] **Whole-file clang-format debt trips diff-hygiene on most diffs — decision** (found
  2026-11-24 with settings-no-writable-banner; pre-existing; low): settings_ui.cppm is
  not whole-file clean at base and at HEAD alike (`clang-format --dry-run --Werror`
  exit 1, ~360 offending lines; my diff-count 368), test_settings_ui.cpp 9 — so
  `scripts/diff-hygiene.sh <base>` exits 1 on that change's format step with not one
  added line inside a hunk (104/104 and 8/8 hunks identical base vs head), AGENTS.md
  §4.4 "changed C++ files pass clang-format --dry-run --Werror" is unmeetable there
  short of an unrelated reformat, and 47 of 60 sampled tracked .cppm files are not
  whole-file clean at HEAD. Decide: one whole-tree reformat commit, or diff-hygiene
  judging added lines only. (The script's own `local`-at-top-level bug is the row above.)
- [x] **smoke_harness hermetic fixture lost seat-lock.sh** (found 2026-11-19, notebook,
  pre-existing since the seat-broker merge ba80f18): the perfetto sub-suite copies
  `ui-bench.sh` + `bench-warmup.sh` into the fixture dir but not `seat-lock.sh`, and
  ui-bench.sh now re-execs `$ROOT/scripts/seat-lock.sh` — 13 sub-FAILs, every traced
  pass exits 127. One-line fix in `tests/smoke_harness.sh` (stage seat-lock.sh too, or
  stub the broker in the fixture) + its own gated round.
  **Closed 2026-11-23 (notebook, tests-only — `tests/smoke_harness.sh` alone):**
  `49 ok / 13 FAIL` → **`82 ok / 0 FAIL`**, `smoke_harness: PASS` in 2.0 s under ctest
  (TIMEOUT 60). Design in one line: the fixture writes its own `scripts/seat-lock.sh`
  wrapper — it refuses any `XDG_RUNTIME_DIR` but the fixture's private one, records its
  argv and execs the tree's real broker with `--no-preflight` ahead of the `--`, so the
  acquire/export/release contract runs for real in a private lock dir while the
  host-dependent /proc preflight (exit 2 whenever a compositor or smoke runs) stays off.
  A plain copy of the broker was the wrong "stage it too": it fails on a busy host and,
  with `XDG_RUNTIME_DIR` unset, takes tickets in the host's real lock dir. The harness
  now asserts the broker contract per pass, the two hermeticity guards, and a mechanism
  guard (every `$ROOT/scripts/` reference of the copied ui-bench.sh is staged; the
  verbatim copies are byte-identical).
- [ ] **ui_bench_e2e_dry_run is not hermetic — it takes a REAL bench ticket** (found
  2026-11-23, review of smoke-harness-hermetic-broker; pre-existing): `add_test
  (ui_bench_e2e_dry_run` (CMakeLists.txt:1250-1253, TIMEOUT 30) runs
  `scripts/ui-bench.sh --e2e --dry-run --no-build`, whose guard (ui-bench.sh:106-108)
  re-execs the real `seat-lock.sh` in the REAL lock dir (`/run/user/<uid>/lumen-seat`,
  seat-lock.sh:41) with the real /proc preflight — exit 2 whenever a compositor or
  smoke runs on the host, and TIMEOUT 30 expires if a real seat job holds the seat
  (the broker waits DEFAULT_WAIT 900, seat-lock.sh:45) — so the `-LE bench` suite's
  green depends on host state, the same dependence smoke_harness just shed. Observed:
  during the 2026-11-23 full suite (137/137) the real queue/seat.lock mtimes moved
  (~0.14 s ticket; attributed by elimination — smoke_harness left both untouched in
  every isolated run). Fix shape: like smoke_harness's fixture — a wrapper + private
  XDG_RUNTIME_DIR, or a --no-preflight/private-dir path for the dry-run.
- [ ] **CMakeLists.txt smoke_harness registration comment is stale** (found
  2026-11-23, same review; low): the comment above the registration
  (CMakeLists.txt:1255-1257) still describes a three-leg harness (build freshness,
  inject-devices lookup, inject-input key table) and omits the bench warm-up gate, the
  --perfetto traced pass and the seat-broker leg the harness now runs. One-line
  comment repair — fold into the next CMakeLists.txt edit rather than a dedicated
  diff.
- [ ] **test_protocol_conformance session-lock rows flake under load** (seen
  2026-11-19 and again 2026-11-23 during heavy parallel session activity — both times
  failing the "session lock" sub-rows, both times PASSING on a quiet standalone rerun;
  2026-11-23 rerun clean on the notebook). Timing-sensitive, not change-caused (the
  harness change touches no product/test code). Next occurrence: capture the failing
  sub-rows with timestamps and check for libinput/seat-event delay correlation before
  considering a stabilization fix (retry-once in the test or a wider timeout).
- [x] **Agent-level parallelism landed** (2026-11-19, this change): gated-change
  confirmations now dispatch under `Promise.all` (studio already did), and
  `lumen-studio` §Lead duties mandates parallel dispatch of independent role agents.
  Engine-level parallel task LANES remain deferred (row below) — a 2-core host makes
  parallel builds counterproductive; parallelism belongs to agents, not compiles.
- [x] **Commit/PR the ADR-013 settings-app tree** — closed 2026-11-17: everything merged to
  main (`ebe7046` portal Settings, `eaaf7ba` LUMEN_THEME export, `9022e74` settings-startup
  apply); 136/136 ctest at merged main. Push/PR remains the owner's call (never pushed
  without asking).
- [x] **ADR-013 open item 1: in-window key capture** — closed 2026-11-17: shipped in
  `0439bc4` all along (capture state machine, Record button, xkbcommon level-0 keysym,
  shared validation — the TODO text predated that commit); proven live on the notebook
  seat (quiet serialized session, compositor + lumen-settings, injected keyboard):
  Super+Shift+y recorded canonically, Save wrote `"Super+Shift+y" = "shell.launcher"` into
  `[bindings]` with reload generation 2 (57→58 bindings), bare Esc cancelled, bare
  Backspace cleared while Ctrl+BackSpace recorded, reserved Super+Escape showed the inline
  error and Save refused byte-unchanged; Ctrl+Alt+F1 is compositor-consumed before the
  client by design. 136/136 ctest. Evidence: `evidence/2026-11-17-shortcut-capture/`
  (ASSERTIONS.md maps every check to artifacts). One finding recorded to memory:
  injected Super-chords can trip the shell's Super tap grab and steal focus mid-chord
  (launcher open/close in the compositor log) — not a capture defect; see
  `.zcode/memories/lumen-settings-key-capture-seat.md` and the open row below.
- [x] **Super tap grab fires mid-chord (compositor tap-arming corner)** (2026-11-17, found
  by the settings-key-capture seat run; low; pre-existing; **2026-11-24: NOT reproduced at
  any cadence — most plausibly never a tap defect; see below before spending more on it**):
  during an injected Super+Shift+y chord the shell's Super tap grab armed and fired anyway
  — the launcher overlay layer mapped and stole keyboard focus ~20 ms after the meta press
  — although `observe_tap` resets `tap_owner_` on every press
  (`modules/lx.compositor/keybindings.cppm:420-423`). Evidence:
  `evidence/2026-11-17-shortcut-capture/` (inj-s2.jsonl t4-chord: shift press 20 ms after
  meta; launcher-tap-events.txt: layer map surface=6 `lumen-launcher` + focus steal) — see
  `.zcode/memories/lumen-settings-key-capture-seat.md`. To a capture/smoke run it looks
  exactly like "capture silently records nothing" (stale client modifier state until the
  refocus re-sends the keymap). (2026-11-23: the evidence directory is not in the main
  checkout — it lives in the worktree
  `../lumen-wt/settings-key-capture-close-out/evidence/2026-11-17-shortcut-capture/`.)
  **2026-11-24 repro attempt (task super-tap-mid-chord, seat, one class-smoke ticket):
  NOT_REPRODUCED.** 45 injected Super+Shift+y chords under a µs timeline (compositor and
  shell lines stamped on the injector's CLOCK_MONOTONIC; every event's kernel frame
  verified, 0 foreign input): cadence A = the original 20 ms spacing, 20/20 no fire;
  cadence B = human-like 80–200 ms gaps, 100–300 ms holds, both roll and both release
  orders plus meta-first release with y still down, 25/25 no fire; control = lone Super
  taps, 6/6 fired, each action 161–182 µs after the *release of a Super pressed alone*
  (the only shape that ever fires — matching `observe_tap` and its unit tests). No
  `grab activated` line at either session: the configured `"Super"` tap binding shadows
  the shell's tap grab, and the 2026-11-17 config had that binding too. The original
  observation is explained by two by-design mechanisms its untimestamped logs cannot
  separate from a mid-chord fire: the session's own lone-meta tap (`inj-s2.jsonl:137-138`,
  t11-meta — opens the launcher by design) and, after the capture test saved it,
  `"Super+Shift+y" = "shell.launcher"` matching every later chord at `match_binding`
  (compositor_impl.cpp:1788) — logging the same action line as a tap fire. The "~20 ms
  after the meta press" claim has no timestamped artifact behind it. The key path is
  unchanged since that tree (only logging refactors 96e336a/8e925e5), so this was not
  fixed in between. Evidence: `evidence/2026-11-24-super-tap-mid-chord/` in the
  super-tap-mid-chord worktree, mirrored to `/home/dimmus/lumen/evidence/2026-11-24-super-tap-mid-chord/`
  (REPRO.md, VERDICT.md, run/TIMELINE.md + summary.json + raw logs).
  **Closed 2026-11-24 as misattributed (owner decision):** nothing to fix — the
  2026-11-17 sighting is explained by the two by-design mechanisms above, and the
  key path is behaviorally unchanged since that tree. If a mid-chord fire is ever
  seen with real hands, a fresh row opens with timestamped evidence.
- [ ] **ADR-013 open item 2: per-device override editor + window-rules UI** — both are
  schema-side rows in `lumen-settings` waiting on their own compositor feature (rule store
  behind the inert `zlm_window_rules_v1`; `[[input.device]]` override plumbing), not on the app.
  **Corrected 2026-11-23:** the compositor side is on main since `0439bc4` (2026-11-06:
  the window-rules store `window_rules.cpp`, its control methods in `control.cpp`, WM use in
  `window_manager.cppm`, and the `[[input.device]]` edit API in `lx.config/edit.cppm`).
  What remains is the two editor UIs in lumen-settings — an unstarted feature, not a
  blocked row. `docs/adr/013-settings-app.md` carries the same stale "waits on the
  compositor" claim; fix it at the ADR's next touch.
- [ ] **Owed VM golden check (unit-pair-green)** — run `./build/test/lumen-ui-gallery --check`
  on the dev VM (tests-enabled build of the unit-pair-green tree, or main once it merges) at
  the next VM session and record the result; then check this row off. It is the open condition
  of the owner's 2026-11-15 acceptance of the golden substitution
  (`.zcode/memories/lumen-unit-pair-green.md`): cpu expected 0/255 in both themes, gl ≤ 3 vs
  CPU (G10); a VM failure there is a new finding about FreeType-version sensitivity, to be
  handled then — not a regression of the substitution.
- [x] **portal-appearance-settings seat gate red is unattributed** — closed 2026-11-17
  (quiet-host serialized redo on merged main 9022e74, notebook): drag-smoke **10/10 PASS**,
  app-smoke **177 PASS / 0 WARN / 0 FAIL** with Z2 Screenshot `colours [(255,0,0)]` and Z4
  PickColor `255,0,0` as real-pixel PASSes — the Nov-16 "portal answering but capturing
  black" reading is not supported; the 101-FAIL signature matches the concurrent-session
  WARN family. All asserted probe artifacts preserved under
  `evidence/2026-11-17-seat-redo/` (busctl ReadOne u2/u1, SettingChanged dbus-monitor +
  journal, NotFound error-name/message, ReadAll glob, idle-strace: backend parked in
  poll(-1), zero syscalls/zero CPU over 30 s windows). Independent analyst recount
  confirmed every number. Note: this row's "no BTN codes" node reading was a
  `/proc/bus/input/devices` bitmap misreading — see
  `.zcode/memories/lumen-uinput-key-bitmap-format.md`.
- [x] **compositor-theme-env-export seat gate BLOCKED** — closed 2026-11-17 (redo on merged
  main 9022e74): stale holders killed (pids 3604798/3604800 from Nov 16; the original
  2692863 died earlier, boot_id unchanged proves no reboot), node regenerated from this
  tree with button caps EVIOCGBIT-verified, acceptance 2-4 run as written incl. the
  letter — appearance flipped **through the lumen-settings UI** (app wrote exactly the
  `appearance` line; compositor re-exported `LUMEN_THEME=one-dark`/`one-light`; app re-themed
  live 0.179/0.968), startup export with empty ui.theme → one-light, verbatim `ui.theme`
  name export, lumen-hello relaunch light 0.9189 / dark 0.2061, lumen-settings fresh start,
  teardown unset, config byte-restored (sha256 082ce0e5…). Smokes green (see the portal row).
  The lumen-shell SIGSEGV did **not** reproduce — split to its own row below (non-reproduction
  is not attribution). The row's "no BTN codes" mechanism was a bitmap misreading — see
  `.zcode/memories/lumen-uinput-key-bitmap-format.md`; the Nov-15/16 click-loss is best
  attributed to the concurrent-session WARN family. See
  `.zcode/memories/lumen-compositor-theme-env-export.md` (2026-11-17 section).
- [x] **lumen-shell notification_view intermittent SIGSEGV** (split 2026-11-17 from the row
  above): ×2 on 2026-11-16 (coredumps 3499403 @21:41:39, 3521729 @21:53:11, identical
  stacks notification_view::build_ui → lx::ui::image → lx::runtime::memory_arena::allocate,
  SI_KERNEL, EXE = the theme-env worktree build), unreproduced 2026-11-17 on merged main
  9022e74 across both smokes, the N1–N8 notification rows and two interactive sessions —
  intermittent, mechanism unattributed. Original stacks preserved at
  `evidence/2026-11-17-seat-redo/a-smokes/coredump-originals-Nov16.txt`.
  **RESOLVED 2026-11-18** (the TODO's own ASan-shell-on-the-seat prescription, run during the
  sanitizer-sweep seat bisect): the class is **retained text blocks holding pointers into
  build_ui() frame storage** — `text_block_node::apply` re-`strlen`s the pointer at every
  tick's reconcile; notification_view was a downstream victim consuming the poisoned stack,
  never the origin. ASan caught three origins: panel tiles (fixed 655ae13 on this branch),
  switcher letters (`switcher.cppm` `letters_[]` member), menu row labels (`menu_popup.cpp`
  `level::label[]` — the mnemonic-stripped `std::string`'s SSO dangled), plus the launcher
  status line (`launcher.cppm` `status_line_[]`). The 2026-11-18 deterministic 5/5 mass-FAIL
  on this branch vs green base was the panel fix's stack perturbation flipping the
  intermittent bug — evidence in `evidence/2026-11-18-sanitizer-sweep/seat-regression/`.
- [x] **lumen-shell heap-use-after-free at tray-menu close** (found 2026-11-18, ASan shell
  on the seat, sanitizer-sweep branch; FIXED 2026-11-24, task `shell-tray-menu-uaf`):
  `menu_popup::close_from()` destroyed the level's surface — its `ui_context` — while the
  menu's own input router was still delivering the Enter that activated the row; the key
  dispatch's tail (`ui_root::key` → `overlays.after_input()`) then read the freed context.
  Full report captured on the seat (READ + freed-by + allocated-by stacks, written by the
  sanitizer runtime) and reproduced in-process by `test_shell_menu`'s new popup-close
  phase. Fix: the teardown waits for the delivery (and the popup-done listener) to return
  and drains in `after_dispatch()` — the calendar popup's rule; post-fix seat loop 30/30
  clean (capture was 1/1). Evidence:
  `/home/dimmus/lumen/evidence/2026-11-24-shell-tray-menu-uaf/`. Sighting: shell-n.log:184-186
  of `evidence/2026-11-18-sanitizer-sweep/seat-regression/asan-final-shell-n.log` (the tray
  rows had already passed; menu/tray code untouched by the branch → pre-existing on
  main, only observable under ASan). Next: targeted repro (ASan shell + tray menu
  open/activate/close loop) to capture the full report.
- [x] **lumen-settings transient "no writable config.toml" banner** — closed 2026-11-24
  (settings-no-writable-banner: reproduced on the notebook seat, fixed, fix proven there).
  It is not transient: on `a89cbad` the banner is up by +2 s after a fresh launch with no
  input event, through +30 s, over a writable user file, and goes only after a successful
  write. Root cause: `session::repaint()` turned `untouched` into `memory_only` on any
  call, and ten callers that are not edits went through it — `set_current`,
  `toggle_expanded`, `open_group`, `begin_edit_action`, `cancel_edit_action`,
  `begin_binding_capture`, `stop_binding_capture`, `set_open_menu`, `capture_binding_key`
  (two branches). The launch trigger is `main.cpp`'s `shortcut_input::on_keymap` →
  `stop_binding_capture()`: the keymap reaches the client 86 ms into a launch
  (WAYLAND_DEBUG trace). Fix: those callers re-describe through `refresh()`; `repaint()`
  stays with the five failure tails. Regression ctest:
  `settings_banner_is_raised_only_by_an_edit_that_did_not_reach_the_file` — red on the
  pre-fix logic (13 of 13 non-edit callers named), green after. Seat A/B, one
  class-smoke ticket per job, same procedure and the same two keys written: pre-fix
  banner PRESENT on 13 of 17 shots; fixed PRESENT only on the two shots of an edit that
  cannot be written (directory read-only), ABSENT after the next successful write. ctest
  141/141. Evidence, mirrored to the main checkout:
  `/home/dimmus/lumen/evidence/2026-11-24-settings-no-writable-banner/` (REPRO.md, `pre/`,
  `post/`, `post-run1/`, `keymap-trace/`, two recorded pre-fix attempts). **Seen, not
  fixed (outside this fix):** `repaint()` escalates only from `untouched`, so after one
  successful write a later edit that cannot be written applies to the session and shows
  nothing — no banner (probe in the unit harness, `session::repaint()`).
  As written (2026-11-17 seat redo,
  supplement): a "Changes are not being saved / There is no writable config.toml" banner
  showed ~10 s after a fresh lumen-settings launch and vanished after the first successful
  write — while locate_config resolved the real user file and the writes demonstrably
  worked (`evidence/2026-11-17-seat-redo/d-supplements/shot-pre-flip-banner-zoom.png`);
  no startup path setting the banner's memory_only state was found by source reading.
  Reproduce, then fix the stale banner state. **Lead 2026-11-23 (read from source, not
  yet reproduced):** `session::repaint()` flips `save_` from `untouched` to `memory_only`
  on *any* repaint (`executables/lumen-settings/settings_ui.cppm:1250-1254`) and
  `set_open_menu` calls `repaint()` (`:314-317`) — so opening or dismissing a dropdown
  with nothing edited raises the banner until a write succeeds. Decide on the seat by
  opening a dropdown without editing.
- [x] **snprintf-log-migration gate BLOCKED: seat evidence owed + vsnprintf acceptance
  unratified** — closed 2026-11-23 (reconciliation; closing note at the end of this row).
  As written 2026-11-17: branch `zcode/task/snprintf-log-migration` 8e925e5..b556db2,
  unmerged; code side verified — ctest 136/136 gate-verified at b556db2, interleaved
  ui-bench A/B vs 9dabcaf 0 FAIL in all 6 pairs, final grep = 4 documented exceptions):
  the implementer's drag/app-smoke PASS runs left no surviving evidence and the gate-time
  re-runs (drag-smoke-logs/run.log Nov 17 23:26, app-smoke-logs/run.log 23:48) failed
  environmentally — uinput holder dead (pid 3845078), pointer fell back to event2
  "no absolute axis 0" ×13, "another session is running" — every failing row
  injection-dependent, non-injection rows clean. Remediation: regenerate the holder
  (`scripts/uinput-mouse.py`), ensure no concurrent session, re-run both smokes
  `--join-seat-session` on the notebook, commit the logs under `evidence/`, and capture
  the app-smoke PASS/WARN/FAIL row counts vs the ~140 floor. Separate ratification item
  for the owner: the acceptance grep as written allows only `error.cppm:200`, but
  `lx.config/parse.cpp:988` + `lx.input/keymap.cppm:277,283` keep commented vsnprintf
  renders of xkbcommon's run-time printf format (std::format cannot consume it; the task
  notes forbade a helper) — ratify the amended acceptance or direct another resolution
  (see `.zcode/memories/lumen-snprintf-log-migration.md`).
  **Closing note (2026-11-23):** both smokes were re-verified PASS on the notebook on
  2026-11-18 after the uinput-holder restart (the record's "Seat re-verification
  2026-11-18 — the authoritative evidence"), and the branch was merged into main the same
  day (`08bde4e`, tip `95372af`): all 329 sites migrated, 4 documented exceptions — the
  three `std::vsnprintf` renders sit on main at `lx.config/parse.cpp:988` and
  `lx.input/keymap.cppm:277,283`. The amended acceptance is taken as ratified by that
  merge and by the record's own "How to apply" (one commented `vsnprintf` at a
  foreign-printf boundary); reopen if that is not the owner's view.

- [x] **Tracked CRLF files on main past .gitattributes** (found 2026-11-19 by
  scripts/diff-hygiene.sh) — closed 2026-11-23 (owner decision: untrack): 12 blobs under
  `evidence/2026-11-18-sanitizer-sweep/` (app-smoke/drag-smoke/asan logs) had been
  force-added with CRLF bytes against the `eol=lf` policy, and `evidence/` never merges
  (37 files were tracked there in total). `git rm -r --cached evidence/` on main: the files
  stay on disk and in history and the `/evidence/` ignore rule covers them, so
  diff-hygiene no longer reports the debt. Branches cut before this change that edit
  those paths show delete/modify conflicts on `evidence/` when merged — resolve by keeping
  them untracked.
- [ ] **Studio parallel task lanes** (deferred 2026-11-19 from the harness review): the
  engine supports concurrency (confirmers already run under Promise.all) but the task
  loop is strictly sequential with shared mutable state (knownRed, branchOf, the board).
  Design sketch: max_parallel lanes over tasks with disjoint scopes and no dependency
  edges between them, seat steps already serialize through the broker, run lock already
  prevents run collisions. Needs a live workflow run to validate engine semantics
  (report/board interleaving, world.run isolation) before shipping — do not land it
  unexercised.
- [x] **compare.sh broker wiring** (2026-11-19) — closed 2026-11-23 (notebook session,
  owner-approved wrap): ~/lumen-compare's compare.sh lives outside the repo and now
  re-execs itself through `scripts/seat-lock.sh with bench compare --` on a direct run —
  the guard sits before its first side effect, and `LUMEN_SEAT_LOCKED=1` from an outer
  wrap (the lumen-compositor-bench workflow) means it proceeds, so that wrap still
  composes. Verified: `ROUNDS=1 NAMES=lumen ./compare.sh` → `seat-lock: acquired class
  bench owner compare`, ticket held mid-run, released after, exit 0. Next touch of
  `lumen-seat` SKILL.md §"Who holds a ticket": move compare.sh from "Calls the broker by
  hand" to the self-brokering list (left undone here — a skill edit owes an eval rerun).
- [ ] **Invariant GAPs** (docs/invariants.md, 2026-11-19; count corrected 2026-11-23 — the
  row first named four; four designated tests added 2026-11-24): six §5 invariants had no
  designated test. One is closed: GPU pool reuse across frames — `test_gpu_pools` (both
  pools over 64 simulated frames; its semaphore half runs on the notebook's hasvk device).
  Closed is the missing test only: the pools are on no frame path, which is the next row.
  Three keep a narrowed marker for what the new test cannot evidence: the memory-pressure
  cascade — `test_memory_pressure_cascade` drives the import-cache and buffer-lifecycle
  legs from one started compositor's own coordinator; the `shm_pool_.trim()` leg stays GAP
  (a private pool, its bytes only in the health report over the control socket); overflow
  caps — `test_overflow_caps` drives 13 structures and found two that do not do what their
  action says (`text_layout`'s line cap keeps what fits under `reject`,
  lx.text/lines.cppm:1246-1278 against its contract at :55-56; `container_node::add` never
  warns under `warn_and_drop`, lx.scene/lx.scene.cppm:546-559) — left unpinned and marked;
  the present-mode policy — `test_present_mode` pins the selection, but
  `output_manager::present_mode_for` has no caller, so the step from the selected mode to
  the frame path stays marked. Two stay GAP because the invariant is not implemented:
  content-hint scanout priority (`content_hint` is stored and read back and never used to
  choose a scanout — surface_manager.cppm:213, 852-866; no direct scanout exists) and the
  kms_damage contract (the commit half exists, but `page_flip_event::applied_damage` is
  written nowhere — lx.drm/atomic.cppm:120). Close each by adding the check named in the
  row, or by building what the row describes; never by weakening the row.
- [ ] **The §5 GPU pools are on no frame path** (reported 2026-11-24 with `test_gpu_pools`;
  docs/invariants.md "GPU pools" row): AGENTS.md §5 says "GPU pools: use `pipeline_cache`
  and `lx.gfx.semaphore_pool` (`timeline_semaphore_pool`) — no per-frame create/destroy",
  and neither pool is on a frame path. Nothing under modules/ or executables/ calls
  `timeline_semaphore_pool::acquire` — not the `semaphore_pool()` accessor of the
  `syncobj_bridge` that owns one (lx.gfx/syncobj_bridge.cppm:44, 58, 77) — and
  `pipeline_cache::acquire` has one caller, `frame_renderer::draw_to_headless`, which drops
  the id it is handed (lx.gfx/renderer.cppm:536). The Vulkan composite path makes its own:
  `vulkan_compositor::initialize` creates its fences and its exportable semaphores once per
  frame slot (lx.gfx/vk_renderer.cppm:1907-1942), and its pipelines go through a
  `VkPipelineCache` (lx.gfx/vk_renderer.cppm:168-173). `test_gpu_pools` evidences the two
  pools themselves, driven directly; it cannot show a frame getting back what an earlier
  frame used until a frame path uses what a pool hands back. Not measured: whether any
  frame path creates or destroys per frame regardless. Close by a frame path that uses what
  the pools hand back, with a test that sees that frame — or, if the composite path's own
  means are the design to keep, by an owner decision that rewrites the §5 line and the row
  to say so.
- [ ] **Remaining LONG memory records** (2026-11-19; count corrected 2026-11-23): the
  "19" counts "LONG" marker *lines* in the generated `.zcode/memories/INDEX.md` — one per
  module group a record is filed under. They cover 12 distinct records; 13 files under
  `.zcode/memories/` exceed 120 raw lines (127–275). The ≤120-line rule applies at next
  touch, split all of them eventually.

## Host-limited gates (ADR-010; VM cannot close these)

- [x] **Vulkan capture green** (replaces the portal task, owner decision 2026-11-14) —
  closed 2026-11-23 (reconciliation; closing note at the end of this row). The ask as
  written then: make
  the Vulkan composite path serve what its GL/CPU siblings do, so the app-smoke rows that
  now WARN naming the gap turn into real PASSes — the SIGUSR1 debug read-back (GL path
  only, `compositor_impl.cpp` `write_screenshot`): S2 T4 T7 L2 L3 Q4 O2 O6 M1 N6 Y1 and
  Z1; window (toplevel) capture (refused in `wire_capture.cpp` `create_session`): Y2 Y5
  Q6, Z6, Z7; capture dma-buf buffers (GL-only, same file): Y4 `into dmabuf`. The harness
  half closed 2026-11-14 in `smoke-bench-harness-green` (host-aware geometry, injector
  pixel-exactness, REL-capable virtual mouse, the backend-limited WARNs and the A/B
  evidence under `evidence/2026-11-14-ui-bench-ab/`).
  **Closing note (2026-11-23; delivered 2026-11-15, on main):** the Vulkan composite path
  serves its GL/CPU siblings' capture — SIGUSR1 read-back (`write_screenshot` via a
  one-shot pooled `read_back`), window capture through the un-gated
  `ext_foreign_toplevel_image_capture_source_manager_v1`, capture dma-buf — all in
  `lx.gfx` behind `void*` interfaces; the rows that WARNed naming the gap are real PASSes
  (notebook app-smoke 177/0/0). Record: `.zcode/memories/lumen-vulkan-capture-green.md`
  (gate READY after two NOT_READY rounds). On main by content: the capture-manager symbol
  in `wire_capture.cpp`, `wire_install.cpp`, `wire_foreign_toplevel.cpp` and
  `lx.portal/desktop.cpp` with the branch tip's counts, `read_back_wait_fence` /
  `vkCmdCopyImageToBuffer` in `lx.gfx/vk_renderer.cppm`, `test_vk_capture` registered in
  `CMakeLists.txt`.
- [x] **Frame-callback / repaint-lead misses for fast-answering clients** (2026-11-03
  found, 2026-11-04 fixed): a wl_surface.frame-driven client answering in ~0.1 ms got
  46–53 fps with ~25 % of frames a period late, while ticks cost 0.4 ms and health said
  0 missed. Root cause was NOT the client-answer adaptation: (1) the aim/health pairing
  kept one slot, overwritten by the next tick before a slow frame's flip landed — the
  late frames were invisible to health and to any feedback; (2) the repaint lead had no
  whole-path feedback (render-thread wake + commit submission + kernel latch slack).
  Fixed by the four-entry aim ring in `output_ui` (health sees the misses again) and
  `repaint_scheduler::observe_commit_late` (a late flip feeds commit-vs-aim distance +
  step into the lead, windowed peak, converging in one miss). Verified: 6× pres-probe
  runs 59.81–59.89 fps, 1 late/200, sd 1.2 ms; vkmark fifo unchanged (60.00, sd 0.00);
  134/134 ctest; drag-smoke PASS (window-move median 16.8 ms, gate 18.7 — the honest
  lead costs ~4 ms against the broken-state 12.7, still ahead of weston/sway's 22/33).
- [x] **"Console fbcon not re-armed after a session exit on i915"** (2026-11-03 raised,
  2026-11-04 resolved — not a compositor bug): the restore was correct all along
  (`kms_pipeline::~kms_pipeline` SetCrtc of the saved console fb returns 0; the plane is
  back at fb 94 within ~2 s of logout, verified by parallel polling and a manual
  run-desktop session). The failing check was session-smoke's driver detection: it ran
  `modetest -M vmwgfx`, and the first two host-generic rewrites read the wrong sysfs
  node (the connector's `device` names the card, a drm_minor whose uevent has no DRIVER
  line; the PCI device is the card's `device`). Fixed with `drm_driver()` resolving
  `readlink -f $connector/device` + `/device/uevent`; session-smoke 24/24 PASS on
  hardware since.
- [ ] **G2 p95**: S2 301-node panel clock frame — p50 passes (95 µs), p95 490–540 µs vs 150 µs
  budget; biggest lever is reshaping the clock label. (ADR-010 §8 says host wake jitter.)
- [ ] **G7 cold on GL**: p95 135–156 ms vs 150 ms budget — driver cost (EGL + Mesa compiles +
  fontconfig ≈103 ms), Lumen ≈5 ms; no GL program binaries on SVGA3D; build-ahead measured
  and rejected.
- [ ] **G11 frame rule** (no frame > 2× median) — VM jitter; passed on a quiet host.
- [ ] **Explicit sync on the wire** untested: no syncobj manager headless, no timeline
  syncobjs on vmwgfx. Needs hardware.

## Notebook (`iritur`) checks

- [x] Notebook `ui-bench` baselines committed (2026-11-03): `iritur-08fd3c-{none,cpu,gl,
  vulkan,client-cpu}.json` — hasvk Vulkan is a hardware backend there now, no longer
  "vulkan-software". The GPU tests take the connected card's render node
  (tests/render_node.hpp; `LUMEN_TEST_RENDER_NODE` overrides).
- [x] Hardware Vulkan numbers landed (Broadwell hasvk, Mesa 26.2 reports 1.3.354 — the
  floor passes; `Vulkan composite path active` verified on the eDP): vkmark immediate
  686–937 FPS / fifo 60.00 sd 0.00, drag-smoke G6 move median 12.7 ms with a measured
  0.8 ms gpu fence leg (rendering-performance.md). The eDP panel is not host_image_copy
  capable, so that upload path stays unmeasured on this host.
- [ ] PRIME offload (PR #71): an app launched "using discrete GPU" should report the
  GTX 950M (look for `shell.apps` "GPUs:" lines). switcheroo-control is not installed.
- [ ] Thread-scheduling tiers re-measure (reverted on GL) and fence-deadline effect —
  now measurable on hasvk/i915.
- [ ] Hyprland in the startup comparison: 0.57 wedges before its IPC socket exists when
  launched from SSH (`start-hyprland` wants a real tty); weston/sway compare fine.
  Needs a console-launched run by the user (`~/lumen-compare`, pres-probe).

## Parked (one fix round each, or waiting for a port)

- [ ] **GL reset recovery** (raise-the-bar S2, reverted): capture cursor lost after reset.
- [ ] **Rounded corners / window shadows** (raise-the-bar F1, reverted): radius 30–32 masked
  client top corners; GL zeroed arrays per draw.
- [ ] **Shell background render cache + loop order**: still only in unpushed WIP c8d4499 on
  `claude/shell-first-paint` (old base) — port onto current main, then bench/tests/docs.
- [ ] **Tiling unbuilt** (ADR-012 trade-offs): pointer-drag split resize, orientations,
  monocle/dwindle.
- [ ] Non-text drag-and-drop with a real app on the real display (uri-list/icon/ask shipped
  and smoke-tested only).

## Do not re-open without new evidence

highp shaders, frozen test clock, malloc replacement, SDR-through-sRGB-view, GL build-ahead,
shell-side tiling, area-filter cost at non-integer render scales — see
`.zcode/skills/lumen-decisions/`.
