---
name: lumen-build-run
description: Build, run, smoke-test, screenshot and benchmark Lumen on this host (vmware Arch VM, native — never Docker; iritur notebook for hardware Vulkan) — verified commands including worktree builds, the tests-enabled ctest build, DRM master from SSH, seat-based smokes (no headless verification), input injection, known environmental noise, and the measurement discipline (interleaved A/B, p50 gates). Use whenever building, running, testing, screenshotting or measuring Lumen, or when a smoke/bench result looks like a regression.
---

# Lumen build / run / measure on this host

Full-fidelity sources: `.zcode/memories/lumen-host-build-run-env.md`,
`lumen-notebook-hw.md`, `lumen-app-smoke-known-noise.md`, `lumen-ui-bench-noise-and-g7.md`.

## Environments (binding)

- **Dev VM** (this host, primary): kernel "fastvm", VMware SVGA/vmwgfx, 2 cores, 15 GB RAM,
  Arch, clang, ccache, sudo NOPASSWD. **No hardware Vulkan** — lavapipe (software) only, so
  the compositor runs the **GL path** (EGL/GLES on GBM, renderer "SVGA3D"); that is
  intended. A Vulkan gain usually cannot be measured here — say so instead of claiming one.
  No vkms module → `test_kms_vulkan_scanout` always soft-skips. No uinput module.
