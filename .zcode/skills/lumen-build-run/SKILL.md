---
name: lumen-build-run
description: Build, run, smoke-test, screenshot and benchmark Lumen on this host (vmware Arch VM, native — never Docker; iritur notebook for hardware Vulkan) — verified commands including worktree builds, the tests-enabled ctest build, DRM master from SSH, seat-based smokes (no headless verification), input injection, known environmental noise, and the measurement discipline (interleaved A/B, p50 gates). Use whenever building, running, testing, screenshotting or measuring Lumen, or when a smoke/bench result looks like a regression.
---

# Lumen build / run / measure on this host

Full-fidelity sources: `.zcode/memories/lumen-host-build-run-env.md`,
`lumen-notebook-hw.md`, `lumen-app-smoke-known-noise.md`, `lumen-ui-bench-noise-and-g7.md`.

## Environments (binding)

Two hosts share the hostname `iritur` — **detect which one you are on before any seat,
injection or measurement work**: `ls /sys/class/drm/card*/device/driver` (vmwgfx → VM;
i915+nouveau → notebook) or `/proc/bus/input/devices` (VMware devices vs Synaptics/HP).
The VM facts and the notebook facts below do NOT transfer between hosts.

- **Dev VM** (vmware): kernel "fastvm", VMware SVGA/vmwgfx, 2 cores, 15 GB RAM, Arch,
  clang, ccache, sudo NOPASSWD. **No hardware Vulkan** — lavapipe (software) only, so
  the compositor runs the **GL path** (EGL/GLES on GBM, renderer "SVGA3D"); that is
  intended. A Vulkan gain usually cannot be measured there — say so instead of claiming
  one. No vkms module → `test_kms_vulkan_scanout` always soft-skips. No uinput module →
  injection writes to the real nodes: event2 = **absolute** VMware mouse (ABS 0..65535),
  event3 = relative, event1 = AT keyboard.
