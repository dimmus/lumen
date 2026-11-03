---
name: lumen-shell-first-paint-wip
description: "Shell first-paint work: font-match persistence merged as PR #44 (2026-10-25); background render cache still only in local WIP commit c8d4499 on claude/shell-first-paint"
metadata:
  node_type: memory
  type: project
  originSessionId: e2911b18-507a-41a3-9a93-104b08a026ec
  modified: 2026-10-25T05:54:41.665Z
---

Shell first-paint latency work, paused 2026-10-24 as a local, unpushed WIP commit (c8d4499) on branch claude/shell-first-paint (based on old main 72eb5b2).

**Done:** font matches kept across runs, redone on the current lx.text `face_cache` and merged as PR #44 on 2026-10-25 (`face_cache::persist_matches`, file `$XDG_CACHE_HOME/lumen/fonts-<program>`; the compositor and the shell opt in). First decorated tick_ui went from 24.6 ms to 1.7–2.8 ms on vmwgfx. Don't port the WIP's font part again.

**Still only in the WIP:**
- **Background render cache** (lx.shell:background): `$XDG_CACHE_HOME/lumen/backgrounds/<key>.xrgb`. The file has a 4 KiB header, and on a hit it is the wl_shm pool itself (zero copy; the worker only runs MADV_POPULATE_READ). A miss is written from the memfd by `cache_writer_` after the commit. The LRU keeps 4 files.
- **Loop order:** the shell loop runs the backgrounds' after_dispatch before the panels.

Left for those: a median benchmark, tests, docs, ctest and the smokes.

**Why:** the first panel paint and the first window title were slow (fontconfig start, background render). **How to apply:** if asked to continue shell first-paint, port only the background cache and loop order from c8d4499 onto current main.
