---
name: lumen-vulkan-capture-green-rounds
description: "Close-out chronology split from [[lumen-vulkan-capture-green]] — every dated re-run, merge-tree proof and gate re-certification, byte-preserved."
metadata:
  node_type: memory
  type: project
---

## Round 6 — the Z7 killer caught from failing runs and the chain closed; a NEW tip-level Z2–Z6 regression blocks the gate, 2026-11-15 (branch tip **959c9cd**; every log below tallied by the memory writer this session)

- **Item 1 — the killer caught (the prescribed plan failed, a cheaper one worked):** the
  z7-flake README's gdb `catch syscall close` could not confirm anything — its own
  caveat says tracer-slowed runs pass. An LD_PRELOAD close auditor (~50 ns/close:
  fd + caller return address + CLOCK_MONOTONIC into a ring, accept4 marked; kept out of
  the tree in /tmp/close-audit) ran on FAILING runs — Z7 failed 3/3 at the round's
  starting head, so the exposure window stayed open — and caught the chain with
  line-level attribution. The kernel-consumption theory is disproven from mainline
  source: `drm_atomic_plane_set_property` handles IN_FENCE_FD via
  `sync_file_get_fence`, which only takes a fence reference — **the kernel never
  closes an IN_FENCE_FD**; a truthful "kernel consumed it" report would be
  constant-false, voiding the packet's prescribed reporting mechanism.
- **The proven chain (z7-fixed/README "The catch"):** `take_sample_fence(!wanted)`
  consumed the sample-fence fd and then returned its dead number
  (modules/lx.compositor/capture_engine.cpp:415-430 — fixed to return -1, f7640f9);
  the capture drain published the stale number into the release-fence mailbox; the
  mailbox's replace-close of that number landed on whatever had reused it — the
  portal's fresh ui connection (accept4, killed 68–71 ms later through the
  publish_release_fence tail call) and, on the older build, the pipeline's own
  composited_fence (the EBADF+EINVAL signature). The F_GETFD probe in
  drop_consumed_fence could not tell a consumed fence from a reused number — deleted,
  only an explanatory comment remains (compositor_impl.cpp:11514;
  forget_consumed_fence :223 answers the layer's word at :11504 and :11647).
- **The fix, three layers (b6b1f55 + f7640f9) — no fence fd crosses a frame:** (1)
  lx.drm's commit owns the in-fence: on success it closes the fd and reports
  consumption by clearing `request.in_fence_fd`; on failure it leaves the fd open for
  the caller's retry (modules/lx.drm/lx.drm.cppm close+clear, verified; result-typed,
  no fd-number guessing above lx.drm, per the scope note). (2) the release-fence
  mailbox holds the newest fence as a persistent DRM syncobj — import-on-publish,
  `drmSyncobjTransfer` to release points. (3) `take_sample_fence(!wanted)` returns -1.
- **Item 2 — seat evidence at f7640f9 with the REL holder live (pid 2692863, event16),
  committed under evidence/2026-11-15-vulkan-capture/z7-fixed/:** three consecutive
  `--only Z` streaks **10 PASS / 0 FAIL each** (Z1–Z8 + 2 run rows) with **0
  `release-fence import refused` lines** (the packet reports the pre-fix streaks at
  117–119/run — not re-verified by me); the full app-smoke **176 PASS / 1 FAIL /
  0 WARN**, the FAIL the owner-documented PSI tmpfs clean-log row, all 15 flip rows +
  Y4 dmabuf + Z1–Z8 PASS (16/16 flip-row lines counted); drag-smoke `VERDICT: PASS
  (warnings: g latency)` — the known host-noise family
  ([[lumen-toplevel-drag-compositor]]).
