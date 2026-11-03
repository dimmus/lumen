---
name: lumen-best-in-class-bar
description: User wants every Lumen design decision to be state of the art and best in class, with maximum efficiency, not the simplest thing that works
metadata:
  type: feedback
---

For Lumen work, choose the best available approach by current standards and maximum efficiency, not the minimal or simplest working one. Compare against wlroots/sway, KWin, Mutter, Smithay/niri and Hyprland, choose deliberately, and say where Lumen matches or goes further. Back efficiency claims with measurement (latency, allocations, damage), not assertion. When the best option is impossible on the host or far out of scope, state the trade-off explicitly instead of silently settling.

**Why:** Said on 2026-10-14 while the interactive move/resize workflow was starting: "all decisions should be best on current technological level and absolutely best in class and efficiency". This matches the project's own Vulkan-first, zero-copy, deadline-scheduled ambitions in AGENTS.md and ADR-004/005.

**How to apply:** Put this bar explicitly into designs and into every subagent or workflow prompt. Example from that session: hardware KMS cursor plane with software fallback, wp_cursor_shape_v1, frame-perfect throttled resize, motion coalesced per frame, measured pointer-to-photon latency. Related: [[lumen-host-build-run-env]].
