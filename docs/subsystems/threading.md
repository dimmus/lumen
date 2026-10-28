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

**Several outputs (ADR 008).** Every output slot has its own view in the scene and its own
`snapshot_buffer` (`commit_frame(slot, frame)`, `acquire_render_snapshot(slot)`,
`release_render_frame(slot, …)`), created by UI with the view and never freed while the
scene lives, so render never creates one. UI publishes only the due outputs with damage and
ORs their bits into `atomic<uint32_t> render_mask_` (release); render exchanges the mask
(acquire) and acquires exactly those views. One output's flip-pending slots never hold back
another's publish.

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
  are then answered from the UI tick once render has consumed every damaged frame of that
  output (the pipeline's `atomic<long long> rendered_frame`), holds no
  composited-but-uncommitted frame (`atomic<bool> frame_outstanding`, lowered only after the
  commit raised `flip_pending`) and no flip is queued on it. With content, they are answered
  by the UI's flip handler of the surface's pace output.
- **Buffer release (render → UI).** Client buffers are released at the **low-water mark**,
  `atomic<long long> render_low_water_`: the oldest frame any live output may still sample
  (its newest consumed publish, or the frame its composited-but-uncommitted slot holds).
  Render stores it after each tick; UI feeds it to `retire_completed_frames()`.

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
| `cursor_image_mailbox` | a set of plane-sized premultiplied ARGB images + hotspots, one per distinct output scale (≤ 4) | when the resolved image changes | when the frame's image generation differs from the one staged |

The reader owns the slot it last took until its next `take`, so the image it is copying into
the plane's back buffer can never be overwritten underneath it. Render reports a rejected
plane back through `atomic<bool> cursor_hw_failed_`, which `tick_ui` turns into
`cursor_manager::on_hardware_failed()`; `atomic<bool> last_commit_content_` tells the UI-side
flip handler whether the flip it is handling presented a frame or only moved the pointer.
The software cursor is a scene node and crosses threads in the snapshot like any other.
The cursor frame is in logical layout coordinates; render places it on each pipeline's
cursor plane with that output's origin and scale (and hides it on outputs it is not on),
using the image of that output's scale. UI resamples client cursor images to each scale
when they change, so render only ever copies pixels.

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
unconditionally) and, when it has work (see the housekeeping tick above), one `tick_render`
that only drains texture updates and publishes the frame index — no composite, no commit. Hot-path cost of the gate: one acquire load per tick
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

No thread polls. The UI loop blocks in `epoll_pwait2` on every fd it serves (Wayland display,
DRM card, libinput, the logind bus) and one `eventfd` that `event_loop::post`, `quit` and
posts to the UI strand write, with the time to the earliest timer deadline as its nanosecond
timeout. A timer wake is then that one syscall; an absolute `timerfd`, which kernels before
5.11 still use, cost a `timerfd_settime` before each wait and a `read` after each fire. The
loop sets the UI thread's timer slack to 1 µs so the timeout keeps a timerfd's precision,
and reads the clock once before the sources prepare and once after the wait, not once per
timer source. With an animating client on a virtual output the main thread went from 11.3
to 8.5 syscalls per frame. The Wayland dispatch inside a tick and in the display fd's
callback do not flush: the tick flushes at its end and the loop once per iteration, so a
buffer release leaves with the frame's events. A flip wakes it the moment the card turns readable. The render thread
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
before this and 5/s after (vmwgfx, shell and one terminal). The housekeeping tick wakes the
render thread only for work: at the end of each tick render reports whether anything of its
own is left (a flip or frame waiting, leftover damage, targets kept around, a deferred op,
texture retirement, capture job or resume) together with the count of UI posts it started
from, and the tick also checks what UI handed over since (a publish, texture updates, output
ops, a cursor, a capture, a screenshot). An idle compositor went from 2.1 to 1.0 wakeups/s;
frame ticks always wake render, as before.