- **Notebook `iritur`** (real hardware; agent sessions run on it directly over the
  user's SSH connection): Intel HD 5500 (hasvk 1.3.354, eDP 3200×1800@2x) + GTX 950M
  (NVK; never force the GL tier on it — nouveau cannot compile Lumen's GLES UI shaders).
  **Hardware Vulkan is the decisive backend here.** Injection: `sudo python3
  scripts/uinput-mouse.py --output 3200x1800` (hold it open — closing the fd destroys
  the device); the smoke scripts find the device by its name ("Lumen Inject Mouse")
  wherever the kernel enumerated it, so no env vars are needed (explicit
  `LUMEN_INJECT_POINTER`/`LUMEN_INJECT_RELATIVE`/`LUMEN_INJECT_KEYBOARD` still override),
  with `LUMEN_SMOKE_SCALE=1`. A holder started from an older `uinput-mouse.py` creates a
  mouse without REL_X/REL_Y: app-smoke says so up front and R1/R4 and W1/W2/W4 cannot
  pass — restart the holder from the current script. app-smoke reports 158 PASS with 15
  WARN and 4 FAIL rows: the WARNs are capabilities the Vulkan composite path lacks (the
  SIGUSR1 debug read-back is GL-only; window capture and capture dma-buf are not on
  Vulkan — `wire_capture.cpp`), named in each row — only the sub-checks that need the
  capability are WARNed away; the row's log-derived half (`record_limited`'s `rest`)
  still judges, so a defect there FAILs on every backend; the FAILs are the Z portal
  rows (same gaps) and the PSI clean-log row. The user may keep a Lumen
  desktop logged in on tty1 (session 1) — it holds DRM master; seat work needs it stopped
  (ask the owner first); the tty1 login itself stays.
- **Never containers**: no Docker, on any host. UI/desktop development runs natively on
  one of the two hosts above; a container has no seat, no KMS master, no input.
- `ui-bench.sh` keys baselines by hostname+CPU digest (`iritur-168b7c` VM,
  `iritur-08fd3c` notebook); baselines never compare across hosts.

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
ctest --test-dir build/test --output-on-failure -LE bench   # 134 non-bench tests as of 2026-11 (139 registered incl. bench)
```

The plain `release` preset sets `LUMEN_BUILD_TESTS=OFF` → `ctest` on `build/release` sees
**one** test (`scanner_golden`). A "suite green" claim from `build/release` is invalid
evidence. "Suite green" on the tests-enabled build means **no new failures beyond the
owner-tolerated known-red set** of that tree — as of 2026-11 the dev tree carries none:
the logout-popup expectations in `test_task_model` and the ui-gallery golden drift were
both fixed on 2026-11-14 (the studio workflow tracks any future set via its `known_red`
arg). One rerun discounts flakes; a repeated failure of the same test is reportable.
`-S <tree>` works from any cwd, so the same line configures a worktree:

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
- One compositor holds DRM master at a time: check
  `grep -l lumen-composito /proc/[0-9]*/comm` (**never `pgrep -x lumen-compositor` — comm
  is truncated to 15 chars, so the name never matches**), `pgrep -f "^bash .*-smoke\.sh"`
  (plain `pgrep -f smoke.sh` matches your own shell), and `/proc/loadavg`, before and
  after seat work. `loginctl list-sessions` shows whether a user desktop session on tty1
  holds the seat.
- Input injection without uinput: `scripts/inject-input.py` — writes `input_event` structs
  to the existing nodes as root. The smoke scripts pick the nodes themselves
  (`scripts/inject-devices.sh`): the virtual "Lumen Inject Mouse" by name wherever the
  kernel put it this boot, the internal keyboard by name; explicit `LUMEN_INJECT_*` env
  still overrides; without either the dev VM's nodes (event2 absolute, event3 relative,
  event1 AT keyboard) are the defaults. On the notebook: `sudo python3
  scripts/uinput-mouse.py --output 3200x1800` held open creates the mouse (absolute +
  relative + wheel on one node) — start it from this tree, or the relative-motion rows
  cannot pass. `LUMEN_SMOKE_SCALE=1` runs the smokes' compositors at scale 1.
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
- The smokes gate binary freshness by asking the build system (`scripts/build-fresh.sh`):
  a touched module the binaries do not link is not staleness; a genuinely stale binary or
  a changed `CMakeLists.txt` fails the preflight. Never gate freshness on
  newest-source-mtime.
- An interactive seat session (evidence probes) uses `scripts/seat-session.sh` for its
  generic half — precheck/postcheck, config backup/restore by hash, the logout key, the
  pointer park — each command taking the run dir as an argument. Never write a run dir
  into a script: the close-ghost gate's second session reused one that hard-coded the
  first session's dir and trampled its teardown.

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
- **Real-hardware bench noise is idle states and frequency, not the host being "moody"**
  (found 2026-11-14, notebook): the bench's 1 ms frame clock lets the pinned core drop
  into C3-and-deeper states that flush its caches whenever the idle governor guesses the
  sleep is long enough — per run, not per sample. One binary read S2 frame p50 29 µs or
  75 µs (idle ticks 0.17 or 4 µs) with nothing changed, and under schedutil the 5 % duty
  cycle moved the frequency besides (27–43 µs). `ui-bench.sh` now holds a 10 µs PM-QoS
  latency request (`/dev/cpu_dma_latency`, root, C1/C1E stay available — no busy-polling)
  and pins the pinned CPU's governor to performance for the run; runs of one binary then
  agree within ~2 % — on `--backend none`. A baseline recorded unheld and a run held
  (or vice versa) do not compare — the script verifies the hold is in effect (the
  holder marks its `/dev/cpu_dma_latency` write, the governor is read back) and exits 2
  rather than measuring unheld; check for the "== quiet CPU" line all the same.
- **The GPU backends have a second, wandering ~2× host mode the quiet hold cannot
  touch** (found 2026-11-14 evening, notebook, hasvk): minutes-long eras in which
  every memory-bound stage reads ~2× (S2 frame p50 71 µs against a 36.74 µs baseline)
  while `update`, `tick_idle` and `layout_solve.warm` sit at baseline and `none` is
  immune — the core's clock, package heat, the iGPU's rps frequency and the other
  CPUs' governors are all identical in both modes; holding all CPUs at performance
  does not help (the prime hasvk-worker hypothesis is refuted by interleaved A/B;
  evidence/2026-11-14-ui-bench-vulkan-ab/). `ui-bench.sh` therefore warms the host
  before measuring: an unmeasured S2 probe, full-length warm-up rounds while it reads
  above 1.15× the baseline's S2 frame p50, and exit 2 when the pole is not reached —
  a slow-mode run is the host's, not the code's, and is neither a regression nor
  green. Check for the "== host warm-up" lines in a gate output.
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
