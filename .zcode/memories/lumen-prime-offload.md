---
name: lumen-prime-offload
description: "PrefersNonDefaultGPU / discrete-GPU launch in lumen-shell (2026-10-29): merged PR #71; the notebook check is still open"
metadata:
  node_type: memory
  type: project
  originSessionId: adff2b51-911e-45e5-b5ed-8b6a69ce9144
  modified: 2026-10-29T17:36:40.103Z
---

On 2026-10-29, lumen-shell's launcher started honouring `PrefersNonDefaultGPU` and `X-KDE-RunOnDiscreteGpu`, and launcher rows got a right-click / Menu / Shift+F10 item named "Launch using Discrete/Integrated Graphics Card". Merged to main as PR #71 (da5712e) on 2026-10-29, after merging main in; ctest 120/120 on the merged tree.

- The variables come from switcheroo-control's GPUs `Environment` (system bus, only opened with ≥2 render nodes in /dev/dri). Without it they fall back to `DRI_PRIME=1`. They go into the app's systemd unit `Environment`.
- The code is in `:app_launch` (`app_launcher::probe_gpus`, `choose_offload_env`). The menu is `menu_kind::launcher` in shell_desktop.cpp.
- `lx.shell.cppm` uses CRLF line endings. Python or other text rewrites convert them to LF, so restore CRLF after any scripted edit.

**Why:** the user still has to check on the HP notebook ([[lumen-notebook-hw]]) that such an app reports the GTX 950M; this VM has one GPU, and switcheroo-control is not installed on it.
**How to apply:** if notebook logs come back, look for `shell.apps` lines "apps: N GPUs: apps that prefer the non-default one get …".
