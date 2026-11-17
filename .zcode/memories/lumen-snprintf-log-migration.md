---
name: lumen-snprintf-log-migration
description: "2026-11-17 notebook: all 329 modules/ snprintf sites migrated to the §4.2/§4.3 format conventions (final grep = 4 lines, all documented exceptions; format_to_n idioms, printf-parity checks, sed-rewrite traps); ctest 136/136 + ui-bench A/B 0 FAIL verified, but gate BLOCKED — smoke PASS logs overwritten by harness-broken re-runs — and the 3 xkbcommon vsnprintf exceptions unratified"
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-17 on the notebook (task `snprintf-log-migration`, worktree
`/home/dimmus/lumen-wt/snprintf-log-migration`, branch `zcode/task/snprintf-log-migration`,
base 9dabcaf; code 8e925e5 + ac869f6 + 96e336a + ad6e30b + bf716ab, harness a49f9a0,
memory 497f92c→b556db2 — unmerged; gate **BLOCKED**, see below). All 329 `snprintf` sites
in `modules/` migrated to the AGENTS.md §4.2/§4.3 conventions (full diff 68 files
+597/−469, incl. the two `.zcode` agent files). Re-ran at record time: `grep -rn snprintf
modules/ --include=*.cpp --include=*.cppm --include=*.hpp` returns exactly 4 lines —
`lx.foundation/error.cppm:200` (the allowed `format_error_into`) plus three `vsnprintf`
renders of xkbcommon's run-time printf format (`lx.config/parse.cpp:988`,
`lx.input/keymap.cppm:277,283`), each carrying the naming comment: `std::format` cannot
consume a foreign printf format and the task notes forbade a shared helper.
`lx.trace.cppm`'s four optional copies were migrated (not left). The ~213 snprintf sites in
executables/, tools/, tests/ are outside the owner's count (329 = `modules/` exactly) and
out of scope.

