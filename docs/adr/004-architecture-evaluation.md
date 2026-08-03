> **Status:** Proposed — evaluation of the v0.3 tree against the goals
> "solid, fast, efficient, bleeding edge, future oriented"

# ADR-004: Architecture evaluation and correction plan

**Date:** 2026-08-03
**Scope:** whole tree (17.5 kLOC across 24 modules, 70 `.cppm` units)
**Deciders:** project owner

---

## 1. Context

Lumen is a greenfield Wayland stack in C++26 modules. The documented architecture
(`docs/architecture.md`) describes a multi-process, Vulkan-first, threaded compositor
with a snapshot handoff, arena memory, budgets, and a broad protocol roadmap. The code
under `modules/` implements the shape of that description, but the load-bearing seams do
not yet carry the semantics the design promises.

This evaluation separates three things that the current docs conflate:

| | Meaning | State |
|---|---|---|
| **Declared** | Written in docs/manifest | Broad and modern |
| **Scaffolded** | Type exists, body returns `not_implemented` | 58 sites, honestly marked |
| **Implemented-but-wrong** | Body exists, is called, and is incorrect | **The real risk** — 6 findings below |

The third category is what this ADR is about. Category 2 is a schedule item; category 3
is a correctness and design item, and several of the items are structural — they cannot
be fixed by finishing a stub because the surrounding contract is wrong.

**Verdict up front:** the module graph, the process split, the layering enforcement, and
the error model are genuinely good and worth keeping unchanged. The frame pipeline —
scene → snapshot → render → present — is the weakest part of the system and is currently
built in a way that structurally forecloses roughly half of the protocols already listed
in `protocols/manifest.toml`. Fix that seam before adding features.

---

## 2. Findings

### 2.1 Blocking correctness defects

#### F1 — `memory_arena` reports a capacity it does not own (memory corruption)

