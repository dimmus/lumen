---
title: Sanitizer sweep green on the snprintf tree
date: 2026-11-18
branch: zcode/task/sanitizer-sweep-green (from 95372af)
host: notebook iritur (hasvk)
---

# Sanitizer sweep (ASan+UBSan, TSan) on 95372af — notebook, first recorded run

Baselines on the unmodified tree (logs in that task's build/evidence/2026-11-18-sanitizer-sweep/):
ASan+UBSan 97/136 (39 failed: 17 HUF, 18 LSan, 4 alloc-dealloc-mismatch, 1 SUR, 1 UBSan);
TSan could not even link 7 targets (duplicate operator new/delete) and after -k0 ran
97/136 with 62 race warnings. No sanitizer suite had ever run on this host.

## Genuine defects found and fixed (all with tests where non-trivial)

- tests/render_node.hpp:51 UAF — card name built from the DIR buffer AFTER closedir
  (every GPU test read dead memory uninstrumented). 16 tests' first failure.
- lx.drm fdinfo — read_gpu_memory fstat'ed fd NUMBERS listed by /proc/self/fdinfo while
  the input thread closed them (TSan: 13 tests). Probe dups first now + regression test.
- lx.runtime memory_arena — render thread reset vs UI thread used_bytes on a plain
  unsigned (TSan). Relaxed-atomic statistic + two-thread regression test.
- lx.text itemizer + shape_cache — nothrow operator new freed by operator delete reads
  as "operator new vs free" under this toolchain (ASan intercepts nothrow-new as a real
  NEW; delete lowers to free). malloc/free pair, placement-new preserved init state.
- libstdc++ 16's __get_temporary_buffer (stable_sort's buffer!) has the SAME pair, so
  std::stable_sort aborts instrumented code everywhere. All four Lumen call sites now
  use snapshot_buffer's exact-stable permutation idiom; task_model gained a tie test.
- lx.shell panel build_ui — taskbar/tray letter tiles, overflow count and bell count
  were pointers into build_ui's stack frame, read at the NEXT tick's reconcile
  (ASan: stack-use-after-return). Panel-member storage now, like clock_text_.
- test_dnd_grab int overflow at the on_tick argument (UBSan).
- libwayland 1.26 CONTRACT: wl_display_disconnect frees NO remaining proxy. ~1800 leaked
  96-byte proxies across the client tests. Fixed with tracked-created-destroyed-at-
  disconnect discipline (see test_window_protocols as the reference implementation);
  idempotent disconnect() guards required or the PLAIN build segfaults (the 136/136 gate
  caught exactly that once).

## Third-party, suppressed by object name (protocol run, see TEST.md)

- leak:libfontconfig (config-parse globals), leak:libsystemd (sd-varlink retention past
  full unref — standalone Lumen-free repro leaks identically).
- race:libvulkan_intel_hasvk, race:libVkLayer_khronos_validation, race:libgallium.
- Per-test documented concessions (CMake + TEST.md): EGL-display tests detect_leaks=0
  (dri2 frames symbolize to Mesa source paths — no object name to match),
  test_deco_titles_vk_validation alloc_dealloc_mismatch=0 (SPIRV-Tools' own stable_sort
  inside the validation layer).

## Not finished (follow-up work, in TRIAGE.md of that evidence dir)

- test_protocol_conformance: ~900 proxy leaks; the mechanized sweep crashed on objects
  destroyed by paths the tracker can't see (wl_proxy_destroy of a freed proxy SEGVs
  inside uninstrumented libwayland; no public API distinguishes live from freed).
  Needs case-by-case application of the window_protocols pattern (~180 expect cases).
- pointer_protocols/shell_bridge/capture_protocols LSan leaks (sweeps reduced to
  conservative globals-only after the double-destroy class), output_management
  head_info/mode_info ownership, ext_workspace 7 proxies, a flaky 6-allocation
  fontconfig-path leak in suite runs (text_node/ui_inputs/font_matches).
- Event-queued-behind-protocol-error offers are unreachable client-side: a new-id proxy
  whose event never dispatches cannot be destroyed; tests must roundtrip between a
  selection broadcast and the error trigger (the data-control expect-cases do now).

## Gates at session end

ASan 127/136, TSan 126/136 (both from 97), normal gate 136/136 exit 0, drag-smoke
VERDICT PASS, app-smoke 176/0/1 (the 1 is the known-environmental PSI critical-pressure
WARN of lumen-build-run). ui-bench A/B vs 95372af: see the evidence dir.
