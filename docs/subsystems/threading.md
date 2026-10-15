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

### Damage across the handoff

Each snapshot carries a `gfx::damage_region` (up to 16 rects, no allocation): what changed
since the previous *published* snapshot.

- **Carry-forward (UI → render).** The mailbox keeps only the newest snapshot, so one the
  render thread never acquired would take its damage with it. `snapshot_buffer` keeps an
  `atomic<bool> consumed_` per slot, set by `acquire()`; `publish()` folds the damage of an
  unconsumed previous snapshot into the new one. The race with a concurrent `acquire` can
  only composite a rect twice, never skip one.
- **Buffer age (render only).** Render keeps, per scanout slot, the damage that slot has
  missed since it was last drawn into, and composites `frame damage ∪ slot age`. It also
  keeps the damage since the last successful KMS commit, which is what FB_DAMAGE_CLIPS
  must describe. Neither crosses a thread.
- **Idle frames.** A tick with no damage composites and commits nothing. Frame callbacks
  are then answered from the UI tick once render has consumed every damaged frame
  (`last_rendered_frame_`), holds no composited-but-uncommitted frame
  (`atomic<bool> frame_outstanding_`, lowered only after the commit raised `flip_pending`)
  and no flip is queued.

## Window management (UI affinity)

`window_manager` (`lx.compositor:wm`), `toplevel_manager` and `surface_manager` are touched
only by the UI thread: protocol handlers run inside `wayland.dispatch` in `tick_ui`, and
`window_manager::on_tick` runs once per tick right after it. Window geometry, stacking
order, the configure queue and focus are therefore plain data with no locks. They reach the
render thread only as scene node origins and bounds inside the published snapshot — a
commit that moves a window (left/top-edge resize) updates the node's origin and its buffer
in the same UI call, so one snapshot carries both.

Input takes the same path. libinput is dispatched on the UI thread, and every pointer
event goes synchronously through `window_manager::on_pointer_motion` / `on_pointer_button`,
which hit-test against the same node rects the scene will publish. That keeps focus and
picture consistent without any handoff. A hit test is one pass over the stacking order with
no allocation and no logging. When the scene changes under a still pointer (map, restack, a
window that moved or resized), the WM does not re-test at once: it sets a flag and
re-tests once at the next `on_tick`. An unmap of the focused window is the exception and
re-tests immediately. Stacking changes reach the shell the same way, as one
`stacking_order_changed` batch per tick.

Interactive move/resize grabs are UI state too (`window_manager::grab()`). During a grab a
motion event only records the pointer position (O(1), no client event); `on_tick` applies
it once per frame, before `flush_configures` and before `cursor_manager::on_tick`. A move
updates the record and the node origin; a resize updates the desired configure size, which
`flush_configures` sends only when the previous one has been acked and committed. The
cursor manager publishes its frame at the same tick during a grab (not per event), so the
pointer and the dragged window are in the same snapshot and reach KMS in the same atomic
commit. Escape is intercepted in the key path (`window_manager::on_key`) before
`input_router`.

Latency instrumentation crosses render → UI through one atomic: when a commit carries a
pointer change, render stores `input_us << 1 | content` into `committed_input_us_` (only
if it is 0, so a commit racing the flip handler drops its sample rather than being timed
against the previous flip); the UI-side flip handler exchanges it with 0 and logs against
the kernel flip timestamp. The sample window for percentiles is UI-only fixed storage.

## Pointer cursor (UI → render)

`cursor_manager` (`lx.compositor:cursor`) is UI affinity: input handlers, protocol requests
(`wl_pointer.set_cursor`, `wp_cursor_shape_v1`) and `on_tick` all run on the UI thread. The
KMS cursor plane (`drm::kms_cursor_plane`) is render affinity after `start`. Two
single-producer single-consumer **latest-value triple buffers** (`latest_value<T>`, one
atomic exchange per side, no locks, no allocation) connect them:

| Mailbox | Carries | Written | Read |
|---------|---------|---------|------|
| `cursor_frame_mailbox` | position, visibility, hardware/software, image generation, input time | per motion event outside a grab; at `on_tick` otherwise | once per render tick, just before the commit is built |
| `cursor_image_mailbox` | plane-sized premultiplied ARGB image + hotspot | when the resolved image changes | when the frame's image generation differs from the one staged |

The reader owns the slot it last took until its next `take`, so the image it is copying into
the plane's back buffer can never be overwritten underneath it. Render reports a rejected
plane back through `atomic<bool> cursor_hw_failed_`, which `tick_ui` turns into
`cursor_manager::on_hardware_failed()`; `atomic<bool> last_commit_content_` tells the UI-side
flip handler whether the flip it is handling presented a frame or only moved the pointer.
The software cursor is a scene node and crosses threads in the snapshot like any other.

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
