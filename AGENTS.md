# AGENTS.md — Instructions for AI Coding Agents

This file defines how AI agents must **think**, **decide**, and **write code** in the
Lumen repository. Treat it as binding alongside the docs in `docs/`.

**Read first:** [docs/architecture.md](docs/architecture.md)

---

## 1. Project identity (non-negotiable)

Lumen is a **greenfield Linux desktop stack** — not a port of EFL/Qt/GTK.

| Decision | Choice | Do NOT |
|----------|--------|--------|
| Language | **C++26**, **Clang** | Use C++20/23 downgrade without reason; MSVC-first patterns |
| Display | **Wayland-only** | Add X11, XWayland-first paths, or Win32 display code in core modules |
| Rendering | **Vulkan-first, multi-backend** — see §1.1 | Add a backend outside `lx.gfx`; make GL or CPU the default on hardware with a working Vulkan driver; Skia-without-Vulkan as compositor backend |
| Processes | **Multi-process** | Merge compositor + shell into one binary |
| WM model | **Stacking-first**, optional tiling **plugin** | Make tiling the default or hard-code only tiling |
| Threading | **UI / render / worker** affinities + immutable snapshots | Share mutable scene graph across threads |
| Buffers | **dmabuf → GPU import**, explicit sync | SHM-only compositor path in production code |
| Security | **Unix session UID** default; Flatpak optional | Expose `zlm_shell_v1` to all clients |

**API contract version:** `lx::version` = **0.3.0** (see `lx.foundation`).

### 1.1 Rendering backends

"Vulkan-first" is about **defaults and design centre**, not exclusivity. Vulkan is the
backend the architecture is shaped around and the one to reach for on hardware that has a
Vulkan driver. It is not the only one, because a compositor that only runs on Vulkan
hardware does not run on a large amount of real hardware.

Three composite backends exist, all in `lx.gfx`, all peers behind
`compositor::config::present`:

| Backend | Module | Use |
|---------|--------|-----|
| `vulkan` | `lx.gfx:vk_renderer` | Default wherever a hardware Vulkan driver exists |
| `gl` | `lx.gfx:gl_renderer` | EGL + GLES on GBM. The accelerated path on drivers Mesa ships no Vulkan driver for — vmwgfx/SVGA3D, older radeon/nouveau, many SoCs |
| `cpu` | `lx.gfx:cpu_renderer` | When the only "GPU" is a software rasterizer, where a graphics API costs an upload and a driver stack to run the same instructions |

`present_backend::automatic` picks hardware Vulkan → hardware GL → CPU, and each backend
refuses itself when it would be a downgrade (the GL path declines a software `GL_RENDERER`).

**Rules for backends:**

- A backend lives entirely in `lx.gfx` as a module partition. `lx.compositor` selects one
  and calls it; no GL, EGL, GBM or Vulkan type appears above `lx.gfx`.
- Partition interfaces keep API types out — Vulkan, EGL and GBM handles cross the boundary
  as `void*`, exactly as `vk_renderer` already does.
- Every backend guards its platform code behind its `LUMEN_HAS_*` macro and returns
  `not_implemented` when built without it. The build must succeed with any subset present.
- **Do not add a fourth backend** without measurement showing the existing three lose on
  hardware that matters. Each one is a full composite path to keep correct.
- Client buffer handling stays dmabuf-first in every backend. SHM upload is the fallback
  path, not the design target.

Measurements behind this split live in
[docs/subsystems/rendering-performance.md](docs/subsystems/rendering-performance.md) §1.2.
Re-measure before changing a default; the right backend is host-dependent and the numbers
are not intuitive.

**Target platform for implementation:** Linux (KMS, libinput, logind). Windows stubs in
`unique_fd` exist for dev only — do not build platform logic around them.

---

## 2. Required reading before changes

