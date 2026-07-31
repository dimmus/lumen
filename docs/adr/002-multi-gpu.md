> **Status:** Current (v0.3) — see code for implementation truth

# ADR 002 — Multi-GPU and PRIME Offload

**Status:** Accepted  
**Date:** 2026-07-30

## Context

Linux desktops frequently ship with hybrid graphics (integrated + discrete). Lumen must assign
outputs to the correct KMS device, composite on the GPU that owns client buffers when possible,
and migrate dmabufs when scanout and client GPUs differ.

## Decision

- `lx::gfx::device_selector::probe_tier()` detects dual render nodes (`renderD128` + `renderD129`
  or `card1`) and selects **discrete** tier when a secondary GPU is present.
- `output_manager` maps each `lx::output_id` to a render node; hotplug refresh rebuilds the map.
- Cross-device presentation uses **dmabuf re-export + Vulkan re-import** via `import_cache` eviction
  and re-acquire — no silent SHM fallback in production paths.
- Present mode follows tier: discrete → `mailbox`, integrated → `fifo_relaxed`.

## Consequences

- Compositor may hold two Vulkan devices in future; current code probes tier and documents the
  migration contract before dual-device instances land.
- Shell reads output layout via **wlr-output-management-v1**, not `zlm_shell_v1`.

See [15-dual-gpu.mmd](../uml/15-dual-gpu.mmd).
