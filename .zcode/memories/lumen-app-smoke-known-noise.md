---
name: lumen-app-smoke-known-noise
description: app-smoke groups that fail on unmodified main on this host (Z2–Z6 portal frontend) or flake (T1/T3 timing); check a baseline before blaming a change
metadata:
  node_type: memory
  type: project
  originSessionId: 04e71aee-9694-428b-9a54-da5aa0caad1c
  modified: 2026-10-28T15:07:54.083Z
---

Measured 2026-10-28 against a clean build of main 6296931 on the vmwgfx VM:
- `app-smoke --only Z`: Z2–Z6 FAIL on main too ("No such interface org.freedesktop.portal.Screenshot" from the xdg-desktop-portal frontend; Z1/Z7/Z8 straight to the backend pass). Environmental, not a regression.
- `app-smoke --only T`: T1 (vkcube rate) and T3 (FIFO dt) flake on both main and branches (SVGA3D present timing); alternate runs before calling it a regression.
- Other sessions run app-smoke/drag-smoke on the same console; a concurrent run makes the second compositor come up without DRM master and every scenario FAIL. Check `pgrep -x lumen-composito` and `pgrep -f "^bash .*-smoke\.sh"` first (a plain `pgrep -f smoke.sh` matches your own shell).

Update 2026-10-29: on main f42b085 Z2–Z6 PASS (177/177 in two runs), so they are no longer known-broken; the new environmental failure is run:(clean log) from '[WARN] compositor.memory: critical pressure' when /tmp tmpfs and other sessions push PSI, reproducible on base binaries. Another session's lumen-vkmark-bench compositor can hold DRM master.

**Why:** a session spent a baseline build proving these were not caused by the tiling change.
**How to apply:** treat Z2–Z6 as known-broken and T as flaky until fixed; compare with a baseline when they fail. Related: [[lumen-host-build-run-env]].
