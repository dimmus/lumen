> **Status:** Current (v0.3) — see code for implementation truth

# Memory Management

Unified memory strategy for Lumen: per-subsystem arenas, global pressure
coordination, bounded fixed pools with overflow policies, GPU budget tracking,
and the client buffer lifetime loop.

**Related:** [architecture.md](../architecture.md) · [rendering-performance.md](rendering-performance.md) · [dmabuf-vulkan-import.md](dmabuf-vulkan-import.md)

---

## 1. Design goals

| Goal | Mechanism |
|------|-----------|
| Predictable hot-path memory | Per-frame bump arenas (UI / render / worker) |
| No unbounded growth | Fixed caps + overflow policies + LRU eviction |
| GPU memory safety | Per-client import limits + `gpu_budget_monitor` |
| Correct buffer lifetimes | `buffer_lifecycle_tracker` → `wl_buffer.release` |
| Pressure response | `memory_budget_coordinator` fans out to caches |
| Cold-path without heap churn | Fixed-capacity event source registry (32 slots) |

---

## 2. Unified memory subsystem

**Module:** `lx.runtime.memory`

```
memory_budget_coordinator
  ├── ui_arena      (default 4 MiB, reset each tick_ui)
  ├── render_arena  (default 8 MiB, reset each tick_render)
  ├── worker_arena  (default 4 MiB)
  ├── cold_arena    (default 512 KiB — protocol parsing, etc.)
  └── staging       (not an arena: the compositor's shm pixel staging, reported each
                     tick_ui against staging_budget_bytes, default 64 MiB)
```

### Pressure levels

| Level | Trigger (default) | Compositor response |
|-------|-------------------|---------------------|
| `normal` | Every pool < 75% and no kernel pressure | None |
| `moderate` | Fullest pool ≥ 75% | Evict 25% import cache LRU; unmap every spare staging block |
| `moderate` | PSI `some 150000 2000000`: some task stalled on memory ≥ 150 ms of a 2 s window (7.5 %) | As above |
| `critical` | Fullest pool ≥ 90% | Evict 50% cache, flush pending releases, evict surface pool; unmap spare staging |
| `critical` | PSI `full 100000 2000000`: every non-idle task stalled at once ≥ 100 ms of 2 s (5 %) | As above |

The fullest pool decides, not the sum of all pools: summing let a large budget dilute a
pool that was actually full (a 64 MiB staging budget would hide a 100 % used frame arena).

The coordinator keeps the pools' level and the kernel's (`on_external_pressure`) apart and
reports the higher; the handler runs when that changes. Neither overwrites the other: the
pools are re-evaluated every frame, which used to drop an external report a frame after it
came.

### Kernel pressure (PSI)

`lx::runtime::psi_monitor` (`lx.runtime`, partition `:psi` for the logic) feeds the kernel's
pressure stall information (Documentation/accounting/psi.rst) into the coordinator. The
compositor starts it in `compositor::start()`, on the UI loop, where the handler runs.

- **Which file.** `$MEMORY_PRESSURE_WATCH` when systemd sets it (`/dev/null`: off); else the
  compositor's own cgroup (`/proc/self/cgroup` → `/sys/fs/cgroup/<path>/memory.pressure`);
  else `/proc/pressure/memory`. The cgroup comes first: PSI is hierarchical, so it shows a
  stall of any compositor thread whatever the cause (a system short of memory, a
  `MemoryHigh=` on the service, `session.slice` or the user slice), and not other cgroups
  stalling against their own limits, which dropping the compositor's caches cannot relieve.
  As a user service (`deploy/systemd`) the file is the user's to write; a login shell's
  `session-N.scope` file is root's, so a compositor started from a tty uses the system file.
- **Triggers.** One open file per trigger (a second write to the same file is `EBUSY`); the
  string is written with its NUL (the kernel overwrites the last byte it is given). Without
  `CAP_SYS_RESOURCE` the kernel (6.5+) accepts only windows that are multiples of 2 s, up to
  10 s — 1 s and 3 s are `EINVAL` — so both use 2 s. The fds wait for `EPOLLPRI`
  (`event_source::poll_priority`): a PSI file always polls readable.
- **Decay.** A trigger fires at most once per window and nothing reports pressure ending.
  The level holds until neither trigger has fired for 10 s, then steps down only as far as
  the file's `avg10` still supports (`full` ≥ 5 % keeps critical, `some` ≥ 7.5 % keeps
  moderate) and checks again 10 s later. Unreadable averages decay to normal.
