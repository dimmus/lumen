> **Status:** Current (v0.3) — see code for implementation truth

# Threading Model — First-Class Multithreading

> **Status:** Current (v0.3) — reconciled to snapshot_buffer v2 (index swap, reader lease)

Lumen v0.3 integrates multithreading at the API layer via **`lx.sync`**, **`lx.runtime.executor`**, and **triple-buffered immutable frame snapshots**.

## Golden rules (hard ownership)

| Resource | Owner thread | Notes |
|----------|--------------|-------|
| `widget` tree | **UI** (`affinity::ui`) | Never touch from worker/render |
| `scene_graph` mutation | **UI** | `update`, `mark_dirty`, `commit_frame` |
| `immutable_frame_snapshot` | **Render** (read-only) | Acquired after UI `commit_frame` |
| `draw_list` build (scratch) | **UI** | Published via `snapshot_buffer` |
| Vulkan record/submit | **Render** | One recording thread per queue |
| `event_bus` | **UI** | Not thread-safe |
| `event_loop::post` | **Any** → drains on UI | SPSC inbound queue |
| Wayland client commits | **UI or Wayland** | Compositor may split wayland affinity |
| Layout assist / I/O | **Worker** | Results posted back to UI strand |
| `shared_state<T>` | **Worker writes**, **UI reads** | Protected by mutex + atomic ready |

Violations are caught in debug via `lx::runtime::assert_affinity()`.

See also: [errors-and-logging.md](errors-and-logging.md) for failure propagation and log levels on frame paths.

## Logical affinities

```cpp
enum class affinity { ui, render, worker, wayland, any };
```

| Affinity | Typical thread | Modules |
|----------|----------------|---------|
| `ui` | Main loop | `lx.ui`, `lx.scene` (mutate), `lx.runtime` |
| `render` | Dedicated render | `lx.gfx`, `lx.scene` (acquire snapshot) |
| `worker` | Thread pool | `lx.async`, `lx.layout` (assist), `lx.text` (cache) |
| `wayland` | Optional split | `lx.wayland.server` dispatch (compositor) |

Use `frame_scheduler::affinity_scope` on thread entry to set `current_affinity()`.

## Cross-thread dispatch

```
Any thread ──executor.post(affinity, fn)──▶ strand queue
UI event_loop.run() ──drain──▶ ui strand + inbound SPSC + worker completions
Render loop ──drain──▶ render strand
```

## Frame snapshot handoff (snapshot_buffer v2)

1. **UI** builds `draw_list` into the **write slot** via `build_draw_list_into` (no interim copy).
2. **UI** calls `draw_list::batch_for_render()` then `scene_graph::commit_frame(index)`.
3. `snapshot_buffer::try_publish()` **swaps the index only** — draw list storage is not copied to render.
4. **Render** calls `acquire_render_snapshot()` → `const immutable_frame_snapshot&` (reader lease).
5. **Render** records Vulkan / headless blit from snapshot; **UI** may already be writing the next slot.
6. **Render** calls `release_render_frame()` after present to return the reader lease.

See [../uml/27-snapshot-v2-handoff.mmd](../uml/27-snapshot-v2-handoff.mmd) and
[../uml/23-frame-handoff-sequence.mmd](../uml/23-frame-handoff-sequence.mmd).

## Modules

| Module | Role |
|--------|------|
| `lx.sync` | `mutex`, `atomic`, `spsc_queue`, `once_flag` |
| `lx.runtime.executor` | `executor`, `strand`, `affinity`, `assert_affinity` |
| `lx.scene.snapshot` | `snapshot_buffer`, `immutable_frame_snapshot` |
| `lx.scheduler` | Routes `post_to(affinity)` to executor |
| `lx.async` | Worker pool; completion via `executor.post(ui, …)` |

## Compositor process (implemented)

`lumen-compositor` runs three logical affinities on **two OS threads** (+ optional worker):

| OS thread | Affinity | Loop |
|-----------|----------|------|
| **Main** | `ui` | `event_loop.run()` + vsync `timer_source` → `tick_ui()` |
| **Render** | `render` | `render_loop()` waits on `condition_variable` → `tick_render()` |
| **Worker** | `worker` | `worker_loop()` drains `executor` worker strand |

### Vsync wiring (current → future)

**Current (P0/P1):** DRM/KMS page-flip when `kms_device::open()` succeeds; `timer_source` remains
fallback for headless/CI. `hotplug_monitor` refreshes outputs each UI tick.

### CLI

```bash
lumen-compositor [socket-name] [-f fps]
```

## Process-level vs thread-level

| Level | Model |
|-------|--------|
| **Process** | Compositor / shell / apps (Wayland) — always separate |
| **Thread** | UI + render + worker within compositor and app processes |

## Embedded / low-core mode

On single-core or kiosk targets, compositor may **merge** `ui + render + wayland` onto one thread. API stays the same; executor affinities collapse to one physical thread (sequential drain).

## UML

- [22-thread-model.mmd](../uml/22-thread-model.mmd) — thread diagram
- [24-executor-class.mmd](../uml/24-executor-class.mmd) — class diagram
- [23-frame-handoff-sequence.mmd](../uml/23-frame-handoff-sequence.mmd) — frame sequence