**Start-up runs four helper threads, before the render and worker threads exist.**
`compositor::start` is on the UI thread, and its slow phases mostly wait on something else,
so they overlap instead of queueing:

| Helper | Runs beside | Joined |
|--------|-------------|--------|
| hardware-Vulkan probe (`device_selector::probe_devices`: the devices and their DRM nodes; which one counts is chosen from the card after the join) | logind session and KMS open (D-Bus, kernel) | before the present path is chosen |
| input open (`begin_input`: keymap compile, libinput, logind `TakeDevice` per device) | EGL/GL set-up and the scanout targets | `finish_input`, after the globals |
| cursor theme at the default output scale (`begin_cursor_theme`) | the same | the cursor set-up, which adopts it |
| title font (`begin_title_font`: resolves and shapes the title bars' font, so fontconfig starts here on a persisted-match miss) | the same | the end of `start`; the face cache's mutex orders it with any title shaped earlier |

Each owns its state until joined (the title font writes only the process-wide face cache,
under its mutex), and the UI thread uses neither `logind_` nor
`input_devices_` in between; a guard joins them on every early return. The probe's
`gfx::hardware_probe_scope` sets `VK_LOADER_DRIVERS_DISABLE` (software drivers cannot
answer the probe and lavapipe's LLVM is most of its cost) while only the UI thread and the
probe run, and clears it after the join, so no thread reads the environment while it is
written. Measured on vmwgfx (2 vCPUs, `LUMEN_TRACE_FILE` start-up instants): the Vulkan
probe 18–31 ms, input open 15–25 ms and the cursor theme 5–14 ms left the critical path;
input in parallel against serial, 8 interleaved runs each: socket 75 vs 92 ms, first client
frame 114 vs 126 ms.

### Thread scheduling

Under load a plain SCHED_OTHER thread waits its turn behind every other runnable task, and a
frame that waits misses its vblank. Once the threads have started, `compositor::start`
raises the render and UI threads (`compositor_impl::apply_thread_scheduling`, UI thread; the
render thread publishes its tid as it starts). `runtime::apply_thread_sched` (`lx.runtime:sched`,
pure over injected calls) walks the tiers best first and keeps the first one granted;
`kernel_sched_ops` are the syscalls, `lx.session::rtkit_client` the broker. One info line per
thread says what it got (category `compositor.sched`; the refusals at debug).

| Tier | How | Render | UI (dispatch) |
|------|-----|--------|---------------|
| real-time | SCHED_RR 1 with the process's own CAP_SYS_NICE / RLIMIT_RTPRIO, else rtkit `MakeThreadRealtimeWithPID` (within its `MaxRealtimePriority`) | yes | **never** |
| high priority | nice −10 with `setpriority`, else rtkit `MakeThreadHighPriorityWithPID` (not below its `MinNiceLevel`) | when not real-time | yes |
| latency hints | `sched_setattr` keeping policy and nice: EEVDF slice 0.5 ms (`sched_runtime`, 6.12+) and `uclamp.min` 512 (5.3+) | slice and uclamp.min | slice only |

- **Which thread goes real-time.** Render work is bounded per frame (composite, present). The
  UI thread runs whatever clients send, config reloads, I/O: a flood there would be a
  real-time thread starving the machine, and RLIMIT_RTTIME would then end the session. So
  UI goes as far as high priority (Mutter likewise keeps only its KMS thread real-time). The
  worker, the start-up helpers and driver threads stay as they are.
- **SCHED_RR, priority 1.** The lowest real-time priority is above every SCHED_OTHER task,
  which is the point, and below everything that chose a priority on purpose (PipeWire's
  rtkit 20, IRQ threads at 50). RR over FIFO: it is what rtkit grants anyway, and a thread
  of equal priority still gets a turn. KWin does the same (`SCHED_RR | SCHED_RESET_ON_FORK`,
  the minimum priority).
- **RLIMIT_RTTIME first.** Before real-time is asked for (by either route) the hard limit is
  set to 200 ms (rtkit's default `RTTimeUSecMax`, lower if rtkit's is lower; a lower one
  already set is kept) and the soft limit to half of it; nothing is asked if that fails.
  A SIGXCPU handler (the soft limit) drops every real-time thread back to SCHED_OTHER — one
  line on stderr, async-signal-safe — before the hard limit's SIGKILL; the process-wide
  signal is why the guard knows the threads. The limit counts only real-time CPU time
  without a blocking call: the render thread blocks every frame.
- **SCHED_RESET_ON_FORK on every raised thread**, set before anything else (nothing above
  the hints is asked for without it): threads and processes started from them begin at
  SCHED_OTHER, nice 0, the default slice and no clamp (checked on 7.2: the slice and uclamp
  reset too). The compositor starts no processes itself; Mesa and other libraries start
  threads.
- **No hints on a real-time thread.** A uclamp.min would lower the full clock RT tasks get
  by default; the slice is a fair-class parameter.
- **What an unprivileged process may set** (tried on Linux 7.2): `uclamp.min` up and down
  and `sched_runtime` (clamped to 0.1–100 ms) on its own threads; negative nice and any RT
  policy are EPERM. Before 6.12 `sched_runtime` is ignored for SCHED_OTHER and reads back
  0, so the outcome reports slice 0; without `CONFIG_UCLAMP_TASK` the clamp is dropped and
  the slice still applied.

`compositor.realtime = "off"` (config, read at start-up) leaves both threads as the process
started. rtkit is reached on a private system-bus connection, only when the kernel refused,
with a 1 s timeout, and closed at the end of start-up. Measured on the vmwgfx VM
(RLIMIT_RTPRIO 0, no rtkit): both threads get latency hints — render slice 500 µs and
uclamp.min 512, UI slice 500 µs.

**Frame callbacks are released on a timer (UI).** After a flip, an output's callbacks are
held until its next repaint start minus what its clients recently took to answer (peak of
the last 8, plus 1.5 ms) and released by `callback_timer_`, a one-shot UI `timer_source`;
the answer time is measured on the UI thread from the release to the first dispatch that
damages the output. See rendering-performance.md (frame clocks). A tick that finds
callbacks still held answers them itself, and pause, power-off and removal drop the hold.

Client content crosses UI → render as `texture_update`s on an SPSC queue. Each carries the
index of the snapshot that was being built when it was queued, and the render thread
applies it no earlier than when it composites that snapshot (later ones wait, in order):
pixels committed after a publish never reach the screen under the previous snapshot's
geometry. Surface trees (`lx.compositor:surface_tree`) are UI-affinity state like the
scene graph they edit.

### Vsync wiring (current → future)

**Current (P0/P1):** DRM/KMS page-flip when `kms_device::open()` succeeds; `timer_source` remains
fallback for headless/CI. Outputs follow hotplug (below). One shared `vsync_timer` serves every output's frame clock (`frame_clocks`): it is
armed at the earliest output start, a tick serves the outputs due within 1 ms of it, and a
flip event re-phases only its own output's clock. Flip events are read once per DRM device
(`kms_device::dispatch_events`) on the UI thread and routed to the pipeline by CRTC. A repeating
`timer_source` advances from its deadline, skipping missed periods; re-arming at
`now + interval` let every tick's lateness push the rest back (60 Hz measured 57).

### Hotplug and output plans (ADR 008 Part B)

| Step | Thread | What |
|------|--------|------|
| udev `change` HOTPLUG=1 | UI | the netlink fd is a loop source (`hotplug_controller`); `drain` folds a burst into one batch |
| Connector probe | worker | `kms_device::probe_connectors(hint, scan, record=false)` into the worker's own `connector_scan`; one in flight, the rest merged into the next; the result goes back through `event_loop::post` (the worker is that queue's one producer) |
| Diff and policy | UI | `diff_scan` → added / removed / changed; removals first, then mode changes, then additions; an identical scan does nothing |
| Output ops | UI → render | `output_op` (add, test_mode, switch_mode, retire, set_vrr) in a mutex-guarded array of 32, read at the start of `tick_render` and again right after the snapshots are acquired; retires run first, a KMS add (and later ops of its slot) waits on render while a retiring CRTC is lit |
| Answers | render → UI | `on_add_result`, `on_mode_tested`, `on_retired` as UI tasks (render is the UI strand's producer) |

Ownership of a pipeline crosses threads only by message. Render owns `pipes_`; UI reaches a
pipeline through `outs_[slot].pipe`, set by the add's answer and cleared **before** UI
asks for the retire, so neither thread ever sees one the other has let go of. UI never
passes that pointer in a task: render's copy of an output's projection (origin, scale,
transform) is posted by slot and looked up in `pipes_` on render. A retired
pipeline leaves the device's event registry on render (no flip is routed to it, no
`commit_modeset` counts its CRTC) and is destroyed on UI — the thread draining KMS events,
which may hold it until its dispatch returns. GL objects of a retired or switched pipeline
are released on render, where the context is current. A mode switch is tagged with the
first frame built at the new size: render applies it right before compositing that frame
and never composites an older one into the new targets (snapshot publish is a release,
acquire an acquire, so the op is visible whenever its frame is). The old targets stay
until the new mode's first flip. While the VT is away nothing is probed (only the master's
probe is forced); the resume waits ≤ 250 ms for a full probe before its modeset.

