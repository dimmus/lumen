---
name: lumen-malloc-replacement-measured
description: "jemalloc/mimalloc/tcmalloc measured against glibc for lumen-compositor on 2026-10-27 — no speed gain, more memory; glibc stays"
metadata:
  node_type: memory
  type: project
  originSessionId: 9f1fffcf-ab0d-4a59-a744-b64c821c11e5
  modified: 2026-10-27T15:53:19.551Z
---

Measured 2026-10-27 (vmwgfx VM, GL path, release build, drag-smoke under LD_PRELOAD, RSS sampled every 250 ms):

- Compositor anonymous RSS at end of run: glibc 26.9 MiB, tcmalloc 36.6, jemalloc 44.1, mimalloc 57.5. Total RSS ~175 MiB is mostly file-backed Mesa/LLVM.
- Latency (drag-smoke g) identical within noise for all four.
- heaptrack: ~100k allocations are start-up (Mesa/gallium, libvulkan loader, xkbcommon, expat, Lumen config); an idle compositor allocates ~nothing afterwards, and the full drag workload adds ~10k (~30 per frame at most). The frame path is arena-based, so the allocator is not on it.

**Why:** The user asked whether jemalloc/mimalloc would speed Lumen up or shrink it. The answer was no, by measurement; re-measuring wastes ~10 min of console time.

**How to apply:** Don't propose a malloc replacement for the compositor without a new workload that allocates on the frame path or a soak test showing glibc fragmentation. The shell was not measured. mimalloc and jemalloc were installed on the host for the test. Related: [[lumen-best-in-class-bar]], [[lumen-host-build-run-env]].
