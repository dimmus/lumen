---
name: lumen-sanitizer-sweep-green
description: "2026-11-18 notebook: first-ever sanitizer runs on this host — ASan+UBSan 97/136 → 136/136, TSan 7-target link failure → 97/136 (62 races) → 136/136, normal gate 136/136 at tip 7870383 (branch zcode/task/sanitizer-sweep-green from 95372af, unmerged, gate NOT_READY on the seat-smoke acceptance only); eight defect classes fixed incl. the libwayland 1.26 no-free-at-disconnect contract and libstdc++ 16's stable_sort buffer; review noise 12/12/0/0; open: app-smoke mass-fail + notification_view SEGV identical on both trees — the user's call"
metadata:
  node_type: memory
  type: project
summary: "First ASan+UBSan/TSan sweep: 97/136→136/136 both, eight defect classes (render_node UAF, fdinfo race, nothrow-new pairs, retained-text stack lifetime ×4 origins); five name suppressions"
modules: harness, modules
topics: asan, tsan, sanitizers

---

Verified 2026-11-18 on the notebook iritur (task `sanitizer-sweep-green`, worktree
`/home/dimmus/lumen-wt/sanitizer-sweep-green`, branch `zcode/task/sanitizer-sweep-green` based on
95372af of [[lumen-snprintf-log-migration]]; sweep round 9ecbd1a..0e0571c, review-fix round
86b9c23..9668aef + dfe18cd/f47506a/9668aef/b6c3442, record 7870383 — unmerged; gate **NOT_READY**
on exactly one gate, see below). All evidence lives on the branch under
`evidence/2026-11-18-sanitizer-sweep/` (TRIAGE.md classifies every distinct report; cited log:line
numbers below verified from those committed logs this session).

- **Baselines (unmodified 95372af — no sanitizer suite had ever been recorded on this host)**:
  ASan+UBSan 97/136, 39 failed (17 HUF, 18 LSan, 4 alloc-dealloc-mismatch, 1 stack-use-after-return,
  1 UBSan) (`asan-baseline-ctest.log:34612`); TSan could not even link 7 targets (duplicate operator
  new/delete), and after `-k` ran 97/136 with 62 race warnings (`tsan-baseline-ctest.log:4069`).