### Output configuration (ADR 008 Part C)

| Step | Thread | What |
|------|--------|------|
| Configuration requests | UI | `wire_output_management.cpp` checks every protocol error and queues a passed apply/test (`mgmt::job_queue`, a copy of the configuration) |
| Validation | UI | `mgmt::resolve` against the heads clients were last told about; one job at a time, none while an output is being added or retired |
| TEST_ONLY | UI → render → UI | one `config_test` (a single slot under `ops_mutex_`) taken by `run_output_ops`: staged targets at new modes (kept in the pipeline for the switch), disables, new outputs through the CRTC allocator; the answer is a UI task (`on_config_tested`) |
| Switch | UI | the logical state, then the usual ops (`add`, `switch_mode`, `retire`, `set_vrr`) carrying a ticket |
| Commit | render → UI | the combined modeset posts each ticketed pipeline's outcome (`on_modeset_result`); a timer bounds the wait (3 s) |
| Save | UI → worker | `outputs.toml` serialised on UI 2 s after a client's apply; written on the worker (write, fsync, rename) — UI is that strand's one producer; flushed synchronously before the threads stop |

A job's serial is checked at its start and again when its test answers (a hotplug in
between makes it `cancelled`); after the switch it is answered from the commit, whatever
moved meanwhile.

