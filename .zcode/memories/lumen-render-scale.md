---
name: lumen-render-scale
description: "HiDPI supersampling (render_scale, umbriel PR 573 port) + area downsample filter, worktree claude/render-scale 2026-10-29, uncommitted; open question on filter cost"
metadata:
  node_type: memory
  type: project
  originSessionId: 254c94fe-2f41-43b7-ac6a-3b35c74ccd54
  modified: 2026-10-29T17:07:48.185Z
---

2026-10-29: render_scale implemented in worktree `.claude/worktrees/render-scale` (branch claude/render-scale off main 65802e1), merged as PR #70 (merge 4f6cc0e). config.toml `[[output]] name/render-scale` (live reload), `--render-scale`, `--output-render-scale NAME=S`, ListOutputs `render_scale`. Area (box) downsample filter on Vulkan/GL/CPU via `gfx::downsample_footprint_for`; exact 2:1 skips it. Tests: test_render_scale, test_downsample, test_output_membership; ctest 120/120.

Decided 2026-10-29: the user said "Keep area filter" on every downscaled draw. The filter costs more at non-integer ratios (GL SVGA3D fullscreen 4:3 ~4 → ~10 ms; CPU ~3x) and also applies to integer clients on fractional outputs (pre-existing path). Measured quality gain: RMS vs area 10–83 → <0.5.

**How to apply:** if asked to commit/PR, use that worktree. Do not re-raise the filter's GL/CPU cost at non-integer ratios as a regression; it is an accepted trade for quality. Related: [[lumen-no-degradation-rule]], [[lumen-umbriel-scenefx-review]].
