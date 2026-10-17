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

Selections (clipboard, primary) are UI state with no render involvement: the per-client
input serial log is written by `input_router` as it delivers each event, the focus-client
hook runs inside `input_router::set_keyboard_focus` between the `leave`s and the `enter`s,
and every source, offer and device table in `wire_selection.cpp` is touched only from
protocol handlers and that hook. The data itself never passes through the compositor: a
`receive` hands the reader's fd to the owner as `send` (libwayland dups it) and the
compositor closes its copy at once — no I/O on the UI thread.

Drag-and-drop is UI state too. The grab (`dnd_grab`, Wayland-free) is driven from the
window manager's pointer path, key path (Escape) and modifier path, all on the UI thread;
the wire side (`wire_dnd.cpp`) runs in protocol handlers and in the grab's `dnd_ops`. The
icon is moved once per tick in `window_manager::on_tick` (after the move/resize grab, before
`flush_configures`), and during a drag the cursor manager publishes the pointer at the tick
as for a grab, so icon and pointer reach KMS in the same commit. Render only sees the icon
as nodes in the published snapshot; dropped data passes client to client by fd, never
through the compositor.

Output membership and liveness are UI state. `surface_tree`'s rect hook runs wherever a
node rect is set (a commit's scene sync, a window move at the tick, an unmap), and the wire
sends `wl_surface.enter`/`leave` from it on the UI thread; `tick_ui` compares the output
manager's generation once per tick and only then resends output state. `liveness_tracker`
is driven from the window manager (activation, the configure watchdog, `on_tick`'s O(1)
deadline check), from the router's user-input hook and from `pong` handlers — UI thread
only, fixed storage, no allocation. Its verdict reaches render solely as a node tint in the
published snapshot. xdg-activation is UI state too: tokens are issued and presented by
protocol handlers, the grant reads the router's serial log and keyboard focus, and the
decision (`decide_activation`, pure) runs in the window manager — at the request or at map —
with no render involvement and no allocation (a fixed 64-slot token table).

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
The output scale never crosses: the UI thread projects the pointer into output pixels
(`cursor_manager::set_projection`) before publishing a frame, and resamples client cursor
images to their output-pixel size when they change, so render only ever sees pixels.

**Debug screenshot (UI → render).** `compositor::request_screenshot` (SIGUSR1) stores
`screenshot_requested_` (async-signal-safe) and wakes the loop; the UI thread turns it into
full damage, a frame request and `screenshot_pending_`; render reads back the slot it
composited with a full-frame region and writes the PPM. Two `std::atomic<bool>`, exchanged,
never read on the hot path otherwise (one relaxed load per GL composite).

## Session pause and resume (UI ↔ render)

logind, libinput and the pause/resume state machine (`lx.compositor:session_state`) are UI
affinity; every KMS commit — including the resume modeset — is render affinity.

| Step | Thread | Shared state crossing threads |
|---|---|---|
| logind signals, `SwitchTo`, `ReleaseDevice`, `PauseDeviceComplete` | UI (sd-bus, `logind_bus_source`; callbacks only record, actions run after `sd_bus_process`) | — |
| State machine, libinput suspend/resume, the key and button sweep, keyboard leave/enter, `suspended` configures, the frame clock | UI | — |
| Display gate | UI sets and clears bits of `display_gate_` (release): `gate_vt` (session paused) and `gate_power` (output powered off); render reads it (acquire) at tick start and again right before each commit ioctl, and every reader means "any bit" | `std::atomic<unsigned>` |
| `pause` acknowledgement | UI posts a quiesce task to the render strand; render posts `render_quiesced(gen)` back to the UI strand | executor strands, one producer each |
| Resume | UI stores `resume_gen_` (release), *then* clears its gate bit (release); render loads `display_gate_`, then `resume_gen_` (acquire) — seeing "unpaused" implies seeing the new generation | `std::atomic<unsigned>` |
| Resume procedure (`prepare_resume`, cursor plane invalidation, full damage, the modeset) | render | — |
| Resume result | render posts `(gen, outcome)` to the UI strand; `gen` comes from one UI counter for every resume (VT or power-on), and the UI routes the result to whichever machine asked for that generation | strand |
| Power off | UI posts the power-off task with its generation to the render strand (after the tick, so nothing else commits meanwhile); render compares it with `power_gen_` (acquire; stale → nothing), checks `gate_vt` and `flip_pending()`, makes the blocking ACTIVE=0 commit, and posts the outcome back; "retry after flip" is re-posted by the UI flip handler | `power_gen_` (atomic), strands |
| Flip events (generation cookie check, `flip_pending_` clear, the handler) | UI only | `flip_pending_` (atomic) |

**The flip handler never runs on the committing thread.** A failed commit is reported by
`commit()`'s return value alone; a blocking modeset (a driver that refuses a nonblocking one)
completes without a kernel event, so render posts `dispatch_events()` to the UI strand, which
reports it as the flip.

**Presentation feedback across the flip (render → UI).** Render tags each content commit
with the frame index of the snapshot it composited (`atomic_commit_request::user_tag`,
render-affine `composited_frame_`). `kms_atomic_commit` stores the tag and "carries a frame"
in two atomics *before* the ioctl that raises `flip_pending_` (the event can be drained on
the UI thread before the ioctl returns; a CRTC takes one flip at a time, so the in-flight
commit is unambiguous) and the UI-side flip handler reads them with the kernel's sequence
and timestamp. Whether the driver timestamps vblanks is probed once by render after the first
modeset (`drmCrtcGetSequence`) and published through an atomic. Everything else of
wp_presentation — the visibility pass, the feedback table, sampling at publish, the events —
is UI affinity; the pause edge discards what is in flight on the UI thread, and a flip
drained after it is ignored by the paused branch of the handler.

**The paused tick.** While paused the frame clock is stopped and `request_frame()` is a
no-op; the housekeeping timer runs at 1 s and calls `on_vsync()` itself: one `tick_ui`
(dispatch, configures, scene publish, buffer retirement, frame callbacks answered
unconditionally) and one `tick_render` that only drains texture updates and publishes the
frame index — no composite, no commit. Hot-path cost of the gate: one acquire load per tick
and one per commit; no lock, allocation or log. The paused tick also discards queued
presentation feedback, so clients pacing by it are not left waiting on a display that is off.

**Display power** reuses all of it. The off edge sets `gate_power` on the UI thread (the
frame clock stops, the paused tick runs, presentation feedback is discarded, windows are
`suspended`), then the render thread's power-off task turns the CRTC off with one blocking
`ALLOW_MODESET` commit — render stays the only committer. Input is not suspended: libinput
events keep arriving on the UI thread, and the first real one wakes the output (a new resume
generation, then the gate bit cleared). The idle tracker, the inhibitor evaluation after each
publish and the idle timer are UI affinity; per input event they cost one clock read and a
store.

**Session lock** is UI affinity throughout: the state machine, the input gate, hiding the
normal layers (scene mutation, published with the next snapshot), the lock timer (the 1 s
wait for lock surfaces, the 2 s sleep delay) and logind calls. The render thread sees only a
snapshot whose clear colour is the lock's and whose draws are the lock surfaces. `locked`
waits for the render thread's work to reach the screen through the existing flip path: the
UI flip handler compares the flip's frame tag with the first locked frame. One atomic is new,
`ever_lit_` (render stores it with release after its first successful content commit; UI
loads it with acquire): before it, nothing of the session was ever on the CRTC, so a lock
completes at once.

**Text input and input methods** are UI affinity throughout: the relay (`:text_input`), the
router's grab routing and virtual-keyboard keymap switching run inside the protocol dispatch
and the libinput key path, and the input-method popups are ordinary scene trees mutated on
the UI thread and published with the next snapshot. Nothing new crosses to the render thread;
per key the cost is one branch while no input method is active.

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

No thread polls. The UI loop blocks in `epoll_wait` on every fd it serves (Wayland display,
DRM card, libinput, the logind bus), one `timerfd` armed at the earliest timer deadline
(absolute, CLOCK_MONOTONIC) and one `eventfd` that `event_loop::post`, `quit` and posts to
the UI strand write. A flip wakes it the moment the card turns readable. The render thread
waits for a tick's snapshot or a post to its strand; the worker only for a post (each
strand's `set_waker`). An `fd_source` with a nonzero `poll_interval_ns` still dispatches on
a period, for an fd whose readiness cannot be trusted; nothing uses one.

**The frame clock runs on demand.** Two consecutive ticks with nothing to do stop it.
Nothing to do means no damage, nothing the render thread has not reached, no flip or commit
outstanding, no frame callback or buffer waiting on a frame (one held back by the 995 ms
throttle of a surface that is not on screen does not count: the housekeeping timer answers
those), and nothing the window manager
deferred. A client request, an input event or a flip restarts it at the display's next
deadline (`repaint_scheduler::next_start`). While it is stopped a 1 s housekeeping timer
ticks for the deadlines only a tick notices: liveness pings, the configure watchdog,
unmapping idle staging. An idle desktop measured 730 wakeups/s across the three threads
before this and 5/s after (vmwgfx, shell and one terminal).

Client content crosses UI → render as `texture_update`s on an SPSC queue. Each carries the
index of the snapshot that was being built when it was queued, and the render thread
applies it no earlier than when it composites that snapshot (later ones wait, in order):
pixels committed after a publish never reach the screen under the previous snapshot's
geometry. Surface trees (`lx.compositor:surface_tree`) are UI-affinity state like the
scene graph they edit.

### Vsync wiring (current → future)

**Current (P0/P1):** DRM/KMS page-flip when `kms_device::open()` succeeds; `timer_source` remains
fallback for headless/CI. `hotplug_monitor` refreshes outputs each UI tick. A repeating
`timer_source` advances from its deadline, skipping missed periods; re-arming at
`now + interval` let every tick's lateness push the rest back (60 Hz measured 57).

### CLI

```bash
lumen-compositor [socket-name] [-f fps] [--scale S]
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