### Content updates and fences (ADR 009 Part A)

| Step | Thread | What |
|------|--------|------|
| Commit, queue, apply | UI | `wl_surface.commit` applies an update at once or queues it (`:content_update`); every apply — at commit, from the poller, at a tick — runs on UI and goes through `apply_cached`, so one snapshot shows a whole update |
| Fence waits | UI (event loop) | One epoll fd (the readiness poller) holds every sync_file (implicit dma-buf fences) and syncobj eventfd (explicit acquire points) a queued update waits on; it is one `fd_source`, so a signalled fence wakes the UI loop, never a thread |
| fifo / commit-timing | UI | `tick_ui` tries timed updates after its dispatch (they join the frame being built), clears fifo barriers after each served output's publish (`content_updates_latched`), and arms stopped clocks for targets (`frame_clocks::request_at`) |
| Release points | render → UI | Render publishes a dup of each composite's out-fence into `release_fence_mailbox` (a mutex around one fd, only while a client uses explicit sync), and after a tick's capture composites that sampled client textures a fence of those; UI dups it when the buffer lifecycle retires a buffer (superseded, and past the low-water mark, which window capture views count towards and a view job not yet taken by render holds back) and attaches it to the release point — no thread waits on a fence |

### Window and session protocols (ADR 009 Part B)

