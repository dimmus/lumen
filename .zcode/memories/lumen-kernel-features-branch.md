---
name: lumen-kernel-features-branch
description: "Branch claude/kernel-features (2026-10-28) — PSI pressure, fence deadlines, SO_PEERPIDFD, DRM fdinfo GPU memory, CLOSEFB exit hand-over (thread scheduling measured worse, reverted); merged to main b4550d3"
metadata:
  node_type: memory
  type: project
  originSessionId: a6815e3f-842a-4d0e-ab37-0ebf35aad1d9
  modified: 2026-10-28T04:39:10.273Z
---

On 2026-10-28 the user asked to implement six newer-kernel items with workflows. Branch `claude/kernel-features`, worktree `.claude/worktrees/kernel-features`; commits 31d3e57..cf7b8ad, main (812ad52, PR #54) merged in. 116/116 tests and tty-smoke pass. Merged to main 2026-10-28 as b4550d3 (pushed directly at the user's request, no PR; main from PR #55 merged in first, that last merge not built or tested at the user's request).

- PSI: `lx.runtime:psi` + `psi_monitor`. Only a 2 s window is accepted unprivileged on 7.2. Falls back to /proc/pressure/memory from a tty.
- Fence deadlines: `lx.gfx:fence_deadline`. vmwgfx accepts the ioctl and ignores it, so there is no gain on this host.
- Scheduling (e674bc9): REVERTED in cf7b8ad. drag-smoke under 4 busy loops: cursor off 9.2/25.5 ms vs auto 10.9/35.2 ms median/p95 (500 us EEVDF slice cuts up the GL composite). The user chose to keep it reverted until it can be measured on hardware Vulkan; the RT/rtkit tiers were never measurable here.
- SO_PEERPIDFD: the sandbox and shell checks now fail closed for zombie or exited peers.
- fdinfo: vmwgfx prints no usage stats, so the health report shows "not reported" here.
- CLOSEFB: the kernel does NOT restore fbcon at lastclose on 7.2/vmwgfx. Lumen restores the saved CRTC itself; checked that the console comes back.

**Why:** a future session may be asked to open the PR, measure gains, or rebase onto a moved main.
**How to apply:** the merge with main needed `drm_timeline_ops::query` callers updated with the extra deadline argument; watch for that kind of semantic conflict again. Push via SSH (see [[lumen-gpu-ui-engine-plan]]).