| Task type | Read |
|-----------|------|
| Rendering / performance | [docs/subsystems/rendering-performance.md](docs/subsystems/rendering-performance.md) |
| Memory / buffer lifetimes | [docs/subsystems/memory-management.md](docs/subsystems/memory-management.md) |
| Health / diagnostics | [docs/subsystems/health.md](docs/subsystems/health.md) |
| Any architectural change | [docs/architecture.md](docs/architecture.md) |
| Compositor / frames | [docs/subsystems/threading.md](docs/subsystems/threading.md), [docs/subsystems/dmabuf-vulkan-import.md](docs/subsystems/dmabuf-vulkan-import.md) |
| Shell / WM | [docs/subsystems/shell-state-sync.md](docs/subsystems/shell-state-sync.md), `protocols/lumen/zlm_shell_v1.xml` |
| Errors / logging | [docs/subsystems/errors-and-logging.md](docs/subsystems/errors-and-logging.md) |
| Public API surface | [docs/api/README.md](docs/api/README.md) |
| Protocols / codegen | [protocols/README.md](protocols/README.md), `protocols/manifest.toml` |
| Diagrams | [docs/uml/README.md](docs/uml/README.md) |

If docs and code disagree, **fix code to match docs** unless the user explicitly
requests an architecture change — then update docs in the same change.

---

## 3. Decision-making framework

### 3.1 Where does this code belong?

Use the **layer graph** (dependencies flow downward only):

```
Foundation → Runtime → Platform → Graphics → Compositor → Desktop → Application
```

| If you are implementing… | Module |
|--------------------------|--------|
| Geometry, errors, handles | `lx.foundation` |
| Event loop, executor, affinities | `lx.runtime` |
| Wayland wire, DRM, libinput, session | `lx.wayland.*`, `lx.drm`, `lx.input`, `lx.session` |
| Vulkan / GL / CPU composite, dmabuf, text, layout | `lx.gfx`, `lx.text`, `lx.layout` |
| Scene graph, snapshots | `lx.scene` |
| Display server integration | `lx.compositor`, `lx.compositor.*` |
| WM policy, shell UI | `lx.shell.policy`, `lx.shell`, `lx.shell.bridge` |
| Widgets, themes, builders | `lx.ui`, `lx.ui.*` |
| App entry point | `lx.app` |

**Never:**
- Import `lx.ui` or `lx.shell` from `lx.compositor` platform internals unnecessarily
- Put Wayland dispatch inside `lx.gfx`
- Put Vulkan calls in `lx.wayland.server` (route through `lx.gfx` / `lx.scene`)

### 3.2 Process boundary check

| Owns | Process | Must NOT |
|------|---------|----------|
| Display socket, DRM, all protocol objects, `zlm_toplevel_v1` | `lumen-compositor` | Run shell panel logic in compositor |
| WM **intent** (raise, tile, workspace UI) | `lumen-shell` | Mutate compositor scene graph or client surfaces directly |
| App windows | user apps | Bind `zlm_shell_v1` or reference `zlm_toplevel_v1` |

**Shell decides intent → compositor executes state.** Wire format:
`zlm_policy_bridge_v1` (see shell-state-sync doc).

### 3.3 Thread ownership check

Before touching shared state, ask: **which affinity owns this?**

| Resource | Owner | Action |
|----------|-------|--------|
| `widget` tree, `scene_graph` mutation | `ui` | `commit_frame()` on UI thread only |
| `immutable_frame_snapshot`, Vulkan record | `render` | `acquire_render_snapshot()` on render only |
| Layout assist, I/O, decode | `worker` | Post results via `executor.post(ui, …)` |
| Cross-thread callback | any | `executor.post(target_affinity, fn)` |

Call `assert_affinity()` at module entry points when enforcing thread contracts.

**Never** pass raw pointers to widgets/scene nodes across threads without the
snapshot/executor handoff patterns.

### 3.4 Stub vs real implementation

| Situation | Return / behavior |
|-----------|-------------------|
| Platform API not wired yet | `lx::not_implemented("fully::qualified::name")` |
| Invalid input | `lx::make_error(error_domain::invalid_argument, 0, "…")` |
| Real subsystem failure | Domain enum + `make_error(domain, code, msg)` + `log_error` at boundary |
| Offline UI demo only | `lx::ui::window::create` may succeed with empty window |