All UI affinity, nothing per frame. Security-context listeners are fd sources on
libwayland's own event loop (`wl_event_loop_add_fd` on the engine's listening socket and its
close fd): the display's loop fd is what the runtime loop already watches, so a sandboxed
connection is accepted inside `server::dispatch` on UI and costs no runtime-loop slot. Icon
snapshots copy at most four buffers (≤ 256 KiB each) into memfds at `set_icon`, on UI, once
per icon change. The shell (its own process, one thread) maps those memfds read-only and
decodes theme icons on its thread when a task's icon changes, cached per (name, size).

### Pointer protocols (ADR 009 Part 3)

UI affinity throughout, inside the libinput dispatch: `on_input_pointer_motion` asks the
active pointer constraint what the motion may do (a lock keeps the position, a confinement
clips it), sends relative motion to the pointer focus's client and gestures to the client
latched at begin — all before or instead of the window manager's hit test, with no
allocation and nothing posted to another thread. Render only ever sees the cursor position
the UI published, so a locked pointer is simply never republished. Constraint state
changes (regions, cursor hints) apply with the surface's content update on UI and take
effect at the end of the commit request or of the loop iteration (`after_dispatch`); a
warp is a synthetic motion on UI.

### Screen capture (ADR 009 Part 4)

| Step | Thread | What |
|------|--------|------|
| Protocol, policy | UI | Sessions, frames, constraints and `capture_decide` (`wire_capture.cpp`, `:capture`) once per tick after the publish and when a frame is requested; window sessions publish their capture view (scene views 16–23) in that tick, so it is built from the same UI state as the outputs' frames |
| Jobs | UI → render | `capture_channel`: a fixed queue under one lock (a dup'd dma-buf plane or a guarded shm mapping per job); posting wakes render. Never dropped: a job that does not fit waits for the next tick |
| Copies | render | On the output's composite (`composite_output`) or, for a first frame or a pointer move, from its newest composited slot; window views composited from their own snapshot after the drain of the texture updates its frame may show. GL objects of capture (`gl_capture`: dma-buf framebuffers, offscreen buffers, pixel-pack buffers) are created and destroyed on render only |
| shm read-back | worker | A shm frame is blitted on render into the session's LINEAR GBM buffer behind a native fence; one worker task waits on the fence, maps the buffer and copies the damaged rows into the client's shm under its guard, completes on the channel and posts the UI task. The session is busy until then: render skips its next job, and a session closed meanwhile is torn down by render once the worker let go. Without workers (or GBM) the read goes through a pixel-pack buffer and a second render job. Vulkan: render records a copy of the changed rects into the session's persistent mapped buffer and submits it with its own fence, no wait; the worker waits on the fence and writes the rows |
| Client shm | render or worker, under a guard | `capture_shm_guards`: render (or the worker read) writes a client's shm only inside the entry's lock and libwayland's access guard; the wl_buffer's destruction (UI) cancels the entry under the same lock, so it waits for a copy in progress and render never touches the memory afterwards. The pool reference is dropped on UI with the job's final completion |
| Completions | render → UI | `capture_channel` again; render posts one UI task when it has some (and `tick_ui` drains them first). A GPU copy's or read-back's native fence goes to the readiness poller (one `fd_source`, keys above every content-update index); its wake sends `ready`, or posts the read-back's copy-out job back to render |
| Lock | UI → render | A lock request bumps an atomic epoch; render cancels a content job issued in an older epoch instead of running it (it would copy a frame from before the lock), and UI decides again a completion carrying an older epoch (a worker read that finished after the request) |

### Session, control socket and health (ADR 011)

| Step | Thread | What |
|------|--------|------|
| Control socket | UI | sd-varlink on a private sd-event loop; its epoll fd is one `event_source` of the UI loop (`control_source`), dispatched with `sd_event_run(e, 0)` until idle — never blocking. Every method handler runs on the UI thread with full access to compositor state; JSON is built per request only. A reply or stream event queued outside a dispatch "kicks" the source for one more run |
| Session environment | UI | The user bus (sd-bus) is attached to the same sd-event loop: `SetEnvironment` / `UpdateActivationEnvironment` are asynchronous (READY from the reply callback); only the `UnsetEnvironment` at exit is synchronous (1 s), after the loop ended |
| GetHealth | UI → render → UI | The UI thread posts a task to render, which reads its own figures (import cache, last render tick, budget counter) and posts them back; the UI thread completes the report. No answer in 500 ms (an sd-event timer): the report says `render: critical` |
| Watchdog | UI, render heartbeat | After each loop iteration the UI thread pings when a quarter of `WatchdogSec` passed, only if `render_ticks_` moved since the last ping or the render thread is not inside a tick (`render_in_tick_`, two relaxed stores per render tick). The control source's timeout carries the next ping only if nothing else wakes the loop first |
| Signals | any → UI | SIGHUP sets an atomic and wakes the loop (`request_reload`); RELOADING/READY are sent from the UI thread |

### Configuration (ADR 011 Part 2)

| Step | Thread | What |
|------|--------|------|
| Watch | UI | One inotify fd (`lx.config:watch`) is an `fd_source` of the UI loop; a change arms a 100 ms one-shot timer (the debounce). It sleeps while the files do not change |
| Read and parse | UI | Each layer read with `read(2)` (≤ 256 KiB, page cache) and parsed into a heap snapshot (~0.1 ms for a few KB); at start-up before the input and cursor helper threads are started, which take the keymap names and the cursor size from it |
| Keymap | UI → worker → UI | A changed layout is compiled on the worker into its own `keyboard_keymap` (a fresh xkb context; ~10 ms); the UI task swaps it into the seat (`adopt_state`: locks by name, the locked layout clamped, held keys pressed again) and sends it to every keyboard. Keys meanwhile use the old keymap |
| Cursor themes | UI → worker → UI | The themes for every output scale in use are read on the worker into a job object; the UI task moves them into the theme slots the cursor manager points at and asks for a frame |
| Report | UI | Once the generation's jobs are done: the log, the shell's zlm v7 events, the control socket's Reload replies, READY after SIGHUP. A reload asked for meanwhile runs after it, and its Reload replies and READY come from that later generation (they must see the files as they were when asked) |
| Without a worker | UI | (`worker_thread_count` 0, or before the threads start) the jobs run inline |

### UI text shaping (ADR-010 §4.6)

| Step | Thread | What |
|------|--------|------|
| Small text (< 4 KiB, `lx.ui::k_text_async_min_bytes`) | UI | `text_block_node::apply` sets the layout's inputs; the shape and wrap passes run lazily on the UI thread at the next measure |
| Large text | UI → worker → UI | `apply` copies the inputs into the node's job; the worker runs the shape + wrap passes into a staging `text_layout` with its own shape cache and itemizer over the block's face cache (`text_async_shaping`); the UI task swaps it in (`rebind_engine`: no reshape) and invalidates the node's layout. The previous layout stays on show until then; a change meanwhile posts one more job when this one lands; text that drops below the boundary is laid out inline and the running job's result is dropped |
| Face lock | worker ↔ UI | The worker's shape pass holds `face_cache::lock()` but hands it to a waiter between paragraphs (`face_cache::contended`): the UI thread rasterising a glyph waits at most one paragraph (1.3 ms worst seen against a 150 KB pass, `test_text_node`) |
| Node destroyed meanwhile | UI | The job outlives the node and is freed by its UI task |

### Compositor title glyphs (ADR-010 §4.11)

| Step | Thread | What |
|------|--------|------|
| Title change | UI | `decoration_manager` lays the title out (`chrome_text`) and rasterises its new glyphs into `chrome_atlas` under the atlas's mutex — only on a glyph-cache miss |
| Frame emission | UI | `frame_node` looks each glyph's tile up (`atlas::use`: the tile table is UI-only, no lock) and emits a sprite into the snapshot |
| Paint | render | A frame whose stream has sprites holds the mutex while `:ui_cpu` samples the pages in place or `:ui_gl` / `:ui_vk` upload their dirty rects (and clear the lists); captures and the headless dump likewise |
| Tile reuse | UI | A tile a published frame names is reused only after the atlas ages it out (120 frames unused); a page rebuild moves tiles under the mutex and damages every frame |

### Workspaces (ADR 011 Part 3)

Everything is UI-affinity and event-driven: the workspace model (`:workspaces`, fixed
tables), the window manager's visibility changes (a tree container's `set_visible`, damage,
the configure carrying `suspended`), ext-workspace's per-manager diff and `done`, and the
shell bridge's `toplevel_workspace` all run inside the operation that changed something (a
binding, an ext-workspace commit, a map or unmap, a hotplug) and are flushed at its end (the
loop's after-dispatch hook flushes anything left). Nothing runs per frame; the render thread
only sees fewer visible nodes in the snapshots it reads. A capture view still draws a hidden
window's tree (`build_draw_list_into`, UI), and a hidden window's commits damage only the
capture views.

### Switching and snapping (ADR 011 Part 4)

UI affinity throughout. A held action is a `keybindings` field and a `zlm_held_action_v1`
resource; diverted keys go out from `on_input_key`, their repeat rides the binding-repeat
timer (armed only while a key repeats). Snapping, edge zones and drag-to-restore are window
manager state changed on input: an edge zone is one rect test on the tick that applies the
coalesced move (`apply_grab`), the preview a scene node whose two 1×1 textures are staged once
through the compositor's texture ops (the render thread draws them from the snapshot like any
texture). Keyboard grabs warp the pointer through the compositor's `warp_pointer` (seat,
cursor, then the window manager's motion path). In lumen-shell the switcher is part of its one
thread: a timerfd slot for the open delay, the view's buffers allocated when it opens.

