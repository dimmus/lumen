---
name: lumen-notebook-hw
description: "The user's HP test notebook (iritur): HD 5500 + GTX 950M (NVK), 3200x1800 eDP scale 2, lis3lv02d accelerometer; what it exposed and the vkmark numbers after the fixes"
metadata:
  type: project
---

The user tests Lumen on real hardware on an HP notebook (hostname iritur), alongside the vmwgfx dev VM ([[lumen-host-build-run-env]]). Facts verified from its logs 2026-10-29:

- GPUs: Intel HD Graphics 5500 (Broadwell GT2, i915 KMS on /dev/dri/card2, Vulkan via hasvk, no VK_EXT_host_image_copy) drives the eDP; NVIDIA GeForce GTX 950M (NVK, GM107) as a second GPU. vkmark may pick either.
- Display: eDP-1 3200x1800 at 59.982 Hz, Sharp 0x141C, scale 2 (logical 1600x900).
- Input: an "ST LIS3LV02DL Accelerometer" (HP DriveGuard, event15) whose kernel open took 7.8 s; Lumen now refuses udev-tagged accelerometers before opening (PR #66). Start-up was 8.2 s before.
- Fixed from its runs: Vulkan shm uploads > 16 MiB staging ring failed (HiDPI background), PR #64; input-open timing, PR #65; display-less frame clock ran at period-1ms, PR #67; never-shown dma-bufs held a refresh (immediate vkmark 90 FPS) and CPU-only repaint estimate (FIFO two frames in three late), PR #68.
- vkmark-bench after PR #68 on the HD 5500: immediate 666-923 FPS (was ~90); FIFO 59-60 FPS, mean interval 16.67 ms sd 0.00, late 0-3, discards 0-2 (was 48-49 FPS, 231 late, ~125 discarded). The bench runs the compositor at LUMEN_LOG=debug, which inflates its CPU numbers.
- The notebook and the dev VM share the hostname `iritur`. Since PR #69 ui-bench.sh keys baselines by hostname + CPU digest: VM `iritur-168b7c`, notebook `iritur-08fd3c` — **notebook baselines committed 2026-11-03** (`iritur-08fd3c-{none,cpu,gl,vulkan,client-cpu}.json`; on hasvk the vulkan rows are a hardware backend now).
- Benchmarks must run with no other Lumen session (else the compositor runs "no scanout" on a timer).

Facts from the 2026-11-03 hardware session (kernel 7.2.14; Mesa 26.2.6):

- **hasvk on the HD 5500 reports Vulkan 1.3.354** — passes the 1.3 floor; `automatic`
  runs the **Vulkan composite path** on the eDP (matched by primary node). NVK on the
  GM107 also 1.3.354. hasvk has **no VK_EXT_host_image_copy** — the staging-ring shm
  upload is this host's Vulkan upload path.
- **134/134 ctest green** after: tests pick the connected card's render node
  (`tests/render_node.hpp`, `LUMEN_TEST_RENDER_NODE` overrides; renderD* are numbered
  128+ and /dev/dri/by-path readdir order is unstable — the helper two-passes with
  rewinddir); EDID/scale expectations derived from output state; **tick_ui drains the
  readiness poller** (tick-driven in-process compositors never ran the loop's fd
  callback — updates waited forever on already-signalled fences once i915's
  DMA_BUF_IOCTL_EXPORT_SYNC_FILE path existed; vmwgfx ENOTTYs it, which is why the VM
  never saw this); test children pass `device_override` so the GL tier avoids
  renderD128 = the GTX.
- **nouveau GL (nvc0) cannot compile Lumen's GLES UI shaders**
  (`unpack_32_2x16_split_x/y` unknown op) and Mesa segfaults in draw after the failed
  link — never force the GL tier on the GTX 950M; NVK is its path.
- **drag-smoke PASS** (scale 1, before the 2026-11-04 pacing fix): cursor 9.0/15.8 ms,
  window move 12.7/22.7 ms — gpu leg a real 0.8 ms fence. After the fix the honest lead
  reads cursor 14.4 / move 16.8 ms (gate 18.7). **vkmark**: immediate 650–937 FPS, fifo
  60.00 with sd 0.00 ms and 0–3 late; the panel/background still free; render_us p50
  0.2–0.4 ms. In docs.
- **Input injection** on the notebook: the touchpad is multitouch-only (no hover; bare
  ABS_X/Y ignored by libinput) and the 7.2.11 kernel had no uinput (module/tree
  mismatch). After reboot into 7.2.14: `scripts/uinput-mouse.py` creates an ABS mouse
  (+REL_WHEEL) used as both `LUMEN_INJECT_POINTER` and `LUMEN_INJECT_RELATIVE`;
  keyboard `/dev/input/event3`. `LUMEN_SMOKE_SCALE=1` runs the smokes' compositors at
  scale 1 (their geometry rows are scale-1). inject-input.py `watch` skips
  udev-tagged accelerometers (the lis3lv02d open blocks ~8 s).
- **Hyprland 0.57 will not start from SSH** (start-hyprland wedges before its IPC socket
  exists; needs a real tty launch). weston and sway start fine with the cgroup-join
  trick but take ~8.6/10 s to their socket on this host (reproducible; the VM measured
  80/88 ms).

Facts from the 2026-11-04 bug-fix session:

- **[[lumen-frame-pacing-fix]]**: the callback-client pacing bug found on this host was
  fixed here (59.8–59.9 fps after); the fix, its root causes and numbers are that memory
  and rendering-performance.md §6.1.
- **session-smoke 24/24 PASS on hardware**: the "console blank after a session" was never
  a compositor bug — the exit restore was verified correct (SetCrtc of the saved fbcon fb,
  rc 0, plane back ~2 s after logout). session-smoke's `drm_driver()` must resolve
  `readlink -f <connector>/device` and read THAT node's `device/uevent`: the connector's
  `device` symlink names the card (a drm_minor whose uevent has no DRIVER line), the PCI
  device is one level deeper. Two faster-looking reads both silently fell back to vmwgfx.
- **The CRLF gotcha is wider than lx.shell.cppm**: lx.drm.cppm is CRLF too, and a
  scripted/Python edit strips it silently (the whole file reads as rewritten in git).
  `git diff --stat` showing a whole-file rewrite is the tell; restore with a bytes-mode
  rewrite.
- The comparison harness (pres-probe.c, compare.sh, STATE.md, results/) lives in
  `~/lumen-compare`, outside the repo; its field extraction must use sed, not
  `grep -oE '[0-9.]+'` (it matches digits inside field names like p50_ms).

**Why:** hardware results arrive as pasted logs; a future session needs the device context to read them.
**How to apply:** when the user pastes notebook logs, assume this hardware; ask them to log out of Lumen before benches. Related: [[lumen-no-degradation-rule]], [[lumen-best-in-class-bar]].
