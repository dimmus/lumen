---
name: lumen-build-run
description: Build, run, smoke-test and benchmark Lumen on this host (vmwgfx Arch VM + iritur HP notebook) — verified commands, DRM master from SSH, input injection, known environmental noise, and the measurement discipline (interleaved A/B, p50 gates). Use whenever building, running, testing, or measuring Lumen, or when a smoke/bench result looks like a regression.
---

# Lumen build / run / measure on this host

Full-fidelity sources: `.zcode/memories/lumen-host-build-run-env.md`,
`lumen-notebook-hw.md`, `lumen-app-smoke-known-noise.md`, `lumen-ui-bench-noise-and-g7.md`.

## Hosts

- **Dev VM** (primary): kernel "fastvm", VMware SVGA/vmwgfx, 2 cores, 15 GB RAM, Arch, clang,
  ccache. **No hardware Vulkan** — lavapipe (software) only, so the compositor runs the **GL
  path** (EGL/GLES on GBM, renderer "SVGA3D"); that is intended. A Vulkan gain usually cannot
  be measured here — say so instead of claiming one. No vkms module → `test_kms_vulkan_scanout`
  always soft-skips. No uinput module. sudo is NOPASSWD.
- **Notebook `iritur`** (real HW, user runs it): Intel HD 5500 (hasvk, eDP 3200x1800@2x) +
  GTX 950M (NVK); lis3lv02d accelerometer refused before open. The VM and notebook share the
  hostname; `ui-bench.sh` keys baselines by hostname+CPU digest (`iritur-168b7c` VM,
  `iritur-08fd3c` notebook). Benchmarks need no other Lumen session running.

## Build

```bash
./scripts/fetch-protocols.sh        # first; needs network; upstream/ is gitignored
cmake --preset release              # presets carry ccache, lld (CMAKE_LINKER_TYPE) and test_prep/*
cmake --build --preset release      # ~526 ninja steps, ~15 min cold on 2 cores
```

The user keeps a release build at `build/release`. ccache runs with host `depend_mode=true`:
`.cppm` interface units pass through uncached (`-x c++-module` → `unsupported_source_language`),
importing `.cpp` TUs are cached and invalidated by module changes (verified by an
edit-invert-revert test on `lx.foundation:result`); the old direct-only config could serve
stale `.cpp` objects after a module change. Protocol gating is via
`LUMEN_PROTOCOL_<STEM>` compile definitions; a checkout built before a protocol merge may
hold ccache-stale objects (check `strings lumen-compositor | grep -x <global_name>`).
`test_prep/<test>` targets (CMake ≥ 4.4) build one test's dependency closure only.

## Run

- From SSH, `/dev/dri/card0` has a logind ACL for dimmus → direct-open DRM master works.
  Input devices have **no** ACL → libinput needs logind TakeDevice; to run "as tty1":
  ```bash
  bash -c 'echo $$ | sudo tee /sys/fs/cgroup/user.slice/user-1000.slice/session-1.scope/cgroup.procs >/dev/null; exec ./scripts/tty-smoke.sh'
  ```
- Input injection without uinput: `scripts/inject-input.py` — write `input_event` structs to
  the existing nodes as root. event2 = **absolute** VMware mouse (ABS 0..65535), event3 =
  relative, event1 = AT keyboard.
- Agents may run `lumen-compositor` only **headless** (`--virtual-outputs`, private socket).
  The standalone headless compositor picks lavapipe and cannot import GL-tier client dmabufs
  (use `VK_LOADER_DRIVERS_DISABLE=*` or the in-process host). Real-display e2e belongs to the
  coordinating session / user.
- Lost console (tty1 text mode, no output): force fbdev re-modeset with
  `head -1 /sys/class/graphics/fb0/modes | sudo tee /sys/class/graphics/fb0/mode` while no
  process is DRM master.
- Debug logging: `LUMEN_LOG=debug`. Vulkan sync validation:
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`.

## Smoke suites (end-to-end gates)

- `scripts/drag-smoke.sh --join-seat-session` (~20 s) — window/cursor work; reports INVALID
  (exit 3) if real input mixes in. Its window-move p95 WARN is SVGA3D host fence latency.
- `scripts/app-smoke.sh --join-seat-session` (~2 min) — real apps, shell, popups, layer-shell.
  Its "host present stalls" WARN swings 0→84 between runs of the same binary.
- `scripts/ui-bench.sh` (headless; `--e2e --join-seat-session` for the display path),
  `scripts/vkmark-bench.sh`, `scripts/tty-smoke.sh`.

**Before blaming a change**: only one compositor may hold DRM master — check
`pgrep -x lumen-compositor` and `pgrep -f "^bash .*-smoke\.sh"` (a plain `pgrep -f smoke.sh`
matches your own shell). Check `/proc/loadavg`. Environmental failures seen on clean main:
PSI "critical pressure" WARN (tmpfs pressure), T1/T3 timing flakes (SVGA3D present),
portal-frontend groups when xdg-desktop-portal is broken. Rerun / build a baseline first.

## Measurement discipline (the no-degradation gate)

- Judge UI perf by **interleaved A/B** (same pin, `setarch -R`, ≥3 rounds), never a single
  run against the stored baseline. One binary's p95 moves 40 %+ run to run; p50 up to 14–18 %.
- `ui-bench.sh` gate: FAIL on p50 +25 %, WARN on p50 +10 % or p95 +25 %.
- Never measure while another session builds or runs app-smoke.
- Memory profiling: `heaptrack -o FILE ./build/release/lumen-compositor` (ptrace_scope 1 —
  launch under it, don't attach). Idle compositor allocates ~nothing; ~100k startup
  allocations are Mesa/LLVM/xkbcommon; a frame path that allocates is a bug.
- Require all baseline-passing tests green and pixels/goldens matching; on any regression,
  stop and show the user the A/B evidence before keeping, working around, or accepting it.