### Notifications and the tray (ADR 011 Part 5)

lumen-shell's one thread again; the compositor is not involved beyond its existing
protocols. The user bus is one `sd_bus` whose fd joins the shell's `ppoll` set (its reply
deadline lowers the timeout) and which is processed after every wake-up; every D-Bus method
of the notification server and the watcher is answered inline, every reply (an item's
`GetAll`, a menu's `GetLayout`, launches' `StartTransientUnit`) handled as it arrives. An
item's change signals only mark it; one `GetAll` per marked item follows in the same loop
iteration. Notification expiry is one timerfd slot (the next running deadline), a tooltip's
delay another; both are armed only while due. The popups, menus and tooltips draw after the
dispatch like the panel (at most one commit per frame callback each). The image-data
conversion and the body's wrap run on the thread that received them — bounded (≤ 4096²
validated, kept ≤ 96², ≤ 4 body lines of ≤ 8 KiB markup) — never per frame.

### CLI

```bash
lumen-compositor [socket-name] [-f fps] [--scale S] [--session] [--no-control]
```

## Client process: lx.app (ADR-010 Phase 4)

An application built on `lx.app` has one **UI thread** and one **worker thread**; there is no
render thread in a client (ADR-010 §4.12: one window, one queue — a render thread is not
justified until a submit costs more than 300 µs).

