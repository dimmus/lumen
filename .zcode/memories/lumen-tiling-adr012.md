---
name: lumen-tiling-adr012
description: Master-stack tiling per workspace shipped in PR #58 (merged 2026-10-28, 7ef5039); compositor-side, plugin scaffold removed, all 11 default bindings kept
metadata:
  type: project
---

The user chose a dwm-style master-stack layout, compositor-side, per workspace (over an i3 tree or a niri strip, and over shell-side policy). Merged as PR #58 (merge 7ef5039) on 2026-10-28; design in docs/adr/012-master-stack-tiling.md. Super+T toggles; `[tiling]` has layout/master-ratio/master-count/gaps. Not built yet (listed in the ADR as trade-offs): pointer-drag split resize, orientations, monocle/dwindle. The user decided (2026-10-28) to keep all 11 default layout bindings, even though they take their Super keys on floating workspaces too.

**Why:** avoids re-asking which tiling model, and records that the default bindings were settled.
**How to apply:** extend `lx.compositor:tiling` / `window_manager::flush_tiling` for tiling work; never reintroduce a shell-side tiling plugin. Related: [[lumen-app-smoke-known-noise]].