- **Genuine defect classes fixed** (sweep round, tests where non-trivial): tests/render_node.hpp:51
  UAF — card name built from the `DIR` buffer after `closedir` (9ecbd1a); lx.drm fdinfo probed fd
  NUMBERS listed by /proc/self/fdinfo while the input thread closed them → dup first (fe56584);
  memory_arena used-bytes statistic read cross-thread → relaxed atomic (babcb69); lx.text itemizer +
  shape_cache nothrow-new/free pairs → malloc/free (efe32b0); **libstdc++ 16's
  `__get_temporary_buffer` (stable_sort's buffer) has the same pair and aborts instrumented code at
  every `std::stable_sort`** → all four call sites use snapshot_buffer's exact-stable permutation
  idiom, tie order pinned by tests (573d7f3, cd4122b); panel tile/overflow/bell strings pointed into
  build_ui's stack frame across ticks (655ae13); UBSan int overflow in test_dnd_grab; allocation
  counting operators cannot exist under TSan (0b455b); **libwayland 1.26 contract:
  `wl_display_disconnect` frees NO remaining proxies** (~1800 leaked 96-byte proxies across the
  client tests) → tracked-created/destroyed-at-disconnect discipline, idempotent `disconnect()`
  guards required or the plain build segfaults (c73ed65, 9b06888, 95950a0, d67601f, bb0dcf0).
- **Suppression extensions** — five third-party object-name entries only
  (`tests/sanitizers/tsan.supp`: race:libvulkan_intel_hasvk, race:libVkLayer_khronos_validation,
  race:libgallium; `lsan.supp`: leak:libfontconfig, leak:libsystemd), each protocol-verified per
  TEST.md (12-run zero-Lumen-frames ownership, load-bearing run fails rc=66/rc=134 with third-party
  top frames, canary still reports; records + canary sources in
  `evidence/2026-11-18-sanitizer-sweep/suppression-protocol/`). Two per-test blunt concessions
  documented in TEST.md and consciously accepted by the gate (EGL-display trio `detect_leaks=0` —
  dri2 frames symbolize to Mesa source paths, no object name to match;
  test_deco_titles_vk_validation `alloc_dealloc_mismatch=0` — SPIRV-Tools' own stable_sort).
- **Review-fix round (same day evening; review noise 12 raised / 12 confirmed / 0 refuted /
  0 preexisting)**: two follow-on TSan races the first pass missed — scene_graph::ensure_view write
  vs snapshots() read → view slots published through an atomic pointer, release/acquire (86b9c23);
  output_manager::recount raced a test-side unlocked `count()` read (bb51b4c). **CMake
  `set_tests_properties(… ENVIRONMENT …)` is a whole-list overwrite** — per-test sanitizer envs had
  silently removed the shared `LUMEN_SANITIZER_ENV`, so five TSan tests ran with no TSAN_OPTIONS at
  all; env is now APPENDed once after the last add_test (aab2831). protocol_conformance's ~900
  leaks proved tractable (3f1b82f): track every creation, untrack before every typed destroy,
  teardown = dispatch_pending → reverse sweep → registry → disconnect; kits that copy a client mark
  borrowed — 242 cases, 0 failed, 0 leaked. Two latent defects the missing env had hidden: bench_ui's
  four nothrow deletes outside the LUMEN_TEST_ALLOC_COUNTING guard (19da34c); settings
  string_field::set memcpy'd an empty view's null `data()` (UBSan, 52bbcab).
- **Gates at HEAD (7870383)**: `asan-final-ctest-v2.log:275` and `tsan-final-ctest-v2.log:275` each
  print "100% tests passed out of 136"; normal gate `test-ctest-final.log:275` = 100% of 136; the
  independent reviewer re-ran all three at HEAD itself (exit 0 ×3) and binary mtimes postdate the
  newest source — the earlier stale-binary problem (committed final ASan log predated 95950a0) is
  fixed in this evidence. ui-bench interleaved A/B: 3/3 fresh quiet rounds at HEAD pass, S2 tick p50
  −1.0/+4.4/+3.2 % vs the stored 95372af baseline; the A/B's base-r1 FAIL (S2 +44.7 %) shifted EVERY
  stage multiplicatively at identical code = host artifact, disclosed and analyzed
  (ui-bench-AB-analysis.md, f47506a).
- **Gate NOT_READY — the one open item is seat evidence, escalated to the user per §1.2**: five
  serialized quiet-host app-smoke re-runs fail identically on the fixed HEAD **and** on the pre-fix
  0e0571c (app-smoke-analysis.md table: 69, 70, 71/101, 71, 70/102 PASS rows; first input-driven
  scenario A2 dies with `grab=-` though pointer motion demonstrably arrives; ~70 pure-protocol checks
  pass; one lumen-shell SEGV at log line 10 — notification_view::build_ui → ui::image →
  memory_arena::allocate, e.g. `app-smoke-fixround3-freshholder.log:10`), while the same-commit
  morning run passed 176/0/1 (`app-smoke.log:192`, the 1 = the known-environmental PSI WARN,
  [[lumen-app-smoke-known-noise]]). Same-commit A/B argues environmental, but the 95372af smoke leg
  was never run, so introduced-vs-environmental is not closable from this packet — that attribution
  (or a fix) is the user's call. The SEGV matches the open TODO row (`.zcode/TODO.md:74-83`);
  notification_view.cppm/text_node.cppm are diff-empty 95372af..HEAD, but the sweep DID edit
  memory_arena::allocate (relaxed-atomic offset_), so the "untouched cited lines" exemption does not
  cleanly apply. drag-smoke PASS (drag-smoke.log 05:43) predates the scene atomic (86b9c23 17:39) —
  no passing seat smoke exists at HEAD; a drag re-run is owed. Cleanup owed before merge: committed
  DBG stderr prints at `tests/test_protocol_conformance.cpp:307,311,387` (verified present this
  session; ~10,737 DBG lines per run per the review).
- **Packet corrections**: gate_runs.seat's "155 PASS rows ≥ 140; exit 1" matches no committed log
  (actual: 176/0/1 morning, 69–71 at HEAD); gate_runs.bench "not run" contradicts the ui-bench A/B
  evidence; "suite green after 1 fix round(s)" understates two rounds (sweep + review-fix). The
  committed prose (TRIAGE.md post-review section, app-smoke-analysis.md) is the accurate account.

**Host/toolchain findings**:
- libwayland 1.26: proxies survive `wl_display_disconnect` (leak-by-design contract) — client tests
  need create/destroy tracking; a `wl_proxy_destroy` on an already-freed proxy SEGVs inside
  uninstrumented libwayland and no public API distinguishes live from freed.
- This toolchain lowers nothrow `operator delete` to `free` while ASan intercepts nothrow new as a
  real NEW → every nothrow-new/delete pair reads as alloc-dealloc-mismatch; malloc/free is the
  workaround, and libstdc++'s own stable_sort buffer carries the same pair.
- hasvk driver-internal races are real but environmental-only after the protocol; TSan x hasvk had
  never been run here before this task.

**Disproven**: "protocol_conformance's leaks are untrackable libwayland magic" — the sweep SEGV was
cross-client tracking (a proxy of client B pushed onto A's list SEGVs at teardown), fixed and green;
"the task tree got slower" (base-r1 +44.7 %) — uniform host artifact at identical code; "the
app-smoke failures are the sweep's regression" — byte-identical failure on the pre-fix tree; "five
TSan tests are red on suppressible races because suppressions don't work" — CMake env overwrite, not
suppression semantics.

**Why:** a sanitizer sweep's real cost is in the environment chain, not the reports — CMake env
plumbing, suppression scoping, stale binaries and follow-on reports each hid genuine defects behind
plausible "tool noise" stories; and a red seat smoke with a same-commit green A/B is a hold for the
owner to adjudicate, never something to relabel environmental.

**How to apply:** run `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
(and the tsan equivalents) inside a task worktree — builds land in `build/<presetName>`, ~2–15x
slower, one suite at a time (TSan memory on a 15 GB host); extend tsan.supp/lsan.supp only by
third-party object name and run the TEST.md suppression protocol before trusting an entry; never add
per-test `set_tests_properties(… ENVIRONMENT …)` — append sanitizer env once after the last
add_test (aab2831); guard allocation-counting operators behind LUMEN_TEST_ALLOC_COUNTING (TSan
cannot link them); after fixing a failing test re-run until clean — follow-on reports are where the
real defects hide; keep client-test proxy tracking discipline (window_protocols is the reference).
At merge keep this main-checkout copy of the file (add/add vs the branch's 7870383 copy, which
predates the gate verdict). Related: [[lumen-snprintf-log-migration]] (base tree),
[[lumen-compositor-theme-env-export]] (the notification_view SEGV TODO row),
[[lumen-app-smoke-known-noise]], [[lumen-host-build-run-env]], [[lumen-memory-pressure-green]]
(PSI-row family), [[lumen-no-degradation-rule]].

**Seat regression resolved 2026-11-18 late evening** (the open item above): A/B on one
evening's seat — base 95372af 176/0/1 (only the environmental memory-pressure WARN row),
0e0571c 71/101 SEGV, bisect leg 573d7f3 (= 655ae13^) 166/0/4 no SEGV — pinned the trigger
to 655ae13 (itself a correct fix) flipping a pre-existing intermittent bug deterministic:
**retained text blocks re-strlen pointers into dead build_ui() frames** (the TODO's
notification_view SIGSEGV since 2026-11-16; notification_view was a downstream victim with
zero stack text buffers of its own). ASan shell on the seat (the TODO's prescription)
caught the origins one per launch: panel (655ae13), switcher letters (ecc162d), menu row
labels' std::string SSO (0bf30d4), launcher status line (920899c). After the four fixes:
ASan smoke SUAR=0; release app-smoke 176/0/1 (parity with the base in the same evening —
N8 idle-row noise discounted by rerun); drag-smoke PASS at HEAD; ctest 136/136. Evidence:
evidence/2026-11-18-sanitizer-sweep/seat-regression/. NEW pre-existing finding, recorded
as a TODO row: heap-use-after-free READ of size 8 at tray-menu close (stack lost to abort
+ truncation; menu/tray untouched by this branch). DBG scaffolding dropped (2698a85).

**Resolved 2026-11-24**: that tray-menu heap-use-after-free was captured (complete
runtime-written ASan report, seat repro 1/1), root-caused (teardown inside the menu's
own input delivery — NOT the retained-text class above) and fixed; 30/30 clean post-fix.
See [[lumen-tray-menu-uaf]].