[modules/lx.runtime/memory.cppm:93](../../modules/lx.runtime/memory.cppm#L93)

```cpp
memory_arena::memory_arena(unsigned capacity_bytes)
    : capacity_{capacity_bytes > 0 ? capacity_bytes : k_inline_capacity} {
    if (capacity_bytes > k_inline_capacity) {
        /* P0: mmap or static pool backing for large arenas */
        capacity_ = capacity_bytes;      // storage_ still points at inline_storage_[65536]
    }
}
```

`storage_` remains the 64 KiB inline array while `capacity_` becomes the configured size.
`allocate()` bounds-checks against `capacity_`, so with the default config
(`ui_arena_bytes = 4 MiB`, `render_arena_bytes = 8 MiB`) the arena will hand out pointers
up to 8 MiB past a 64 KiB buffer. `compositor_impl` constructs `memory_budget_coordinator`
with defaults and calls `reset_frame_arenas()` every `tick_ui`/`tick_render`, so this is
live in the compositor, not dormant.

This is worse than an unimplemented stub: an unimplemented arena would return `nullptr`
and callers would degrade. This one silently corrupts adjacent memory the moment anything
allocates past 64 KiB.

**Fix:** until backing storage exists, clamp `capacity_` to `k_inline_capacity` and log.
Then back large arenas with `mmap(MAP_NORESERVE)` and drop the 64 KiB inline array
(it puts 256 KiB of the coordinator on whatever holds it).

#### F2 — `snapshot_buffer::pick_write_slot()` can hand back a slot a reader holds

[modules/lx.scene/snapshot_buffer.cppm:238](../../modules/lx.scene/snapshot_buffer.cppm#L238)

```cpp
for (unsigned i = 1; i < k_snapshot_slot_count; ++i)
    if (readers_[idx].load(...) == 0) return idx;
return (current + 1) % k_snapshot_slot_count;   // unconditional fallback
```

The fallback returns a slot without re-checking `readers_`. `write_draw_list()` then
returns that slot's `draw_list` and the UI thread mutates it for the whole tick, while
`try_publish()` — which does validate — runs only *after* the mutation. With one render
thread this is currently unreachable; with the second reader the design already implies
(direct scanout evaluation, screencopy via `ext-image-copy-capture`, a second output) it
is a torn-frame data race with no diagnostic.

The write/validate ordering is the deeper problem: validation must happen at slot
acquisition, not at publish.

#### F3 — Backpressure policy is declared but not implemented

[modules/lx.scene/snapshot_buffer.cppm:257](../../modules/lx.scene/snapshot_buffer.cppm#L257)

All three `backpressure_policy` values take the same path — `return false`. `stall_ui`
does not stall, `triple_buffer` does not use the third slot (`max_in_flight = 2` caps
in-flight before the third slot is ever reachable), and `drop_latest` differs only in
whether a counter increments. `publish()` compounds it:

```cpp
void snapshot_buffer::publish(...) {
    if (!try_publish(...) && config_.backpressure == backpressure_policy::drop_latest) {
    }   // empty body
}
```

An empty `if` body is not a placeholder a reader can distinguish from a deletion. The
enum is currently three names for one behavior.

#### F4 — `strand::pending_` is a non-atomic read-modify-write across threads

[modules/lx.runtime/executor.cppm:110](../../modules/lx.runtime/executor.cppm#L110)

```cpp
pending_.store(pending_.load() + 1, std::memory_order_release);   // producer
pending_.store(pending_.load() > 0 ? pending_.load() - 1 : 0, ...); // consumer
```

Load-then-store is not atomic. Producer and consumer are by construction different
threads, so counts are lost. Use `fetch_add`/`fetch_sub`. A counter in the module that
the docs call "first-class threading" needs to be right if the rest is to be trusted.

Related, same file line 115: `drain()` sets `lx_runtime_current_affinity = bound_` and
never restores it. Any thread that drains a strand not its own permanently mislabels
itself, and `assert_affinity` calls `std::abort()` in debug builds. Latent today, a trap
tomorrow.

---

### 2.2 Structural: the scene → render contract discards the frame

This is the most consequential finding and it is not a bug — it is a contract mismatch
that quietly caps what Lumen can ever display.

`draw_command` ([snapshot_buffer.cppm:17](../../modules/lx.scene/snapshot_buffer.cppm#L17))
carries 13 fields. `blit_command`
([modules/lx.gfx/renderer.cppm:11](../../modules/lx.gfx/renderer.cppm#L11)) carries 4.
`tick_render` translates one to the other
([compositor_impl.cpp:656](../../modules/lx.compositor/compositor_impl.cpp#L656)):

| `draw_command` field | Reaches the GPU? | Protocol it is needed for |
|---|---|---|
| `texture`, `dst`, `opacity`, `blend` | yes | — |
| `src` (source rect) | **dropped** | `viewporter` crop |
| `transform` (2×3 affine) | **dropped** | rotation, animation, `wp-*` transforms |
| `clip` | **dropped** | subsurface clipping, layer-shell regions |
| `scale` | **dropped** | `wp-fractional-scale-v1` |
| `buffer_xform` | **dropped** | `wl_output.transform` (rotated monitors) |
| `tint`, `src_space` | **dropped** | `color-management-v1`, `alpha-modifier-v1` |
| `sort_key` | used for sorting only | — |

`tick_ui` already reads `protocols_.fractional_scale(i)` and applies it to outputs —
and then the value cannot reach a pixel. `viewporter`, `wp-fractional-scale-v1` and
`color-management-v1` are all in `protocols/manifest.toml`; none of them is implementable
against the current renderer interface, regardless of how much protocol code gets written.

The shader confirms the ceiling
([modules/lx.gfx/shaders/composite.frag](../../modules/lx.gfx/shaders/composite.frag)):

```glsl
out_color = texture(surface_texture, v_uv) * push.opacity;
```

No transform, no crop, no clip, no color transform. The render target is
`VK_FORMAT_B8G8R8A8_UNORM` ([vk_renderer.cppm:179](../../modules/lx.gfx/vk_renderer.cppm#L179)),
so **all blending happens in 8-bit non-linear space** — mathematically wrong alpha
compositing (visible as dark halos on antialiased edges), and no path to HDR at all.
Meanwhile `lx.foundation` already defines `color_space::{bt2020, scrgb}` and
`transfer_function::{pq, hlg}`. The vocabulary is ready for HDR; the pipeline is 8-bit
sRGB-unaware. For a project positioning itself as future-oriented, this is the gap that
matters most: HDR and color management is the single largest thing happening in Linux
display right now, and the compositor's blending stage is where it is won or lost.

---

### 2.3 Performance architecture

#### P1 — Per-frame CPU↔GPU serialization

`tick_render`'s own comment states it:
[compositor_impl.cpp:690](../../modules/lx.compositor/compositor_impl.cpp#L690) —
"The composite above waits for GPU completion". `vulkan_compositor` blocks on
`vkWaitForFences(..., 1'000'000'000ull)` in the submit path
([vk_renderer.cppm:1125](../../modules/lx.gfx/vk_renderer.cppm#L1125)).

This makes buffer release trivially correct and throughput structurally capped: the CPU
cannot begin frame N+1's work while the GPU finishes frame N, so the pipeline never
overlaps. The correct shape is a timeline semaphore per frame slot, a ring of in-flight
frames, and buffer release driven by timeline value rather than a blocking wait.

#### P2 — Half-megabyte stack traffic per frame

[snapshot_buffer.cppm:194,217](../../modules/lx.scene/snapshot_buffer.cppm#L194) —
`batch_for_render()` puts `unsigned indices[4096]` (16 KiB) **and**
`draw_command tmp[4096]` on the stack. `draw_command` is ~120 B, so that is ~500 KiB of
stack touched on every sort, sized by the constant rather than by `count_`.
[compositor_impl.cpp:654](../../modules/lx.compositor/compositor_impl.cpp#L654) adds
another `blit_command blits[4096]` (~115 KiB) per `tick_render`.

Neither overflows an 8 MiB thread stack, but both evict the working set from L1/L2 every
frame and the sort does two full copies of the array regardless of occupancy. Sort
indices for `count_` elements, or better, sort a 8-byte `(key, index)` array and permute
in place.

#### P3 — One draw call, one descriptor bind, one push-constant per quad

[vk_renderer.cppm:1279-1295](../../modules/lx.gfx/vk_renderer.cppm#L1279). Acceptable for
a compositor with 10 surfaces; not acceptable once `lx.ui` emits draw commands into the
same list — a text-heavy panel produces hundreds of quads per frame. The declared
`k_max_draw_commands = 4096` says the design expects that volume.

#### P4 — Single queue, single output, Vulkan 1.1

- `apiVersion = VK_API_VERSION_1_1` ([lx.gfx.cppm:232](../../modules/lx.gfx/lx.gfx.cppm#L232)),
  legacy `VkRenderPass` + `VkFramebuffer`, `vkCmdPipelineBarrier`. No dynamic rendering,
  no synchronization2, no descriptor indexing. This is a 2018 baseline.
- One `VkQueue` ([lx.gfx.cppm:76](../../modules/lx.gfx/lx.gfx.cppm#L76)): texture staging
  uploads serialize against composite on the same queue.
- `req.connector = 0` hardcoded and a single `scanout_fbs_[]` set
  ([compositor_impl.cpp:702](../../modules/lx.compositor/compositor_impl.cpp#L702)). The
  frame loop composites one draw list into one target and flips one connector.
  Multi-monitor is not a missing feature here — it is a missing dimension in the loop.

#### P5 — Frame pacing is inverted

`on_vsync()` → `tick_ui()` → notify render thread
([compositor_impl.cpp:337](../../modules/lx.compositor/compositor_impl.cpp#L337)).
The compositor builds the frame at the *start* of the interval and presents whenever the
render thread finishes. Modern compositors schedule the repaint to *end* just before the
deadline, sampling input as late as possible — that is the difference between ~2 frames
and ~1 frame of latency.

This is also the blocker for the modern half of the protocol manifest: `wp-fifo-v1`,
`wp-commit-timing-v1`, and adaptive-sync/VRR all require a deadline-driven scheduler that
knows the next presentation time. `lx.scheduler.presentation` exists as the right seam;
the loop is wired the other way around.

The worker thread meanwhile polls on a 16 ms `wait_for`
([compositor_impl.cpp:385](../../modules/lx.compositor/compositor_impl.cpp#L385)) — a
wakeup every frame whether or not there is work.

#### P6 — The executor cannot carry work

[modules/lx.runtime/executor.cppm:15](../../modules/lx.runtime/executor.cppm#L15):

```cpp
using callback = void (*)();
```

A bare function pointer with no context parameter. No captured lambda, no `this`, no
`void* user_data`. Nothing that needs to know *what* to work on can be posted. The
documented purpose of the worker thread — "layout assist, I/O, decode, text shaping
cache" — is unreachable through this API. The worker thread wakes 60× a second to drain
a queue that cannot meaningfully be filled.

This is a one-line-looking fix with a real design choice behind it: a fixed-size
type-erased task (`struct task { void(*fn)(void*); alignas(16) std::byte ctx[48]; }`)
keeps the allocation-free property; `std::move_only_function` does not.

---

### 2.4 Things that are right and should not be touched

Worth stating explicitly, because the list above is long:

- **Layer enforcement in CMake** ([cmake/LumenCXXModules.cmake:35](../../cmake/LumenCXXModules.cmake#L35)) —
  numeric layers with a configure-time rejection of upward dependencies. Most projects
  document a layer graph; this one fails the build. Keep.
- **Process split and the policy bridge.** Shell states intent, compositor enforces state.
  Correct boundary, and the reason a shell crash cannot corrupt the scene graph.
- **`lx::result<T>` with union storage** ([lx.foundation/result.cppm](../../modules/lx.foundation/result.cppm)) —
  no exceptions, no default-construction requirement, `[[nodiscard]]`. Sound.
- **`not_implemented("fully::qualified::name")` discipline.** 58 honest stubs beat 58
  silent no-ops; this is why this evaluation could be written at all.
- **Protocol manifest scope.** `color-management-v1`, `wp-fifo-v1`, `wp-commit-timing-v1`,
  `wp-security-context-v1`, `ext-workspace-v1`, `xdg-session-management-v1` — this is a
  genuinely forward-looking list, ahead of most existing compositors. The problem is the
  renderer beneath it, not the ambition.
- **Wrapping libwayland-server rather than rewriting the wire** (docs/architecture.md §8).
  Correct call, correctly justified.
- **dmabuf import reading back layout from the driver rather than assuming**
  ([dmabuf_import.cppm:100](../../modules/lx.gfx/dmabuf_import.cppm#L100)). The subtle
  thing done right.

---

## 3. Decisions

### D1 — Widen the render contract before writing more protocol code

**Decision:** replace `blit_command` with a GPU-side draw record that carries the full
`draw_command` payload (affine transform, source rect, clip rect, tint, color space,
scale), and move the composite shader to sample with a transform and a source rect.

**Why now:** every dropped field is a protocol in the manifest. Writing `viewporter` or
`wp-fractional-scale-v1` protocol handling against the current renderer produces code that
provably cannot affect the screen. This is the highest-leverage change in the tree.

| Option | Complexity | Consequence |
|---|---|---|
| **A. Widen `blit_command`, keep per-quad draws** (recommended) | Low | Unblocks 5 protocols this week; per-quad cost stays |
| B. Widen + instanced/bindless rewrite together | High | Right endpoint, but couples an unblocking fix to a renderer rewrite |
| C. Leave as-is, add fields later | — | Protocol work accumulates that cannot be tested end-to-end |

Take A now, B at D4.

### D2 — Composite in linear space at ≥10 bits

> **Status: linear space done, ≥10 bits not.** Blending is now correct in all three
> backends. Wider storage is not, and is the remaining half — see "Needs revisiting".

**Decision:** render target moves to `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (or
`R16G16B16A16_SFLOAT` for the HDR path), the shader linearizes on sample per the surface's
`transfer_function`, blends linear, and encodes on output.

**Why:** 8-bit non-linear blending is incorrect today (visible fringing) and is the one
decision that cannot be retrofitted cheaply later — every surface's color state, the
scanout format negotiation, and the KMS `COLOR_ENCODING`/`HDR_OUTPUT_METADATA` properties
all hang off it. `lx.foundation` already has the vocabulary. Doing this before the surface
pipeline solidifies is the difference between Lumen being HDR-native and Lumen bolting HDR
on in v2.

**Consequence:** `color-management-v1` becomes implementable; scanout format selection gets
more complex; the golden-image tests need regeneration.

### D3 — Invert frame scheduling to deadline-driven repaint

**Decision:** `lx.scheduler.presentation` becomes the clock. The loop learns the next
presentation deadline from page-flip/`wp_presentation` feedback, estimates repaint
duration from `budget_tracker`, and starts `tick_ui` at `deadline − estimate − margin`
rather than at interval start.

**Consequence:** unblocks `wp-fifo-v1`, `wp-commit-timing-v1`, VRR, and tearing-control;
cuts roughly a frame of input latency. Requires the render path to stop blocking on
fences (see D4) for the estimate to mean anything.

### D4 — Modernize the Vulkan baseline

**Decision:** target Vulkan 1.3 core — dynamic rendering (drop `VkRenderPass`/
`VkFramebuffer`), synchronization2, timeline semaphores; add a dedicated transfer queue;
replace the blocking `vkWaitForFences` with a frame-slot timeline and timeline-driven
buffer release; batch quads into one instanced draw with a bindless descriptor array.

**Why 1.3 and not 1.1:** every driver Lumen targets (anv, radv, NVIDIA ≥ 525, turnip)
shipped 1.3 years ago. Vulkan 1.1 with render passes buys compatibility with hardware
this project has already excluded by being Vulkan-first and dmabuf-modifier-dependent.

**Trade-off:** `VK_EXT_descriptor_buffer` is tempting and is the actual bleeding edge, but
driver coverage is thinner than descriptor indexing. Recommend descriptor indexing now,
descriptor buffer behind a capability check later.

### D5 — Make the frame loop per-output from the start

**Decision:** `tick_render` iterates outputs; scanout targets, damage, timing, and KMS
commit become per-output; the scene emits a draw list per output.

**Why now rather than later:** retrofitting multi-output into a loop whose every stage
assumes one target is the single most common rewrite in compositor projects. The cost of
carrying an output index through the pipeline today is small; the cost after the pipeline
has 20 stages is a rewrite. This also unlocks per-output refresh rates, which D3 needs.

### D6 — Fix F1–F4 before anything above

Non-negotiable ordering. F1 in particular is live memory corruption behind a comment that
reads like a scheduling note.

---

## 4. Consequences

**Becomes easier**
- Protocol implementation stops being speculative — a protocol landed can be verified on screen.
- HDR/wide-gamut becomes a configuration of the pipeline rather than a new pipeline.
- Latency becomes a tunable (D3) instead of a property of the loop's shape.

**Becomes harder**
- D2 and D4 invalidate the golden-image tests; they need regeneration and a tolerance model.
- D5 makes every frame-path signature carry an output index. Do it while there are 12 call sites.

**Needs revisiting**
- The 12-test suite for a 17.5 kLOC security boundary. The Wayland server parses untrusted
  client input; there is no fuzzing target for it. That gap grows with every protocol added.
- `spsc_queue` uses `% Capacity` ([modules/lx.sync/queue.cppm:22](../../modules/lx.sync/queue.cppm#L22))
  — a division on the hot path. Constrain `Capacity` to a power of two and mask.
- `executor::post` targets an SPSC queue, so only one producer per strand is legal. The
  contract is now documented rather than mis-stated, but a compositor with UI, render, and
  worker threads all posting to the UI strand needs an MPSC queue. Next threading change.
- ~~The sanitizer suites are not usable as gates~~ — **fixed.** `test_dmabuf_composite`
  failed under both ASan and TSan on a pristine checkout, entirely inside Mesa lavapipe.
  TSan now uses `tests/sanitizers/tsan.supp` (`race:libvulkan_lvp.so`, scoped to the
  object, never to a symbol — Lumen's `std::mutex` lowers to the same `pthread_*` calls).
  LSan cannot be suppressed at all: the ICD is `dlclose`d before the exit check, so its
  frames are `<unknown module>` and no `leak:` pattern matches; the two Vulkan tests run
  with `detect_leaks=0` instead, keeping every other ASan check. All four configurations
  are green. Rationale and the checks that keep it honest are in `TEST.md`.
- `immutable_frame_snapshot::dropped()` is public and always returns false — nothing sets
  it. Either wire it or remove it.
- **D2's second half: ≥10-bit storage and HDR output.** Linear-light blending landed; wider
  storage did not. The blocker is structural, not effort: correct blending needs the
  attachment to *hold* linear values, and the trick that makes 8-bit work — an sRGB
  attachment, where the blend unit decodes, blends and re-encodes in hardware — has no
  equivalent at 10 bits, because no 10-bit format has an sRGB variant. Wider storage
  therefore needs a linear float intermediate (`R16G16B16A16_SFLOAT`) plus an explicit
  encode pass that applies the output transfer function on the way to the scanout buffer.
  That is a second render pass, pipeline, descriptor set and set of layout transitions.
  The transfer-function math it needs is already in place (`lx::to_linear` /
  `lx::from_linear`, including PQ and HLG), and the composite shader already decodes per
  draw — so the encode pass is the missing piece, not the color model.
- **The GL backend blends against the framebuffer in encoded space.** It decodes, shades
  and re-encodes each draw's own color correctly, but GLES 2 offers neither an sRGB
  framebuffer nor a float attachment, so the blend against what is already there stays
  encoded. Vulkan is linear end to end. Worth revisiting if GL becomes a primary path
  rather than a fallback.
- `docs/architecture.md` §13 marks the status table honestly but the rest of the document
  reads as descriptive. Anything the render contract cannot express should be marked as
  such in the doc, not only in the status table.

---

## 5. Action items

**Immediate (correctness) — landed**
1. [x] F1 — `memory_arena` now `mmap`s its backing at construction and reports only mapped
   bytes; a failed mapping yields a zero-capacity arena that refuses every allocation
   rather than one that bounds-checks against memory it does not own. Allocation also
   rejects non-power-of-two alignment and cannot overflow its own arithmetic.
   `memory_budget_coordinator` seeds pool capacity from the arenas, not the config.
2. [x] F2 — `write_draw_list()`/`try_publish()` replaced by an explicit `write_lease`.
   `begin_frame()` validates and reserves the slot before the producer writes a byte, and
   returns an invalid lease instead of a live slot when everything is busy. `publish()`
   consumes the lease, so a frame cannot be published twice.
3. [x] F3 — the three policies now behave differently: `drop_latest` refuses admission up
   front, `stall_ui` waits (bounded, so a wedged consumer degrades to a drop rather than
   hanging the producer), `triple_buffer` admits on slot availability and ignores
   `max_in_flight` — which is what makes the third slot do anything.
4. [x] F4 — `pending_` uses `fetch_add`/`fetch_sub`; `strand::drain()` saves and restores
   the caller's affinity. The `executor::post` doc comment no longer claims general
   thread-safety it cannot provide over an SPSC queue.
5. [x] P6 — `runtime::task` replaces the bare `void (*)()`: an allocation-free type-erased
   callable with a 48-byte inline payload (one cache line total), so posted work can name
   its subject. Callables that would need the heap are rejected at compile time.
   `runtime::callback` remains as an alias, so downstream signatures kept their shape.

Follow-on from the above:
- `scene_graph::commit_frame` now takes the lease *before* building, so a backpressured
  frame skips the scene walk and the sort entirely instead of doing the work and
  discarding it.
- New suites: `tests/test_runtime_memory.cpp` (arena bounds, alignment, move, exhaustion)
  and `tests/test_runtime_task.cpp` (context payload, affinity restore, a 20k-post
  producer/consumer race on `pending_`). `tests/test_snapshot.cpp` gained coverage for
  slot starvation and for the policies actually differing.

**Near term (unblocks the roadmap)**
6. [ ] D1 — widen the renderer contract to the full `draw_command`
7. [ ] D6/P2 — size sort scratch by `count_`; stop allocating 4096-element frames
8. [ ] D2 — linear-space, ≥10-bit composite
9. [ ] Add a fuzz target for the Wayland request-dispatch path

**Structural**
10. [ ] D3 — deadline-driven repaint scheduling
11. [ ] D4 — Vulkan 1.3 baseline, timeline semaphores, non-blocking present, batched draws
12. [ ] D5 — per-output frame loop

---

## 6. Summary

The skeleton is better than the muscle. Layering, process boundaries, error model, and
protocol ambition are all sound and several are better than what comparable projects have.
The frame pipeline is where the design promises exceed what the code can express — and
because it sits under everything, its narrow contract silently defines the ceiling for the
whole roadmap.

Nothing here requires abandoning a decision already made. D1–D5 are all widenings of seams
that already exist in the right places.