- **Without PSI** (no file, no write access, a kernel before 6.5 refusing a user's trigger,
  `psi=0`): one `info` line at start-up, and pressure comes from the pools alone. A trigger
  that reports faster than its window (its cgroup gone) is dropped with one `warn` rather
  than spin the UI thread.

### Pixel staging

shm pixels are copied at commit (the client may reuse its buffer at once) and consumed on
the render thread — except on GL with client-memory uploads (`shm_direct_uploads`), where
nothing is staged and the client's buffer is held until the render thread has read it.
GL otherwise, and Vulkan: `shm_upload_pool` — transient mmap'd blocks (sized by the
buffer-age region on GL, the whole image on Vulkan), returned right after the upload,
unmapped after 1 s idle; an idle desktop holds none, and no more than 64 MiB is staged and
not yet uploaded. CPU: `shm_pixel_store` — the staged rows are the texture, freed by `collect` once a
retired texture's slots are no longer sampled. Both are reported as `memory_pool::staging`.
See rendering-performance.md, SHM staging, for why per-texture staging used to grow
without bound.

Start-up probes Vulkan with an instance only (`device_selector::probe_devices`) and
creates a Vulkan device only when Vulkan may composite: hardware Vulkan behind the card, or an explicit
`vulkan`/`cpu` request. A software ICD (lavapipe) costs about 28 MB of heap to create a
device on (LLVM and its JIT, measured with heaptrack) — paid before for a device the GL
path never used. The CPU and nested fallbacks create it on demand; on the GL path any
device that exists is destroyed. Start-up then calls `malloc_trim(0)` once, since glibc
keeps freed heap pages resident.

The other heap leak found with heaptrack: on GPU paths the commit path still copied every
shm buffer into the headless software backend, whose table was keyed by never-repeating
texture ids and never pruned — 60 MB over a 30-resize run. The headless backend is now
handed to the protocols only when it is the present path, and retired textures are erased.

### Fixed tables

The compositor's per-object tables keep fixed capacities, so their indices stay stable and
nothing reallocates on the frame path. They are `lx::sparse_slots<T, N>` (`lx.foundation`):
storage comes in blocks of 16 slots, allocated on first write. A desktop with a dozen windows
touches a block or two. Before this, every table was fully resident from start-up, about
1 MB together. The tables converted are:

- toplevels and popups;
- the surface tree;
- decorations;
- surfaces and buffers.

Iteration visits allocated blocks only. Toplevel lookups by id and by surface go through an
`id_slot_index`, not a scan.

`lx::spsc_queue` leaves its slots unconstructed until pushed. Its storage is written lazily,
so an idle queue's pages are never touched.

Register handler via `memory_budget_coordinator::set_pressure_handler`.

Compositor wires this in `compositor::start()` — see `compositor_impl::handle_memory_pressure`.

---

## 3. Overflow policies (fixed caps)

**Module:** `lx.runtime.memory` — `overflow_action`

| Policy | Behavior |
|--------|----------|
| `reject` | Skip push; caller handles error |
| `drop_newest` | Ignore incoming entry (default for draw list) |
| `drop_oldest` | Evict oldest, accept new (scene children) |
| `warn_and_drop` | Log warn + drop_newest |

### Where applied

