---
name: lumen-host-build-run-env
description: "How to build and run Lumen on this VM host (vmwgfx, lavapipe-only Vulkan, GL path, running \"as tty1\" from SSH, input injection without uinput)"
metadata:
  node_type: memory
  type: project
  originSessionId: fdb74e89-ff71-4d92-bec2-0594143afd63
  modified: 2026-10-14T11:25:12.835Z
---

Verified 2026-10-14 on this host (kernel "fastvm", VMware SVGA / vmwgfx, 2 cores, 15 GB RAM, Arch, clang 23, cmake 4.4, ccache present):

- A cold build needs `./scripts/fetch-protocols.sh` first (needs network; `protocols/upstream/` is gitignored).
- `cmake --preset release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` then `cmake --build --preset release`. Compositor plus demo clients is ~526 ninja steps, ~15 min on 2 cores. The user keeps a release build in the main checkout at /home/dimmus/lumen/build/release.
- Vulkan: user installed vulkan-headers, vulkan-swrast (lavapipe) and shaderc on 2026-10-14. The compositor sees "llvmpipe" as a software device and still picks the GL path ("GL composite path active (EGL/GLES on GBM)", renderer "SVGA3D"), which is the intended behaviour. No hardware Vulkan exists for vmwgfx.
- Running from an SSH/pts session works: /dev/dri/card0 has a logind ACL for dimmus (owner of the active tty1 session), so the direct-open fallback gets DRM master. Input devices have no ACL, so libinput needs logind TakeDevice.
- To run "as tty1" from SSH: move the shell into the tty1 logind session's cgroup, then logind TakeControl/TakeDevice work and the VT is handled like a real tty1 launch:
  `bash -c 'echo $$ | sudo tee /sys/fs/cgroup/user.slice/user-1000.slice/session-1.scope/cgroup.procs >/dev/null; exec ./scripts/input-smoke.sh 30'` (sudo is passwordless).
- Input injection: no uinput module in this kernel. Writing `input_event` structs to the existing node as root works. The VM console drives the *absolute* VMware mouse at /dev/input/event2 (ABS_X/Y 0..65535); event3 is the relative node; event1 is the AT keyboard. A ready injector is in the session scratchpad as inject.py (ioctl EVIOCGABS = 0x80184540+axis).
- `scripts/input-smoke.sh` runs a 2 s probe *before* starting the compositor, so input must be moving from the moment it starts. Its exit hang (bare `wait` on its own log tee) and the `key` counter matching wev's `keymap` line were fixed on branch claude/build-and-run-bf084f on 2026-10-14, together with two compositor bugs (absolute pointer motion ignored; client INT32_MAX damage forwarded unclamped into FB_DAMAGE_CLIPS, EINVAL on the first frame). Merged as PR #12. Interactive move/resize, hardware cursor plane and `scripts/drag-smoke.sh --join-seat-session` (10 scenarios incl. latency) merged as PR #13 on 2026-10-15; drag-smoke is the end-to-end check for window/cursor work.
- `wev`, foot, alacritty and (installed 2026-10-15 for popup testing) `gtk4-demos` (gtk4-demo, gtk4-widget-factory) are present; `weston-simple-shm`, GTK3 and Qt are not.
- Memory profiling: `heaptrack` installed 2026-10-16 (ptrace_scope is 1, so launch the compositor under `heaptrack -o FILE` rather than attaching). PR #19 fixed heap growth (headless backend copies, unused lavapipe device, per-texture staging); a scripted 30-resize foot run should keep the heap flat at ~10 MB, RSS ~160 MB (mostly shared LLVM/Mesa).
- End-to-end checks on main since PR #15: `scripts/drag-smoke.sh` (~20 s) and `scripts/app-smoke.sh` (~2 min, real apps, shell, popups, layer-shell), both with `--join-seat-session` from SSH; both report INVALID (exit 3) if real input mixes in. The window-move p95 WARN in drag-smoke is SVGA3D host fence latency.

- Lost console (2026-10-17): if tty1 is in text mode but the VM screen shows nothing (modetest -p: no plane has an fb, or a stray CRTC still on Virtual-2 after monitor experiments), force fbdev to re-modeset with `head -1 /sys/class/graphics/fb0/modes | sudo tee /sys/class/graphics/fb0/mode`; works only while no process is DRM master. A vmwgfx layout ioctl (DRM_VMW_UPDATE_LAYOUT) alone does not restore it. The console is 1280x800 (fbdev size from boot).
- Protocol gating (since PR #25, 2026-10-21): code tests `LUMEN_PROTOCOL_<XML_STEM>` compile definitions, not `__has_include`; `fetch-protocols.sh` touches `protocols/upstream/.fetch-stamp`, so the next build re-configures by itself. Checkouts built before #25 may still hold ccache-stale objects missing globals; check with `strings lumen-compositor | grep -x ext_idle_notifier_v1`.

- Vulkan checks on this host (2026-10-28): the kernel has no vkms module, so `test_kms_vulkan_scanout` always soft-skips here (passes in 0.01 s) — the Vulkan→KMS present path cannot be exercised on this VM. Sync validation: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`; `test_dmabuf_composite` has 4 pre-existing VUID-vkDestroyImage-image-01000 errors at test teardown (render target freed before the compositor waits).

**Why:** Each fresh session otherwise re-discovers the Vulkan situation, that protocols must be fetched, how to reach DRM master and logind from SSH, and that the VM mouse is absolute.

**How to apply:** For "build and run" use the release preset and `LUMEN_BUILD_DIR=build/release ./scripts/tty-smoke.sh`. For input tests use the cgroup trick plus inject.py instead of asking the user to type at the console.