- **Notebook `iritur`** (real hardware): Intel HD 5500 (hasvk 1.3.354, eDP 3200×1800@2x,
  `/dev/dri/card2`) + GTX 950M (NVK; never force the GL tier on it — nouveau cannot compile
  Lumen's GLES UI shaders). Sessions run with the user connected to the notebook over SSH,
  tty1 logged in (logind session 1) for `--join-seat-session`. No SSH route to it exists
  from this VM — when hardware-Vulkan evidence is needed, ask the user to host the session
  on the notebook. `ui-bench.sh` keys baselines by hostname+CPU digest (`iritur-168b7c` VM,
  `iritur-08fd3c` notebook); baselines never compare across hosts.
- **Never containers**: no Docker, on any host. UI/desktop development runs in this VM
  natively or on real hardware; a container has no seat, no KMS master, no input, and its
  results are not evidence for Lumen's surface.

## Build

```bash
./scripts/fetch-protocols.sh        # first; needs network; upstream/ is gitignored
cmake --preset release              # presets carry ccache, lld (CMAKE_LINKER_TYPE) and test_prep/*
cmake --build --preset release      # ~526 ninja steps, ~15 min cold on 2 cores
```

**The tests-enabled build (the only one that can gate a change):**

```bash
cmake --preset release -S . -B build/test -DLUMEN_BUILD_TESTS=ON
cmake --build build/test -j2
ctest --test-dir build/test --output-on-failure -LE bench   # 138 tests as of 2026-11
```

The plain `release` preset sets `LUMEN_BUILD_TESTS=OFF` → `ctest` on `build/release` sees
**one** test (`scanner_golden`). A "suite green" claim from `build/release` is invalid
evidence. `-S <tree>` works from any cwd, so the same line configures a worktree:

```bash
cmake --preset release -S ../lumen-wt/<task> -B ../lumen-wt/<task>/build/test -DLUMEN_BUILD_TESTS=ON
```

Worktrees need `protocols/upstream` (gitignored) copied in before configure:
`cp -r protocols/upstream <worktree>/protocols/`. ccache is shared and `depend_mode=true`:
`.cppm` interface units pass uncached, importing `.cpp` TUs cache and invalidate on module
changes. Protocol gating is `LUMEN_PROTOCOL_<STEM>` compile definitions; a checkout built
before a protocol merge may hold ccache-stale objects (check
`strings lumen-compositor | grep -x <global_name>`). `test_prep/<test>` targets
(CMake ≥ 4.4) build one test's dependency closure only.

## Run on the real seat (no headless verification)

Compositor/display/input verification runs **on the seat**, never on `--virtual-outputs`
instances — a headless run is not evidence for that surface (unit ctest and the in-process
`ui-bench` engine bench remain valid supplementary evidence).

- From SSH, `/dev/dri/card0` has a logind ACL for dimmus → direct-open DRM master works.
  Input devices have **no** ACL → libinput needs logind TakeDevice. The smoke scripts'
  `--join-seat-session` handles moving into the active tty1 session cgroup; for a manual
  run:
  ```bash
  bash -c 'echo $$ | sudo tee /sys/fs/cgroup/user.slice/user-1000.slice/session-1.scope/cgroup.procs >/dev/null; exec ./scripts/tty-smoke.sh'
  ```
- One compositor holds DRM master at a time: check `pgrep -x lumen-compositor` and
  `pgrep -f "^bash .*-smoke\.sh"` (plain `pgrep -f smoke.sh` matches your own shell), and
  `/proc/loadavg`, before and after seat work.
- Input injection without uinput: `scripts/inject-input.py` — writes `input_event` structs
  to the existing nodes as root. event2 = **absolute** VMware mouse (ABS 0..65535),
  event3 = relative, event1 = AT keyboard. On the notebook: `scripts/uinput-mouse.py`
  (virtual ABS mouse + wheel) as `LUMEN_INJECT_POINTER`/`LUMEN_INJECT_RELATIVE`,
  keyboard `/dev/input/event3`, and `LUMEN_SMOKE_SCALE=1`.
- Lost console (tty1 text mode, no output): force fbdev re-modeset with
  `head -1 /sys/class/graphics/fb0/modes | sudo tee /sys/class/graphics/fb0/mode` while no
  process is DRM master.
- Debug logging: `LUMEN_LOG=debug`. Vulkan sync validation (notebook):
  `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`.

## Screenshots and pixel evidence

- Pixel gates live inside the smoke suites (`scale-smoke`, `monitor-smoke`, goldens under
  `tests/golden/`) — they capture and compare on the seat; run them rather than eyeballing.
- `lumen-settings-shot` (tests-enabled build) renders UI golden shots offline.
- Ad-hoc screenshots of a live desktop go through the Screenshot portal of the running
  session (`scripts/run-desktop.sh` brings up the session; portal setup is
  `scripts/portal-session.sh`, a library the scripts source).
- The cross-compositor comparison harness (pres-probe, compare.sh) lives in
  `~/lumen-compare` outside the repo — see "Comparing with sway/weston/hyprland" below.

## Smoke suites (end-to-end gates on the seat)

- `scripts/drag-smoke.sh --join-seat-session` (~20 s) — window/cursor work; reports INVALID
  (exit 3) if real input mixes in. Its window-move p95 WARN is SVGA3D host fence latency.
- `scripts/app-smoke.sh --join-seat-session` (~2 min) — real apps, shell, popups,
  layer-shell. Its "host present stalls" WARN swings 0→84 between runs of the same binary;
  ~36 rows embed the VM's 1914×924 geometry and fail on the notebook's panel — known
  environmental, not regressions.
- `scripts/ui-bench.sh` (in-process engine bench; `--e2e --join-seat-session` for the
  display path), `scripts/vkmark-bench.sh`, `scripts/tty-smoke.sh`,
  `scripts/session-smoke.sh`, `scripts/scale-smoke.sh`, `scripts/monitor-smoke.sh`
  (vmwgfx-specific), `scripts/input-smoke.sh`.
- Smoke scripts grep compositor log lines — log format strings are interfaces; keep them
  stable when touching logging.

**Before blaming a change**: rerun/build a baseline first. Environmental failures seen on
clean main: PSI "critical pressure" WARN (tmpfs), T1/T3 timing flakes (SVGA3D present),
portal-frontend groups when xdg-desktop-portal is broken. One rerun discounts noise; a
second failure is reportable.

## Measurement discipline (the no-degradation gate)

- The bar (AGENTS.md §1.2): best-in-class against wlroots/sway, KWin, Mutter, Smithay/niri,
  Hyprland — every efficiency claim backed by measurement (latency, allocations, damage),
  never assertion.
- Judge UI perf by **interleaved A/B** (same pin, `setarch -R`, ≥3 rounds), never a single
  run against the stored baseline. One binary's p95 moves 40 %+ run to run; p50 up to
  14–18 %. `ui-bench.sh` gate: FAIL on p50 +25 %, WARN on p50 +10 % or p95 +25 %.
- Never measure while another session builds or runs a smoke. Serialize all seat work.
- Memory: `heaptrack -o FILE ./build/release/lumen-compositor` (ptrace_scope 1 — launch
  under it, don't attach). Idle compositor allocates ~nothing; ~100k startup allocations
  are Mesa/LLVM/xkbcommon; a frame path that allocates is a bug.
- Require all baseline-passing tests green and pixels/goldens matching; on any regression,
  stop and show the user the A/B evidence before keeping, working around, or accepting it.

## Comparing with sway/weston/hyprland

The comparison harness (`pres-probe` client, `compare.sh`, results/) lives in
`~/lumen-compare`. Verified procedure (notebook, 2026-11): launch each compositor on the
seat (lumen/weston/sway fine from SSH with the cgroup-join trick; **Hyprland wedges from
SSH** — it needs a console launch by the user), drive `pres-probe`, take medians over ≥5
rounds. Metrics: socket-ready ms, first-frame ms, commit→present p50/p95, anim CPU %,
idle CPU %, kill ms, callback fps; add `VmRSS` sampling from `/proc/<pid>/status` for the
memory footprint. Sample results (notebook, medians of rounds 2–9): Lumen socket 370 ms /
commit→present p50 3.94 ms / anim 5.1 %; weston 8948 / 22.4 / 4.8; sway 10432 / 33.3 / 2.4.
The `lumen-compositor-bench` workflow automates this — prefer it over manual runs.

## Gotchas

- `lx.shell.cppm` and `lx.drm.cppm` use CRLF — scripted/Python edits silently strip it
  (tell: `git diff --stat` whole-file rewrite); restore CRLF after.
- Push needs SSH: `git push git@github.com:dimmus/lumen.git <branch>` (https origin has no
  credential helper).
- `pkill -f` self-matching patterns kill your own shell — build patterns at runtime.
