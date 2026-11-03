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
- The notebook and the dev VM share the hostname `iritur`. Since PR #69 ui-bench.sh keys baselines by hostname + CPU digest: VM `iritur-168b7c`, notebook `iritur-08fd3c` (no notebook baseline committed yet as of 2026-10-29).
- Benchmarks must run with no other Lumen session (else the compositor runs "no scanout" on a timer).

**Why:** hardware results arrive as pasted logs; a future session needs the device context to read them.
**How to apply:** when the user pastes notebook logs, assume this hardware; ask them to log out of Lumen before benches. Related: [[lumen-no-degradation-rule]], [[lumen-best-in-class-bar]].