- **New tests (all present, verified by grep):**
  `release_fence_mailbox_keeps_the_newest_as_a_syncobj`
  (tests/test_content_update.cpp:356), the in-fence ownership rounds
  (tests/test_kms_vkms.cpp:182 — this host's vkms lacks DRIVER_SYNCOBJ, so only the
  failure half runs on the seat; the success half is proven by the 60 fps seat runs),
  `vk_take_sample_fence_never_hands_on_a_closed_fd` (tests/test_vk_capture.cpp:643).
- **Review: 4 findings confirmed, 0 refuted, all fixed by f073a24** (content per the
  review report; the resulting tree state verified by grep): 1ce36d8 — the DPMS
  black-frame path double-closed the in-fence the KMS layer now consumes; the caller
  closes only when `!committed && fence >= 0`. dcf001e — an LRU-evicted capture
  dma-buf freed its VkMemory/VkBuffer without waiting in-flight copies;
  `wait_capture_copies()` now runs before the free (also in capture_dmabuf_forget).
  bd9043f — the borrowed-fd comment replaced by the real contract
  (compositor_impl.cpp:11421). f073a24 — the z7-fixed README states 3a4441e was not
  evidence-only (it added the errno arg to the `release-fence import refused` debug
  line in modules/lx.gfx/drm_timeline.cpp), so the certified code is exactly
  f7640f9; history untouched. Review re-ran at f073a24: ctest 132/134 tolerated pair,
  capture ctest set 6/6, test_vk_capture 13 tests 0 failed (per the review report,
  not re-run by me).
- **Gate verdict NOT_READY on a NEW regression — the §1.2 stop condition, live:** the
  gate-time full app-smoke at tip 959c9cd (evidence/smokes/gate-workflow/app-smoke-logs/
  run.log, 15:17–15:27, holder event16 alive) is **171 PASS / 6 FAIL / 0 WARN** with
  **Z2–Z6 all FAIL `response None`** — the portal's interactive frontend never
  answered (Z1/Z7/Z8 and all 15 flip rows PASS) — against the same-morning f7640f9
  run's 176/1/0: same harness, same host, same holder, **5 previously-green rows
  regressed**, and the committed logs do not determine the root cause (0 EBADF,
  0 import-refused, no client-kill line; the keyboard reached the chooser). The only
  code between is 3a4441e (log string), 1ce36d8 (black-frame close fix), dcf001e
  (capture eviction wait). Per §1.2 the A/B evidence goes to the user, not accepted.
  The tip-level drag-smoke did PASS (gate-workflow/drag-smoke-logs/run.log).
- **Packet misstatement, for the record:** gate_runs.seat's "app-smoke → PASS at host
  tolerance (152 PASS rows ≥ 140)" — no committed run has 152 PASS rows (176 and 171
  verified by me here); the number is the workflow step counting `^PASS` over the
  captured process output (lumen-development-studio.dwf.ts:672-684 — the count spans
  stderr+stdout of the run; the 65536-char cap the gate names sits upstream of that
  file, not verified by me), and the tolerance's premise ("the known host-calibrated
  set") is false for Z2–Z6, green the same morning.
- **Evidence hygiene at close:** the worktree is dirty —
  `evidence/smokes/gate/precheck-app-smoke.txt` modified and `precheck-drag-smoke.txt`
  untracked (the gate-time precheck still leaks into the round-5 acceptance
  namespace — [[lumen-smoke-bench-harness-green]]'s owed cleanup, still owed), and
  the 15:07/15:17 prechecks record no injection-device line unlike every certified
  run's. Gate-named remedy: diagnose the Z2–Z6 `response None` at final head (portal
  frontend vs the three intervening commits), then re-deliver ITEM 2's battery at the
  true final head — full app-smoke with Z1–Z8 and the 15 flip rows, Z-streaks per the
  card, drag-smoke — committed with prechecks showing the injection device.

## Round 7 — the Z2–Z6 `response None` "regression" was never code: a relative build path dangled the portal symlinks; battery re-certified at the true final head; gate READY, 2026-11-15 evening (branch tip **f84e2dd**, unmerged; checks marked "ran" were executed by the memory writer this session)