| Thread | Runs | Wakes on |
|--------|------|----------|
| UI (`main`) | `lx::runtime::event_loop`: the display (`wayland::display_source`: prepare_read, flush, read or cancel), each window's frame source, describe / reconcile / layout / emit, record + submit on the window's tier (`ui_cpu` / `ui_gl` / `ui_vk`), commit | the display fd, one timerfd, the swapchain's release eventfd (explicit sync), the theme file's inotify fd (hot reload), the UI strand's eventfd |
| worker (`lx-app-worker`) | `executor::global()`'s worker strand: icon rasters (`icon_cache::set_async`), theme reloads (`theme_watcher`), large-text shaping | the strand's waker (a condition variable); never on a period |

**Frame loop** (§4.9). An event or a reducer leaves work in lx.ui's ledger; the window's
frame source (an `event_source` with no fd, due when a frame should be drawn now) sees it in
the same loop iteration. With no frame callback outstanding the frame is drawn at once; with
one outstanding it waits for it — every drawn commit requests the next callback, which is the
throttle. A configure or a preferred-scale change is answered at once. Pointer motion is
coalesced: only the latest position is kept and hit-tested, once per frame, or before a
button, axis or key.

**Timer.** One timerfd, armed after each loop iteration at the earliest deadline anything
needs — a held key's repeat (`wl_keyboard.repeat_info`), a UI timer still ahead (a tooltip's
hover delay, a caret's blink) — and disarmed when nothing does. A UI timer already due is the
frame loop's (now, or at the callback it waits for); it never re-arms the timer for the past.