| Structure | Cap | Default overflow |
|-----------|-----|------------------|
| `draw_list` | 4096 commands | `drop_newest` |
| `container_node` children | 128 | `drop_newest` (configurable) |
| `dmabuf_import_cache` | 512 global / 64 per client | LRU evict before reject |
| `event_loop` sources | 32 | Silent reject on add |
| `buffer_lifecycle_tracker` | 1024 records | `reject` |
| `presentation_feedback_table` (wp_presentation) | 1024 slots, 16 per content update | `reject`: the new feedback is sent `discarded` at once, one `warn` a minute |
| `idle_tracker` (ext_idle_notification_v1) | 64 notifications, 16 per client | `reject`: the client gets `no_memory`, one `warn` per run |
| `inhibitor_set` (zwp_idle_inhibitor_v1) | 64 inhibitors, 16 per client | `reject`: as above |
| zwlr_output_power_v1 objects | per request (an intrusive list, freed by the resource destructor) | — (bounded by the client's own objects) |
| `session_lock` lock surfaces (ext_session_lock_surface_v1 of the lock in charge) | 16, each with 16 outstanding configures | `reject`: no_memory past 16; past 16 configures the oldest is forgotten (acking it is `invalid_serial`) |
| ext_session_lock_v1 / lock-surface wire objects | per request (freed by the resource destructor) | — (bounded by the client's own objects) |
| text-input, input-method, input-popup, keyboard-grab and virtual-keyboard wire objects | per request (freed by the resource destructor); a text input holds two 4 KiB state copies, an input method two 4 KiB strings; never per frame or per key | — (bounded by the client's own objects) |
| virtual-keyboard keymaps | one sealed memfd per virtual keyboard whose keymap differs from the seat's (the seat's is shared) | refused (logged, the keyboard keeps its previous keymap): larger than 4 MiB or not compiling |
| Pending frame callbacks | 512 | answered at once (a dropped callback stalls its client) |
| Visibility-pass scratch (`scene_graph`) | 4096 node pointers + flags, parallel to the draw list | fixed array, reused per frame |

---

## 4. GPU memory management

### Import cache per-client limits

**Module:** `lx.gfx.import_cache`

```cpp
gfx::import_cache_config{
    .max_entries = 512,
    .max_entries_per_client = 64,
    .stale_frame_threshold = 120,
};
```

When a client exceeds its quota, LRU eviction runs **within that client** before rejecting new imports.

### GPU budget monitor

**Module:** `lx.gfx.gpu_budget` — `VK_EXT_memory_budget` scaffold

- `query()` → `gpu_memory_stats` (total / used / budget bytes)
- `client_within_budget()` — per-client byte cap (default 256 MiB)
- `track_client_usage()` / `untrack_client()`

### Compositor surface pool

**Module:** `lx.gfx.surface_pool` — reuse compositor-owned VkImages for chrome/effects.

`evict_on_pressure()` drops unused pooled surfaces on moderate/critical pressure.

### Scanout buffer pinning

**Module:** `lx.compositor.scanout` — `scanout_pin_policy`

| Policy | Behavior |
|--------|----------|
| `prefer_scanout` | Keep dmabuf pinned on KMS plane |
| `prefer_composite` | Release scanout quickly |
| `auto_select` | Pin video/game hints; release UI |

### Scanout targets per output

Every output pipeline (ADR 008) owns three scanout targets of its mode's size — GL
renderbuffers on GBM, CPU dumb buffers, or Vulkan images — `3 × W × H × 4` bytes each
(≈ 21 MB for 1914×924, 15 MB for 1280×1024), allocated when the pipeline is created at
start and freed with it; nothing per frame. The CRTC allocation's TEST_ONLY commits reuse
those targets' framebuffers. Each output view also has its own `snapshot_buffer` (three
draw lists, grown to the largest frame seen), created on the UI thread with the view and
kept for the scene's lifetime. A connector probe fills a fixed `connector_scan` (16
connectors × 96 modes with their 68-byte blobs) — no allocation.

Hotplug (ADR 008 Part B) changes this only at the edges, never per frame:

- A **new output**'s pipeline and targets are made by render when the output is added
  (one set per candidate mode size tried, the failed ones freed at once) and freed when it
  goes: the CRTC is turned off first, GL/Vulkan objects are released on render, the rest
  of the pipeline on UI.
- A **mode change** allocates the new size's targets once, for its TEST_ONLY ("staged"),
  and switches to them; the old mode's targets stay until the new mode's first flip (one
  of them is on screen until then) — for one or two frames both sets exist (≈ 21 MB more
  for a 1914×924 output).
- The hotplug controller allocates its `connector_scan` (≈ 140 KB, the worker's) once at
  start, and the compositor a table of pending adds (16 probes, ≈ 150 KB) at the first
  hotplug. A probe, a diff and an identical scan allocate nothing.

Output configuration (ADR 008 Part C), also only at the edges:

- A configuration's **TEST_ONLY** allocates targets at every new mode it tests (staged:
  a changed output's are kept ≤ 5 s for its switch, an enabled output's dropped after the
  test — its add allocates its own); a refused test frees them at once.
- The compositor keeps a copy of the newest `connector_scan` (≈ 150 KB, for the heads of
  monitors that are not outputs), the heads it builds (`mgmt::head_set`, ≈ 50 KB, and the
  wire's copy of what clients were told, the same), the job being run (two targets and a
  configuration, ≈ 6 KB) and the persisted `output_config` (64 monitors, 64 setups:
  ≈ 250 KB) — each allocated once. Heads are rebuilt only when outputs or connectors
  changed; an apply or a save allocates the file's text (a few hundred bytes) on UI.
- Each turned output's render pipeline grows one cursor-image buffer (plane-sized) once.
- Head, mode and configuration objects of `zwlr_output_manager_v1` are allocated per object
  and freed by their destructors (intrusive lists), never from a shared table.

---

## 5. Buffer lifetime loop

