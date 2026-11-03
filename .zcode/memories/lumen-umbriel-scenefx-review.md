---
name: lumen-umbriel-scenefx-review
description: 2026-10-27 study of umbriel (noctalia) and scenefx for Lumen speed/memory ideas; 2026-10-28 outcome of implementing items 1-5 on claude/render-efficiency
metadata:
  node_type: memory
  type: project
  originSessionId: b66679e2-b37c-4f8d-a20e-a65f3602e64b
  modified: 2026-10-28T10:08:20.390Z
---

On 2026-10-27 the user asked what Lumen could take from github.com/noctalia-dev/umbriel (C++23 wlroots compositor, in-tree scenefx fork "umbrielfx") and github.com/wlrfx/scenefx (wlr_scene + GLES FX renderer). Neither is usable as a dependency (wlroots/GLES2-only); value is in techniques.

Outcome 2026-10-28 (workflow, merged to main as PR #55, 2026-10-28):
- KEPT 2: Vulkan linear FP16 intermediate kept per target size (up to 4, 600-frame idle free) — no realloc+GPU drain when outputs differ in size.
- KEPT 1: Vulkan composite + encode damage-limited (persistent intermediate holds newest frame) — cursor frame ~123 → 1.7 ms on lavapipe.
- KEPT 4: partial opaque regions (2 largest rects) seed occlusion; Vulkan no-blend pipeline for blend_mode::opaque.
- KEPT 5: partly covered draws clipped to ≤4 visible rects (cut only if ≥¼ of width hidden) — GL on SVGA3D 25-36% faster on overlapping-window scenes.
- REJECTED 3: SDR into an _SRGB view of scanout — 8-bit storage between blends is off by up to 4 levels vs FP16; do not re-propose.
- Also fixed in PR #55: Vulkan XRGB dmabuf alpha-ONE swizzle; app-smoke title regexes (now 176/176). Still open: GL has no reset recovery; no RLIMIT_NOFILE raise.

**Why:** user wants Lumen best-in-class ([[lumen-best-in-class-bar]]) under [[lumen-no-degradation-rule]]; malloc replacement already ruled out ([[lumen-malloc-replacement-measured]]).
**How to apply:** check the branch/main before re-proposing these. app-smoke's "host present stalls" WARN swings 0 → 84 between runs of the same binary on this SVGA3D VM, so rerun before calling it a regression ([[lumen-host-build-run-env]]).