- **The idiom**: `*std::format_to_n(dst, sizeof(dst) - 1, "...", args).out = '\0';`
  reproduces snprintf's truncation + guaranteed NUL (in-repo precedents
  `lx.config/diagnostics.cppm:44-48`, `lx.app/clipboard.cpp:211`). Append chains read the
  untruncated length from `format_to_n_result::size` (snprintf's return contract) and still
  terminate each segment. `%g`→`{:.6g}` and `%.2f`→`{:.2f}` were verified byte-identical to
  printf over 23 + 18 representative doubles before converting; `%.61s`→
  `string_view::substr(0, 61)`; run-time-chosen format strings (`"%.2f/%.1f/%.0f"` ternary
  in `inputs.cppm`, `"%u application(s)"` in launcher) become literal branches — a ternary
  of literals is not a constant expression for `format_string`.
- **Scripted-rewrite traps** (all hit, all fixed before commit): (1) regex-rewriting
  `n = std::snprintf(...)` into `n = *std::format_to_n(...).out = '\0'` silently sets
  `n = 0` — it compiles clean and would have made both `cache_dir()`s (`ui_gl.cppm`,
  `ui_vk.cppm`) always return false with XDG_CACHE_HOME set, disabling the program caches;
  the correct form assigns `n = seg.size`. (2) A leading `(void)` discard collides with
  `*...out = '\0'` ("assignment to cast is illegal"). (3) `{:02u}` is invalid — std::format
  has no `u` presentation type; unsigned decimal is `{:02}`. (4) Placeholder-count
  mismatches surface as "call to immediate function is not a constant expression", not a
  format error. Audit every scripted site for null `char*` args: glibc `%s` printed
  `(null)`, `{}` on null is UB.
- Messages that existed only to be logged became direct category calls;
  `blank_outputs(const char* why)` became `[[nodiscard]] bool` (`lx.shell.cppm:351,1312`)
  with its **four** call sites (`:1260, :1277, :1903, :2059`) logging their own
  `log_idle.info` line, text unchanged, only on `true` — the log-only-when-off condition
  survives via the return value. Script-grepped lines stay byte-identical: gate spot-check
  found `present kind=…` (`compositor_impl.cpp:2020`) and `flip frame=…`
  (`wire_presentation.cpp:259`) character-identical to 9dabcaf, and `output 0 mode=` /
  `map id=… app_id=…` live in the gate-run compositor log.
- **Gates**: baseline ctest 136/136 → 136/136 after (gate-verified per packet at LastTest.log
  start Nov 17 23:23 = b556db2; the artifact was since overwritten at 23:56 by an empty run —
  121 bytes, 0 tests — the recurring Testing/Temporary trap; `CTestCostData.txt` 23:25 still
  holds 136 entries + separator, corroborating the run but not pass/fail). Interleaved
  ui-bench A/B vs base 9dabcaf (`/tmp/ab`, 12 JSONs = 3 rounds × vulkan + GL, new git
  a49f9a0): 0 FAIL in every pair by the bench's own thresholds — **not** "0 WARN": 4–6
  warnings per pair, all "gate G5 still fails (baseline too)" or driver-growth deltas
  matching baseline, plus two single-round p50 moves (S2 submit +15.2 %, S4 gpu +24.2 % in
  vk round 1 only) that do not repeat in rounds 2–3. Sub-µs idle stages read +20 %
  (0.156→0.191 µs) binary-deterministically on both backends — below `ABS_FLOOR_US=1.0`,
  the class the floor exists for; every stage ≥1 µs stayed within ±4 %.
- **Gate verdict BLOCKED (2026-11-17)** — the seat half of the acceptance is unproven: the
  implementer's drag-smoke/app-smoke `--join-seat-session` PASS claims left no surviving
  evidence, and the gate-time re-runs (`drag-smoke-logs/run.log` 23:26,
  `app-smoke-logs/run.log` 23:48, both in the worktree) are VERDICT: FAIL on a broken
  injection harness, not compositor behavior — line 1 `pointer /dev/input/event2 (default:
  no 'Lumen Inject Mouse')`, 13× `inject-input: /dev/input/event2 has no absolute axis 0`,
  `WARN: another session is running`; the uinput holder (pid 3845078, `/tmp/uinput-holder.log`,
  node event16) is dead now. Every failing row is injection-dependent while all
  non-injection rows passed with a clean compositor (0 error/warning lines in
  `drag-smoke-logs/compositor.log`, clean exit) — the documented
  [[lumen-app-smoke-known-noise]] failure mode, and environmental failure never converts to
  a pass. Remediation: regenerate the uinput holder (`scripts/uinput-mouse.py`), ensure no
  concurrent session holds the seat, re-run both smokes on the notebook, capture the
  app-smoke PASS/WARN/FAIL row counts against the ~140 floor.
- **Review noise 2/2/0/0** (raised/confirmed/refuted/preexisting), both confirmed findings
  dispositioned: (1) low — this record's predecessor (497f92c) said blank_outputs has "the
  three callers" logging byte-identical lines; there are **four** — a future auditor would
  stop at three; fixed in b556db2 (1 file, 1 insertion, 1 deletion, whole fix delta). (2)
  low — the delivered state does not satisfy the acceptance grep as literally written (the
  enumerated allow-list covers only error.cppm:200 and optionally lx.trace.cppm); the three
  xkbcommon vsnprintf sites were judged correct (run-time printf format, bounded,
  non-allocating, commented) — a **ratification item for the owner**, not a code defect;
  no commit should "fix" it.
- **Fuckups**: (1) claiming seat PASS without preserving evidence — the passing logs were
  overwritten by the gate's failing re-runs, and the app-smoke PASS/WARN/FAIL row counts
  were trimmed by a `tail -8` pipe; rerun results count only once committed under `evidence/`
  (the [[lumen-panel-blur-fix]] lesson, re-learned). (2) "three callers" vs four in a memory
  record — recount before writing numbers into the archive.
- **Host findings**: `lx.shell.cppm` and `lx.drm.cppm` are **LF** at base and HEAD
  (`git ls-files --eol` i/lf w/lf) — the lumen-decisions CRLF gotcha predates commit
  1a44c01's normalization; re-verify, don't trust the skill line. `lx.app/clipboard.cpp`
  was a precedent, not a site (untouched); `lx.foundation/error.cppm` untouched by design.

**Why:** printf→std::format migration has silent traps that compile clean (the
`n = *...out = '\0'` mangling is the worst — zero writes, working build); and a gate can
only certify evidence that survived — PASS claims whose logs later get overwritten are
holds, not passes.

**How to apply:** for any new bounded fill use the `*std::format_to_n(...).out = '\0'`
idiom, never snprintf into a stack buffer; a foreign printf-format boundary keeps one
commented `vsnprintf` (see the three) rather than a helper; when auditing a scripted
rewrite grep for `= \*std::format_to_n` and `(void)\*std::format_to_n` first. Before seat
smokes regenerate the uinput node and check for a concurrent session; commit gate evidence
under `evidence/` immediately. Never cite `Testing/Temporary/LastTest.log` without a fresh
run — it gets overwritten by later no-op ctest invocations. At merge, keep this
main-checkout copy of the file (add/add vs the branch's b556db2 copy, which predates the
gate verdict). See [[lumen-ui-bench-noise-and-g7]] for the p50/floor judgment the A/B
verdict used; related also [[lumen-notebook-hw]], [[lumen-no-degradation-rule]].