**Never** return `{}` success from platform/integration `result<T>` stubs.

When replacing a stub, remove `not_implemented` and use proper domain error codes
(`wayland_err`, `drm_err`, `vulkan_err`, `gl_err`, etc. from `lx.foundation.error`).

---

## 4. Coding conventions

### 4.1 C++26 modules

- One module per logical unit under `modules/lx.*/`
- Partitions: `export module lx.foo:bar` in separate `.cppm` files; re-export from facade with `export import :bar`
- Implementation: `module :private;` section at bottom of `.cppm`, or `*.cpp` with `module lx.foo;`
- **Imports:** `import lx.foundation;` — prefer facade imports over deep partition imports
- **Exports:** public API in `export module` / `export namespace`; hide details in private sections
- Use **`[[nodiscard]]`** on fallible and pure query functions
- Use **typed handles** (`toplevel_id`, `surface_id`, `client_id`, `texture_id`) — not raw pointers in public API
- Use **`lx::unique_fd`** for kernel FDs; compositor owns FD lifetime for imports

### 4.2 Errors

```cpp
import lx.foundation;
import lx.trace;

// Propagate
LX_RETURN_IF_ERROR(subsystem.open());

// Assign from result
LX_TRY_ASSIGN(device, gfx::device_selector::select_best());

// Fail with domain
return lx::make_error(lx::error_domain::wayland,
                      static_cast<int>(lx::wayland_err::bind_failed),
                      "failed to bind socket");

// Log once at boundary, then return
lx::trace::logger::global().log_error(err, "wayland");
return err;
```

- Error messages: **static string literals** (no heap allocation in `error.message`)
- Do not log inside `format_error` or `result::get_error()`
- Do not use exceptions for control flow

### 4.3 Logging

- Default sink: stderr; override with `LUMEN_LOG` env (see errors-and-logging doc)
- Categories: stable, grep-friendly (`compositor`, `compositor.render`, `wayland`, `drm`, `gfx`, `shell`)
- **Frame hot path** (`tick_ui`, `tick_render`): only `LX_TRACE_SCOPE` / `trace` level — no `info`+ logs, no heap alloc, no sync I/O
- Startup / errors: `info`, `warn`, `error` as appropriate

### 4.4 CMake / build

- Add modules via `lumen_add_module_library()` in `CMakeLists.txt` with correct `DEPENDS`
- Protocol XML changes: update `protocols/manifest.toml` → regenerate via `protocol_manifest.py`
- Upstream protocols: `./scripts/fetch-protocols.sh` before full build
- Do not hand-edit `build/generated/GeneratedProtocols.cmake`
- Prefer **minimal dependency edges** — match `docs/uml/01-package-layers.mmd`

### 4.5 Wayland protocols

- Upstream: fetch to `protocols/upstream/`; codegen to `lx.wayland.protocols.*`
- Private: `protocols/lumen/zlm_shell_v1.xml` only for privileged shell
- Codegen tool: `wl-scanner-cpp` — extend scanner rather than adding ad-hoc XML parsers
- **Valid IDL only** — no length-prefixed implicit arrays (see stacking index design)
- Privileged globals: gate via `lx.session.privilege_checker` + `allow_privileged_global()`

### 4.6 Scope and diff discipline

- **Minimal diff** — fix the requested problem; do not refactor unrelated modules
- Match existing naming, partition layout, and stub style in the target module
- Do not add tests unless requested or they cover non-trivial behavior you introduced
- Do not create markdown files unless the user asks or architecture genuinely changed
- Do not commit unless explicitly requested

---

## 5. Key workflows (implement in this order)

### P0 milestone (current goal)

**One client → one dmabuf → one frame on screen.**

Suggested sequence:

1. `lx.wayland.server` — real socket bind, dispatch, P0 globals
   (wraps **libwayland-server** when `LUMEN_HAS_WAYLAND`; raw AF_UNIX otherwise —
   wire layer is not from-scratch; see architecture §8)
2. `lx.compositor.surface` — `on_commit`, dmabuf desc extraction (D0)
3. `lx.gfx.dmabuf` — basic Vulkan import (D1) / soft-import for headless CI
4. `tick_render` → record/present
5. `lx.drm` — page-flip vsync (P1; timer stub OK until then)