**Idle (G1).** No ledger entry, no animation, no timer, no key held: no frame callback is
requested after the last frame's, nothing is submitted, the timerfd is disarmed and the loop
blocks in `epoll_pwait2` with no timeout. `test_app_window` measures it: 2 s idle, 0 requests,
0 commits, 0 timer expirations, 0 socket reads, one wakeup (the test's own quit timer).
`lumen-ui-bench --client --scenario S1` measures it from outside the loop too: over 10 s idle
the window's process switched once in total over all its threads (the quit timer's wake;
`/proc/self/task/*/status`), with 0 frame callbacks requested, 0 commits, 0 GPU submits, 0
timer expirations and 0 socket reads, on the cpu, gl and (lavapipe) vulkan tiers
(`tests/bench/baseline/iritur-client-{cpu,gl}.json`).

**Frame record.** Every committed frame leaves a `frame_record` in a 256-entry ring: start,
commit, the UI thread's CPU time over the frame (`CLOCK_THREAD_CPUTIME_ID`), the stages
(input flush, geometry, acquire, tick and its describe / reconcile / layout / emit, order,
render, present) and, when wp_presentation answers, the presented time. The bench reads it;
nothing on the frame path allocates for it.

`event_loop::run` drains a wake token left from before it started (the `quit` that ended a
previous run), which otherwise cost one spurious wakeup per run.

## Process-level vs thread-level

| Level | Model |
|-------|--------|
| **Process** | Compositor / shell / apps (Wayland) — always separate |
| **Thread** | UI + render + worker within compositor and app processes |
| **Portal backend** | xdg-desktop-portal-lumen: one thread — a `poll` over the session bus, a signalfd and, only while a request runs, that request's Wayland connection; no timers, so an idle backend never wakes. The compositor's side of its captures is the capture engine above (UI → render → worker → UI) |

## Embedded / low-core mode

On single-core or kiosk targets, compositor may **merge** `ui + render + wayland` onto one thread. API stays the same; executor affinities collapse to one physical thread (sequential drain).

## UML

- [22-thread-model.mmd](../uml/22-thread-model.mmd) — thread diagram
- [24-executor-class.mmd](../uml/24-executor-class.mmd) — class diagram
- [23-frame-handoff-sequence.mmd](../uml/23-frame-handoff-sequence.mmd) — frame sequence
- [47-client-frame-loop.mmd](../uml/47-client-frame-loop.mmd) — an lx.app window's frame (client process)
