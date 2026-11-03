---
name: lumen-ui-bench-noise-and-g7
description: "2026-10-28 audit of ADR-010 Phases 0–4 \"regressions\": all host drift; ui-bench gate moved to p50; GL build-ahead for G7 measured and rejected"
metadata:
  node_type: memory
  type: project
  originSessionId: 06076445-a05e-4f4c-9024-6e431e8af343
  modified: 2026-10-28T17:06:47.722Z
---

On 2026-10-28 (branch `claude/ui-regressions`, merged as dimmus/lumen#60, 846b6ad) the open items of ADR-010 Phases 0–4 were checked:

- **Bench "regressions" are host drift, not code.** An interleaved A/B of Phase 3's `lumen-ui-bench` (ae65349) against main gave the same p50 on every S1–S3 stage. S3 `update` (pure xorshift, unchanged code) read 69.7 µs in the Phase 3 baseline and 79–83 µs in both binaries that day. One binary's p95 moves 40 %+ run to run, and its p50 up to 14–18 %.
- `scripts/ui-bench.sh` headless gate changed: FAIL on p50 +25 %, WARN on p50 +10 % or p95 +25 % (it was FAIL on p95 +10 %). `iritur-none.json` was rebaselined.
- **G7 GL cold**: the cost is the driver (EGL ≈27 ms, two Mesa compiles ≈60 ms, fontconfig ≈16 ms on a first run); Lumen's own code is ≈5 ms. Building the programs ahead on a helper thread in a shared EGL context was measured and LOST: +12 ms with the client pinned to one CPU, and warm +15–20 ms unpinned, because the compositor is busy on the other core. Don't retry it.
- `test_dmabuf_composite`'s 8 VUID-vkDestroyImage-image-01000 errors were test teardown order (render target destroyed before the compositor waited). Fixed.
- G2 p95 / G3 / G11's frame rule: host wake jitter, as the ADR says. G11 and all client gates passed on a quiet host.
- Wayland clipboard implemented and merged as dimmus/lumen#61 (1f924e4): `lx.wayland.client:data` + `lx.app:clipboard` behind `lx::ui::clipboard`; eager read of other clients' selections, per-process marker mime, tested via ext-data-control in test_app_window. Primary selection (middle-click paste) and text drag and drop merged as dimmus/lumen#62 (9f75f7c) on the same service (`transfer_host`, `drop_target`, `drag_service`). Not done: non-text drags, drag icon, `ask` action; not tried with a real app on the display.

**Why:** future sessions would otherwise chase the same false regressions or retry the build-ahead.
**How to apply:** judge UI perf changes by an interleaved A/B (same pin, `setarch -R`, ≥3 rounds), not by a single run against the stored baseline. Never measure while another session runs app-smoke or builds (check `/proc/loadavg` and `pgrep smoke`). Related: [[lumen-gpu-ui-engine-plan]], [[lumen-no-degradation-rule]], [[lumen-host-build-run-env]].