**Module:** `lx.compositor.buffer_lifecycle`

```
client commit
  → import_cache.acquire
  → lifecycle.on_import
  → lifecycle.on_attach
tick_render
  → lifecycle.on_present
  → lifecycle.on_render_done
  → lifecycle.flush_pending_releases
      → import_cache.release
      → wl_buffer.release (P0 wire)
client disconnect
  → lifecycle.on_client_disconnect
      → import_cache.evict_client
```

States: `imported → attached → in_flight → pending_release → released`

With several outputs a buffer is released at the **render low-water mark**: the oldest
frame any live output may still sample (a view's newest consumed publish, or the frame its
composited-but-uncommitted slot holds). An output that is not due keeps its last snapshot,
so a buffer it shows stays held until that output publishes again.

A dma-buf is sampled zero-copy by every composite that shows it, so a record stays
**current** — never released, whatever the low-water mark — while it is its surface's
content. A newer commit (a different buffer, a null attach) or the surface's end supersedes
it; every published frame tags the current records with its index (an output's, and a
window capture view's), so the superseded record is released once the frame of its last
tag has completed. Window capture views count towards the mark (render's newest composited
view), and a view job UI posted that render has not taken yet holds it back
(`capture_view_floor`). A wl_buffer destroyed while still current keeps its record (the
import stays on screen): no `wl_buffer.release` goes to the dead object, and an
explicit-sync release point it owed moves to the record and is signalled when the record
is released.

On memory pressure, `pending_release` and (critical) `in_flight` buffers are flushed early
— never a current one (its import stays on screen, so releasing it frees nothing).

---

## 6. Multi-process footprint

Multi-process is a **security/stability tradeoff**, not a memory bug. Mitigations:

| Process | Memory strategy |
|---------|-----------------|
| Compositor | Arenas + bounded caches (this doc) |
| Shell | Normal Wayland client; prefer deltas over full snapshots |
| Apps | Per-client import limits in compositor |

Shell policy bridge should emit **deltas** after initial snapshot — see [shell-state-sync.md](shell-state-sync.md).

---

## 7. Cold-path allocation

| Before | After |
|--------|-------|
| `event_loop` used `std::vector` for sources | Fixed array of 32 `event_source*` |
| Unbounded source registration | `add_source` rejects when full |

Hot path (`tick_ui` / `tick_render`) uses arenas; cold path uses fixed pools where possible.

---

## 8. Module map

| Concern | Module |
|---------|--------|
| Arenas + pressure | `lx.runtime.memory` |
| Kernel pressure (PSI) | `lx.runtime` (`psi_monitor`), `lx.runtime.psi` |
| GPU budget | `lx.gfx.gpu_budget` |
| Import cache LRU + per-client | `lx.gfx.import_cache` |
| Surface pool | `lx.gfx.surface_pool` |
| Buffer lifetime | `lx.compositor.buffer_lifecycle` |
| Scanout pinning | `lx.compositor.scanout` |
| Overflow policies | `lx.runtime.memory` (`overflow_action`) |

---

## 9. Compositor config

```cpp
lx::compositor::config cfg{
    .memory = {
        .ui_arena_bytes = 4 * 1024 * 1024,
        .pressure_moderate_used_pct = 75,
        .pressure_critical_used_pct = 90,
    },
    .import_cache = {
        .max_entries_per_client = 64,
    },
    .gpu_budget = {
        .max_import_bytes_per_client = 256 * 1024 * 1024,
    },
};
```

---

## 10. Implementation phases

| Phase | Deliverable | Scaffold |
|-------|-------------|----------|
| **M0** | Memory coordinator + overflow policies | ✅ |
| **M1** | Per-client import limits + pressure eviction | ✅ |
| **M2** | Buffer lifecycle tracker | ✅ API |
| **M3** | GPU budget query (`VK_EXT_memory_budget`) | ✅ stub |
| **M4** | Surface pool + scanout pin | ✅ stub |
| **P0** | Real `wl_buffer.release` + measured RSS tests | Runtime |

---

## 11. UML index

| Diagram | Topic |
|---------|-------|
| [34-memory-pressure-response.mmd](../uml/34-memory-pressure-response.mmd) | Pressure fan-out |
| [35-buffer-lifecycle.mmd](../uml/35-buffer-lifecycle.mmd) | Import → release loop |
| [36-gpu-memory-budget.mmd](../uml/36-gpu-memory-budget.mmd) | Per-client GPU limits |
| [37-overflow-policies.mmd](../uml/37-overflow-policies.mmd) | Fixed-cap overflow behavior |
