---
name: lumen-raise-the-bar-run
description: "2026-10-28 gated workflow on claude/raise-the-bar implementing the review's features (corners/shadows, springs, blur, HDR) and small items; user's chosen defaults"
metadata:
  node_type: memory
  type: project
  originSessionId: b66679e2-b37c-4f8d-a20e-a65f3602e64b
  modified: 2026-10-28T12:49:41.395Z
---

On 2026-10-28 the user asked to implement the "Features that would raise the bar" and "Smaller, cheap items" from the umbriel/scenefx review ([[lumen-umbriel-scenefx-review]]), under [[lumen-no-degradation-rule]].

User decisions (AskUserQuestion):
- Run it as a gated multi-agent workflow (run wf_77452688-839, branch claude/raise-the-bar from main 6296931, notes in session scratchpad wf2/notes.md).
- HDR: serve wp_color_manager_v1 + PQ/scRGB conversion with headless pixel tests; KMS HDR output stays unverified on this VM.
- Rounded corners and window animations ship OFF by default behind config.toml keys, so default pixels and latency stay identical.
- vkmark was installed via pacman for the benchmark item.

Item order: RLIMIT_NOFILE, GL reset recovery, Vulkan GPU timing, shader highp, test clock freeze, vkmark bench, then corners/shadows, spring animations, ext-background-effect-v1 blur, colour management.

Paused by the user 2026-10-28 (workflow stopped): baseline done; S1 RLIMIT_NOFILE reviewed KEEP (fd_limit::raise, test_fd_limit, 500 -> 1500+ clients at soft 1024); S2 GL reset recovery was mid-implementation, uncommitted in the tree with no notes yet (snapshot snap-s2-gl-reset-recovery.tree = tree after S1). Items S3-S6 and F1-F4 not started. Resumed the same day as run wf_94ba05f5-ac8 (script lumen-raise-the-bar-resume.js): branch reset onto origin/main 7ef5039 (Vulkan 1.3 floor, kernel features, tiling), S1 reapplied uncommitted (118/118 ctest), half-written S2 dropped (tree kept in wf2/partial-s2-abandoned.tree), fresh baseline, then S2..F4. Old notes moved to wf2/notes-pre-rebase.md; S1 also in `git stash` s1-before-rebase.

Outcome, merged 2026-10-29 as PR #63 (f42b085): KEPT S1 RLIMIT_NOFILE, S3 Vulkan GPU timestamps (debug-gated), S6 scripts/vkmark-bench.sh, F2 spring animations ([animations] enabled=false), F3 ext-background-effect-v1 blur (shell [panel] blur=false), F4 wp_color_manager_v1 v3 (SDR outputs only, no KMS HDR). REVERTED after review: S2 GL reset recovery (capture cursor lost after reset), F1 rounded corners/shadows (radius 30-32 masks client top corners; GL zeroed arrays per draw). ABANDONED: S4 highp (nothing at risk), S5 test clock (no user). Known cost: gamma22/PQ-tagged surfaces ~2x on SVGA3D GL, 20-30x on CPU.

**Why:** the user wants Lumen best-in-class ([[lumen-best-in-class-bar]]) without regressions.
**How to apply:** S2 and F1 came close (one more fix round each); if re-proposed, start from those review findings. Do not re-propose S4/S5 without a new reason.
