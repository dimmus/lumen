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
  └── cold_arena    (default 512 KiB — protocol parsing, etc.)
```

### Pressure levels

| Level | Trigger (default) | Compositor response |
|-------|-------------------|---------------------|
| `normal` | Used < 75% across pools | None |
| `moderate` | Used ≥ 75% | Evict 25% import cache LRU |
| `critical` | Used ≥ 90% | Evict 50% cache, flush pending releases, evict surface pool |

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

On memory pressure, `pending_release` and (critical) `in_flight` buffers are flushed early.

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