### Frame pipeline (must preserve)

```
vsync → tick_ui (UI)
  → wayland.dispatch
  → scene.update → build_draw_list_into(write_slot) → try_publish
  → notify render thread
tick_render (render)
  → acquire_render_snapshot (immutable)
  → scanout evaluate → assign_plane OR merged_render_pass
  → import_cache.tick → presentation.on_present_submitted
  → release snapshot
```

### Shell startup (must preserve)

```
shell bind zlm_shell_v1
  → capabilities
  → get_policy_bridge
  → snapshot_begin … snapshot_done (full state)
  → runtime deltas (toplevel_*, focus_changed, stacking_order_changed)
```

### dmabuf commit (must preserve)

```
client wl_surface.commit
  → import_cache.acquire(client, buffer, modifier)  // hit → reuse VkImage
  → surface_manager.on_commit
  → vulkan_composite (direct scanout: not implemented — no plane assignment yet)
  → draw_list.batch_for_render → snapshot_buffer.publish (index swap)
  → tick_render → acquire const ref → present(mode per output)
  → snapshot_buffer.release
  → wl_buffer.release
```

### Performance invariants (must preserve)

- **`snapshot_buffer` v2:** `try_publish()` swaps index only — never copy full draw_list to render
- **`build_draw_list_into`:** write directly into `write_draw_list()` — no interim copy (R1)
- **`acquire()`:** returns `const&` — render thread does not copy snapshot
- **Back-pressure:** respect `snapshot_config.backpressure` — drop/stall/triple-buffer
- **Direct scanout:** evaluate before Vulkan when `enable_direct_scanout`; hybrid uses `assign_overlay_stack`
- **Content hints:** `content_hint::video` / `game` get scanout priority
- **Import cache:** key = `(client_id, buffer_id, modifier)` — LRU eviction in `tick()`; per-client cap; evict on disconnect
- **Memory pressure:** `memory_budget_coordinator` → evict import cache, flush buffer lifecycle, evict surface pool
- **Buffer lifecycle:** import → attach → in_flight → `wl_buffer.release` via `buffer_lifecycle_tracker`.
  The tracker is **UI-affinity only** (it sends Wayland events); render publishes a completed
  frame index that UI feeds to `retire_completed_frames()`. A commit that cannot be used must
  still release its buffer, or the client stalls.
- **Client resources:** anything holding a `wl_resource*` across frames (frame callbacks,
  buffer records, scene nodes) needs a destructor hook — clients disconnect mid-flight.
- **Overflow caps:** use `overflow_action` — never silent unbounded growth on fixed arrays
- **KMS damage:** pass `kms_damage_region` via `kms_atomic_commit`; report in `presentation_feedback`
- **GPU pools:** use `pipeline_cache` and `lx.gfx.semaphore_pool` (`timeline_semaphore_pool`) — no per-frame create/destroy
- **Present mode:** discrete → mailbox, integrated → fifo_relaxed (unless overridden)
- **Hot path:** respect `hot_path_budget`; warn on exceed, no `info`+ logs per frame

See [docs/subsystems/rendering-performance.md](docs/subsystems/rendering-performance.md).

---

## 6. Module map (quick reference)

```
modules/
├── lx.foundation/     types, result, handles, error
├── lx.sync/           spsc_queue
├── lx.trace/          logger, spans
├── lx.runtime/        event_loop, executor, memory (arenas + pressure)
├── lx.wayland.*/      server, client, protocols (generated)
├── lx.drm/            KMS, atomic commit, kms_damage
├── lx.input/          libinput seat
├── lx.session/        logind, privilege
├── lx.gfx/            Vulkan, dmabuf, syncobj, import_cache
├── lx.scene/          scene graph, snapshot_buffer (triple-buffer + back-pressure)
├── lx.layout/         flex/grid
├── lx.text/           shaping
├── lx.ui/             widgets, style, theme.compile
├── lx.compositor/     compositor + surface, toplevel, shell_bridge, cursor, output
├── lx.shell/          shell + bridge
├── lx.shell.policy/   stacking/tiling policies
├── lx.app/            application framework
└── lx.portal/         optional Flatpak (LUMEN_BUILD_FLATPAK_PORTAL)

executables/
├── lumen-compositor/
├── lumen-shell/
└── lumen-hello/

protocols/             manifest.toml, lumen/*.xml, upstream/*
tools/                   wl-scanner-cpp, lumen-theme
docs/                    architecture, subsystems, api, uml
```