- **Independent review of the round-6 close: 7 findings confirmed, 0 refuted** — ITEM A
  undelivered; the cursor-dmabuf worker probe; the undisclosed EINVAL present freeze;
  evidence purity (3a4441e 'evidence:' carried a code hunk); battery not at the true
  head + prechecks without the injection line; a stray git-log line pasted into a
  committed precheck; `-1` SYNC_FD exports treated as errors. The fix round's total code
  delta 3f542b5..f3ab935 is exactly **5 files +155/−5** (git diff --stat, ran).
- **ITEM A root cause — harness, not code (f3ab935):** every Z2–Z6-failing run (round
  3's 01:11/01:23, the round-6 tip battery 171/6) invoked app-smoke with a **relative
  LUMEN_BUILD_DIR**; `portal_session_start` symlinks `lumen.portal`/`lumen-portals.conf`
  into `/run/user/1000/lumen-portals-<socket>/`, and a symlink's target resolves against
  the *directory holding the link*, not the invoking cwd → both dangled → the frontend
  journal read "Error loading .../lumen.portal: No such file or directory" →
  xdg-desktop-portal never learned the Lumen backend serves Screenshot/PickColor → every
  frontend-routed row answered `response None` while backend-direct Z1/Z7/Z8 passed (the
  backend served fine all along, portal-lumen.log). The card's three suspect commits
  (3a4441e/1ce36d8/dcf001e) were innocent — 14:23's 176/1/0 vs 15:17's 171/6 differed
  only in invocation shape. Fix: absolutize `$build` at function entry
  (scripts/portal-session.sh:69-76). **Proof under the original failing shape (ran:
  recount of final-head/z-relative-logs/run.log):** `--only Z` with
  `LUMEN_BUILD_DIR=build/test` relative → 10 rows / 0 FAIL / **VERDICT: PASS**;
  portal-z2.log answers `response 0` where the old frontends logged "No such interface".
- **Cursor-dmabuf probe (970b483):** `worker_read_vk_dmabuf`'s landed probe called
  `read_back_wait`, which requires `rb.mapped` — a row no dmabuf session ever maps — so
  every cursor-painting dmabuf capture frame reported failed and no fence was waited
  (capture_engine.cpp:328-332; capture_dmabuf_copy never maps). New
  `vulkan_compositor::read_back_wait_fence(slot, timeout)` (vk_renderer.cppm) checks
  only submitted/fence; engine test `vk_engine_dmabuf_job_with_cursor_lands` added.
  **Ran: `./build/test/test_vk_capture` → 14 tests, 0 failed.**
- **The EINVAL present wedge, disclosed then bounded (5798c32):** the round-6 battery's
  compositor.log:28730 (f7640f9 09:13:14) held one
  `drmModeAtomicCommit rejected: invalid state (EINVAL)` → 131 `commit=failed` frames +
  8 consecutive "no present in 250ms" warnings (~2 s freeze) that round 6 misattributed
  to the PSI clean-log row (correction in c19faf5's README); the family does not exist
  in the pre-branch round-4 battery. Fix: `drm_rejected_streak` counts EINVAL-class
  rejections only (EBUSY/EACCES reset), at `k_rejected_recovery=10` forces
  `prepare_resume()` guarded by `!flip_pending()`; one error per streak names
  fb/fence/cursor/damage/flags. Non-reproduction at this head: ~15 000 debug commits
  across 5 runs, grep 0 everywhere; the kernel-side trigger stays unproven (kernel log
  silent).
- **-1 SYNC_FD exports (b23bc6d):** the three capture sites errored on the legal -1
  `vkGetSemaphoreFdKHR` return (semaphore already signalled = copy completed); now only
  a failed call errors, -1 passes through as the documented completed outcome.
- **ITEM B via the authorized history rewrite:** 3a4441e no longer exists — 78fc0e2
  carries the drm_timeline errno change as a code commit, 8d065aa is evidence-only
  (ran: `git show --name-only --format='' 8d065aa | grep -v '^evidence/'` → 0 lines);
  hashes above f7640f9 differ from the morning records and memory notes.
- **Battery at the true final head (c19faf5; code frozen at f3ab935 — ran:
  `git diff f3ab935..HEAD -- modules/ scripts/ tests/ executables/` → empty):** recount
  of app-smoke-logs/run.log: **176 PASS / 0 WARN / 1 FAIL**, the FAIL the PSI tmpfs
  clean-log row; **Z1–Z8 all PASS, all 15 flip rows + N8 PASS** (each grepped). First
  attempt 175/0/2 with an N8 idle flake — rerun-proven PASS (n8-rerun-run.log,
  VERDICT PASS) and disclosed in the README, not hidden. Streak shape, disclosed: four
  consecutive full-Z-passing runs on head code (z-relative 19:21, first battery ~19:30,
  kept battery 19:38–19:45, gate-time 20:18) but not three dedicated back-to-back
  `--only Z` runs — z-streak-final1/2/3 stays certified at f7640f9 only. drag-smoke
  `VERDICT: PASS` (g latency p95 47.6 ms over the 37.3 ms bar, the known family).
  Prechecks 19:21:08 / 19:22:22 / 19:47:44 + postcheck 19:48:23 each carry
  `injection: pointer /dev/input/event16 ('Lumen Inject Mouse', pid 2692863), relative
  event16, keyboard event3` (read in full); the kept battery is bracketed by the 19:22
  and 19:47 ones, same holder pid throughout.
- **Gate-time corroboration (f84e2dd, own gate-workflow namespace):** recount 175 ^PASS
  / 2 ^FAIL — the N8 idle flake (rerun-proven) and the PSI row; drag PASS. Suite:
  recorded run 19:58 (postdates the last code commit f3ab935 19:05), LastTestsFailed =
  {17:ui_gallery, 82:test_task_model} — the base-known pair (review ran ctest 132/134
  in 77.0 s; not re-run by me — a ctest run rewrites the cited LastTest.log). Round 6's
  owed `evidence/smokes/gate/` precheck leak is gone (ran: `git status --porcelain
  evidence/smokes/gate/` → 0 lines).
- **Gate verdict: READY.** No §1.2 regression: the battery improved over base
  (158/15/4 → 176/0/1), the suite is unchanged, the P1 presentation-timing row passes
  in-battery, no new config keys/env vars/Vk types above lx.gfx in the fix round. Tip
  f84e2dd **unmerged** (ran: `git merge-base --is-ancestor f84e2dd main` → not an
  ancestor). The packet's "155 PASS ≥ 140, exit 1" seat row is the truncated-^PASS
  workflow artifact again — true counts are 175/2 and 176/1, recounted here.

**Disproven / retracted (do not re-chase):** "Z7's residual is the portal's connection
churn" — disproven by strace (the compositor closes the fd); the portal is the victim.
"Z7 is undiagnosed timing luck" — closed in round 6: the killer is
`take_sample_fence(!wanted)`'s dead-fd handoff amplified by the release mailbox's
replace-close, caught with line-level attribution by an LD_PRELOAD auditor on failing
runs (the planned symbolized backtrace was never needed). "The kernel consumes
IN_FENCE_FD (so the KMS layer can report consumption)" — disproven from mainline
drm_atomic_uapi.c: sync_file_get_fence takes a reference only; ownership had to move
into the commit layer itself. "The WARNs were row-hardcoded and would flip by relabel"
— they were capability-derived and flipped because the capability now exists. "The
tip-level Z2–Z6 `response None` is a code regression across 3a4441e/1ce36d8/dcf001e"
— disproven in round 7: the differing variable was the invocation shape (relative vs
absolute LUMEN_BUILD_DIR), a harness symlink bug; the three commits were innocent and
no bisect was ever needed.

**Why:** closing a backend-limited surface area is product work measured in *what real
clients can now do* — but a flaky acceptance row is not a pass, and a diagnosis that
names the wrong component (portal vs compositor) sends the next session down the same
wrong path; the strace evidence, not intuition, settled it, and round 6 finished it
only by abandoning both parts of the prescribed plan: the mechanism ("the kernel
consumes the in-fence") was disproven from mainline source before implementing, and
the tool (gdb `catch syscall close`) could not observe the failure at all — a ~50 ns
LD_PRELOAD auditor running on failing runs caught in one evening what a week of
tracer-based inference had not. Round 7's "5 previously-green rows regressed" §1.2
stop condition dissolved the same way: the suspect diff was innocent and the real
variable was the run's invocation shape — a §1.2 A/B is only meaningful between runs
that differ in code, so diff the *invocation* (cwd, env, absolute vs relative paths,
symlink targets) before bisecting commits. Evidence validity is provisioning-bound
(the holder-less run produced garbage), and evidence *summaries* are not evidence
either: the workflow's `^PASS` count over truncated captured output reported "152
PASS rows" that no committed log contains — only the committed logs, tallied, are the
record.

**How to apply:** the fence contract as of b6b1f55/f7640f9 — lx.drm's commit owns the
in-fence (close on success and clear `request.in_fence_fd`; a failed commit hands the
fd back untouched), no fence fd crosses a frame (release-fence mailbox = one
persistent DRM syncobj, `drmSyncobjTransfer` to release points),
`take_sample_fence(!wanted)` returns -1. Never re-add a caller-side close of an
in-fence/composited_fence after a successful commit — that is the Z7 bug class twice
over (round-5 killer, 58ff211's black-frame double-close). Never write an engine
"did it land" probe that assumes a shm session's mapped rows — use
`read_back_wait_fence` for dmabuf sessions (970b483). Portal harness rule: paths fed
to `portal_session_start` must be absolute — its runtime-dir symlinks resolve against
the link's own directory, so a relative `LUMEN_BUILD_DIR` silently disconnects
xdg-desktop-portal from the Lumen backend while the backend itself stays healthy
(diagnose from the frontend journal's "Error loading .../lumen.portal", not from the
compositor). A persistent EINVAL-class KMS rejection now self-recovers via a full
modeset after 10 consecutive failures (5798c32) — do not remove the guard or the
per-streak error naming. Evidence hygiene: an 'evidence:' commit carries only
`evidence/` paths (verify: `git show --name-only --format='' <sha> | grep -v
'^evidence/'`); a code hunk landing with evidence gets split by history rewrite while
nothing is pushed, not documented. The capabilities log line is grep-stable
(`compositor.capture:capabilities read-back=…`) — scripts key on it. Do not call the
O6 ±18 tolerance a regression; it is a measured gamma-vs-linear blend delta. GL/CPU
no-regression claims must cite
test_gl_capture/test_capture/test_capture_protocols until GL seat smokes actually run
on the VM. Read-back latency is quoted as `./build/test/test_vk_capture` at
3200x1800 (~3.3 ms median on hasvk). Tally smoke evidence from the committed logs —
never from a workflow step's row counts. State at close: tip f84e2dd gate-READY,
unmerged — the user decides push.

Related: [[lumen-smoke-bench-harness-green]] (the 15-WARN contract this closes, the
gate-workflow namespace, the uinput-holder provisioning rule), [[lumen-notebook-hw]]
(hasvk host), [[lumen-app-smoke-known-noise]] (PSI tmpfs row), [[lumen-no-degradation-rule]],
[[lumen-best-in-class-bar]] (the read-back comparison), [[lumen-unit-pair-green]]
(tolerated-pair expectations).
