---
name: lumen-ui-phase5
description: ADR-010 Phase 5 (shell adoption + deletion) started 2026-10-29 by workflows on branch claude/ui-phase5; user's design decisions and where the plan lives
metadata:
  type: project
---

Phase 5 of ADR-010 started 2026-10-29 on branch `claude/ui-phase5` in worktree `.claude/worktrees/ui-regressions` (base main f42b085). The design workflow wf_e10e9a78-398 produced a 10-stage plan (P5-1 deletions … P5-10 docs/§8.6), saved in the session scratchpad as p5-final.json.

User decisions (2026-10-29):
- New layer-5 module `lx.ui.host` (per-thread context + role-agnostic surfaces: toplevel, layer, popup, subsurface); lx.app, lx.shell and lx.portal build on it; lx.app gets multi-window.
- Shell memory: accepted up to G8 (25 MB anon). Today's shell is 2.4 MB anon; the cpu tier is projected at 7–10 MB, a GL client is ≈27 MB on this host. First-panel time (14.1 ms p50 today) and panel redraw CPU (314 µs p50 today) must not get worse.
- Decorations on the lowering path (GLES 2, Vulkan without ui_vk): extend `lower_to_blits` with coverage-texture blits in both renderers rather than keep the deco atlas.
- §8.6 comparison: egui may be built from crates.io; pacman installs of waybar/ironbar/gtk4-layer-shell/layer-shell-qt approved.

**Why:** these were the plan's open owner decisions; a later session continuing Phase 5 should not ask again.
**How to apply:** keep them unless the user changes them; any regression found still stops and goes to the user ([[lumen-no-degradation-rule]]). Related: [[lumen-gpu-ui-engine-plan]].