---

## 7. Anti-patterns (reject these ideas)

| Anti-pattern | Why |
|--------------|-----|
| Shell holds `xdg_toplevel` for WM ops | Use compositor-owned `zlm_toplevel_v1` only |
| Shell mutates compositor scene graph | Policy requests over `zlm_policy_bridge_v1` |
| Render thread calls `scene_graph::update` | UI-only mutation; render reads snapshot |
| Global mutable singletons for frame state | Use `snapshot_buffer` + executor strands |
| Raw `unsigned` for object IDs in public API | Use `lx::*_id` handles |
| Silent `return {}` on unimplemented platform APIs | Use `not_implemented("…")` |
| Logging every frame at `info` | Use `trace` + scopes on hot path |
| Adding X11-first compositor core or GBM-without-Vulkan primary path | Violates architecture |
| Optional **rootless XWayland** via `lx.compositor.xwayland` (`LUMEN_BUILD_XWAYLAND=OFF`) | Do not merge X11 paths into core compositor modules |
| Editing generated protocol files by hand | Regenerate via manifest + wl-scanner-cpp |
| Circular module dependencies | Respect layer graph |
| `static` widget builders shared across calls | Breaks fluent multi-widget API |
| Force-push / amend commits without user request | Git safety |

---

## 8. Thinking checklist (use before submitting work)

```
[ ] Read relevant docs/architecture.md + subsystem doc
[ ] Identified correct module and layer (no upward deps)
[ ] Respected process boundary (compositor vs shell vs app)
[ ] Respected thread affinity for mutated state
[ ] Fallible APIs return result<T> with correct domain
[ ] Stubs use not_implemented (not silent success)
[ ] Errors logged once at subsystem boundary
[ ] Frame hot path free of info+ logs and heap churn
[ ] Protocol changes reflected in manifest.toml + XML
[ ] CMake DEPENDS updated if module graph changed
[ ] Diff is minimal and matches existing conventions
[ ] Docs updated if architecture or public API contract changed
```

---

## 9. Document maintenance

When you change architecture or public API contracts, update **in the same PR/change**:

| Change | Update |
|--------|--------|
| New module / partition | `docs/api/README.md`, `CMakeLists.txt`, `docs/architecture.md` §3 |
| Threading behavior | `docs/subsystems/threading.md`, relevant UML `.mmd` |
| Error domains / stub policy | `docs/subsystems/errors-and-logging.md` |
| Protocol wire format | `protocols/lumen/*.xml`, `docs/subsystems/shell-state-sync.md` |
| dmabuf/Vulkan flow | `docs/subsystems/dmabuf-vulkan-import.md`, UML 19–20 |
| New executable or systemd unit | `README.md`, `deploy/systemd/`, UML 17 |

---

## 10. Environment & commands

```bash
# Fetch upstream Wayland protocols
./scripts/fetch-protocols.sh

# Configure & build (Linux)
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build

# Debug logging
LUMEN_LOG=debug ./build/lumen-compositor

# Compositor CLI
lumen-compositor [socket-name] [-f fps]
```

**Socket path:** `$XDG_RUNTIME_DIR/lumen-0` (default name `lumen-0`).

---

## 11. Summary principle

> **Compositor owns display state and execution. Shell owns policy intent. Apps own
> their surfaces. UI thread mutates; render thread reads immutable snapshots.
> Fail loudly with typed errors; log at boundaries; zero-copy dmabuf everywhere
> production matters.**

When uncertain, re-read [docs/architecture.md](docs/architecture.md) and choose the
option that preserves these invariants.
