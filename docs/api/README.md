> **Status:** Current (v0.3) — see code for implementation truth

# Lumen Public API Reference

C++26 module API surface for future implementation. All modules live under `modules/`.

**Version:** `lx::version` — 0.3.0 (API contract)

**Architecture overview:** [architecture.md](architecture.md)

**AI agent instructions:** [../AGENTS.md](../AGENTS.md)

## Module index

| Module | Purpose | Key types |
|--------|---------|-----------|
| [`lx.foundation`](../modules/lx.foundation/lx.foundation.cppm) | Core types, errors, handles, WM vocabulary | `point2i`, `result<T>`, `make_error`, `toplevel_id`, `toplevel_state`, `placement` |
| [`lx.foundation.error`](../modules/lx.foundation/error.cppm) | Error helpers | `format_error`, `LX_RETURN_IF_ERROR`, `*_err` codes |
| [`lx.sync`](../modules/lx.sync/lx.sync.cppm) | Thread synchronization | `spsc_queue`, `handoff` |
| [`lx.trace`](../modules/lx.trace/lx.trace.cppm) | Logging & tracing | `logger`, `log_error`, `LX_TRACE_SCOPE`, `LUMEN_LOG` |
| [`lx.runtime`](../modules/lx.runtime/lx.runtime.cppm) | Event loop & bus | `event_loop`, `event_source`, `timer_source`, `event_bus` |
| [`lx.runtime.executor`](../modules/lx.runtime/executor.cppm) | Thread routing | `executor`, `strand`, `affinity`, `assert_affinity` |
| [`lx.runtime.memory`](../modules/lx.runtime/memory.cppm) | Memory strategy | `memory_arena`, `memory_budget_coordinator`, `overflow_action` |
| [`lx.scheduler`](../modules/lx.scheduler/lx.scheduler.cppm) | Frame pacing | `frame_scheduler`, `frame_tick`, `thread_affinity` |
| [`lx.scheduler.presentation`](../modules/lx.scheduler/presentation.cppm) | Present timing | `presentation_tracker`, `presentation_feedback`, `kms_damage` |
| [`lx.scheduler.budget`](../modules/lx.scheduler/budget.cppm) | Hot-path CPU limits | `hot_path_budget`, `budget_tracker` |
| [`lx.session`](../modules/lx.session/lx.session.cppm) | logind session | `logind_session`, `credentials` |
| [`lx.session.privilege`](../modules/lx.session/privilege.cppm) | Shell access control: same UID and the peer's executable equal to the configured shell binary (`check_shell_bind`, used by `lx::wayland::server::allow_privileged_global`) | `privilege_checker`, `shell_verification`, `privilege_result` |
| [`lx.input`](../modules/lx.input/lx.input.cppm) | Input devices | `seat`, `pointer`, `keyboard`, `input_manager` |
| [`lx.drm`](../modules/lx.drm/lx.drm.cppm) | KMS/DRM | `kms_device`, `plane_manager`, `mode` |
| [`lx.drm.atomic`](../modules/lx.drm/atomic.cppm) | Atomic KMS; cursor-only and TEST_ONLY commits | `kms_atomic_commit`, `kms_damage_region`, `page_flip_event`, `atomic_commit_request::cursor` |
| [`lx.drm.cursor`](../modules/lx.drm/cursor_plane.cppm) | KMS cursor plane: type-matched plane, SIZE_HINTS / `DRM_CAP_CURSOR_*` sizing, two ARGB8888 dumb buffers, HOTSPOT_X/Y | `kms_cursor_plane`, `cursor_plane_caps` |
| [`lx.wayland.server`](../modules/lx.wayland.server/lx.wayland.server.cppm) | Compositor protocol core | `server`, `client_connection`, `resource`, `global_descriptor` (libwayland `wl_resource_set_implementation` vtables; no opcode router) |
| [`lx.wayland.client`](../modules/lx.wayland.client/lx.wayland.client.cppm) | Client connection | `display`, `surface`, `xdg_toplevel` |
| [`lx.gfx`](../modules/lx.gfx/lx.gfx.cppm) | Vulkan RHI | `device`, `swapchain`, `render_pass`, `dmabuf_importer` |
| [`lx.gfx.import_cache`](../modules/lx.gfx/import_cache.cppm) | Import reuse + LRU | `dmabuf_import_cache`, `import_cache_key`, per-client limits |
| [`lx.gfx.semaphore_pool`](../modules/lx.gfx/semaphore_pool.cppm) | Reusable timeline semaphores | `timeline_semaphore_pool`, `timeline_semaphore_handle` |
| [`lx.gfx.syncobj`](../modules/lx.gfx/syncobj_bridge.cppm) | drm-syncobj ↔ Vulkan timelines | `syncobj_bridge`, `syncobj_timeline` |
| [`lx.gfx.damage`](../modules/lx.gfx/damage.cppm) | Fixed-capacity multi-rect damage (16 rects = FB_DAMAGE_CLIPS), no allocation | `damage_region`, `k_damage_rects` |
| [`lx.layout`](../modules/lx.layout/lx.layout.cppm) | Layout engine | `flex_node`, `grid_node`, `layout_engine` |
| [`lx.text`](../modules/lx.text/lx.text.cppm) | Text shaping; single-line CPU rasterization (HarfBuzz shaping, FreeType grayscale glyphs, fixed glyph cache, cluster-boundary ellipsis) into premultiplied ARGB, for text drawn as a texture (server-side titles, the panel) | `font_stack`, `text_layout`, `shaped_line`, `shape_line`, `fit_line`, `render_line`, `raster_target` |
| [`lx.scene`](../modules/lx.scene/lx.scene.cppm) | Scene graph | `scene_graph`, `commit_frame`, `acquire_render_snapshot` |
| [`lx.scene.snapshot`](../modules/lx.scene/snapshot_buffer.cppm) | Immutable frames; damage carried forward over unconsumed snapshots | `snapshot_buffer`, `immutable_frame_snapshot::damage_rects` |
| [`lx.ui`](../modules/lx.ui/lx.ui.cppm) | Widget toolkit | `ui_node`, `element`, `reconciler`, `draw_scope` |
| [`lx.ui.element`](../modules/lx.ui/element.cppm) | Descriptors + frame arena | `element`, `child`, `build_context`, `next_element_type_id` |
| [`lx.ui.node`](../modules/lx.ui/node.cppm) | Retained nodes + draw emission | `ui_node`, `draw_scope`, `size_hint`, `describe<>` |
| [`lx.ui.reconcile`](../modules/lx.ui/reconcile.cppm) | Descriptor → node folding | `reconciler`, keyed child matching |
| [`lx.ui.decorator`](../modules/lx.ui/decorator.cppm) | Pipe composition | `operator\|`, `insets`, `align`, `grow`, `keyed` |
| [`lx.ui.reducer`](../modules/lx.ui/reducer.cppm) | State hosts | `reducer_node`, `reducer()`, `bind()` |
| [`lx.ui.invalidate`](../modules/lx.ui/invalidate.cppm) | Damage tracking | `damage_ledger`, `invalidate_paint/layout/animate` |
| [`lx.ui.widgets_decl`](../modules/lx.ui/widgets_decl.cppm) | Declarative widgets | `box()`, `text()`, `button()` |
| [`lx.ui.root`](../modules/lx.ui/root.cppm) | Retained tree host | `ui_root`, `describe_fn` |
| [`lx.ui.style`](../modules/lx.ui/style.cppm) | Compiled themes | `theme`, `style_ref`, `state` |
| [`lx.ui.theme.compile`](../modules/lx.ui/theme_compile.cppm) | Theme compiler API | `theme_compiler`, `theme_source` |
| [`lx.ui.builder`](../modules/lx.ui.builder/lx.ui.builder.cppm) | Fluent UI builders | `make_window()`, `widget_builder<T>`, `flex_builder` |
| [`lx.shell.policy`](../modules/lx.shell.policy/lx.shell.policy.cppm) | WM policies | `stacking_policy`, `tiling_policy`, `policy_registry` |
| [`lx.shell.task_model`](../modules/lx.shell/task_model.cppm) | The shell's window list (pure): snapshot + deltas into a fixed 256-entry table, generation-checked handles, creation order, UTF-8-safe title cut at 255 bytes, change bits | `task_model`, `task_handle`, `task_entry`, `task_change`, `copy_utf8` |
| [`lx.shell.bridge`](../modules/lx.shell/bridge.cppm) | Shell state sync client over libwayland-client (no Wayland type exported): listens on `zlm_shell_v1` (`capabilities`), creates `zlm_policy_bridge_v1`, applies its events to the task model, sends requests by task handle | `policy_bridge` |
| [`lx.shell.panel`](../modules/lx.shell/panel.cppm) | The top panel: a wlr-layer-shell surface (top, 32 px, exclusive zone 32) with one entry per window, focus highlighted, click → raise + activate; pure layout and layout diff (repaint and damage only what changed), `lx::text` labels, two-buffer shm | `panel`, `panel_globals`, `panel_layout`, `layout_panel`, `panel_hit`, `panel_diff` |
| [`lx.shell`](../modules/lx.shell/lx.shell.cppm) | lumen-shell: connect, bind, one-thread `ppoll` loop (display fd + signalfd), teardown; exit 1 when refused or disconnected | `shell_app`, `shell_config` |
| [`lx.compositor`](../modules/lx.compositor/lx.compositor.cppm) | Compositor; `config::ping_timeout_ms` (`--ping-timeout`) is how long a ping may go unanswered before a client is shown as not responding, and `liveness()` the tracker; `inject_key` / `inject_pointer_motion` / `inject_pointer_button` run virtual input through the same path as libinput's (headless tests; UI affinity) | `compositor`, `output_manager`, `seat_manager` |
| [`lx.compositor.buffer_lifecycle`](../modules/lx.compositor/buffer_lifecycle.cppm) | Buffer lifetimes | `buffer_lifecycle_tracker`, `buffer_lifecycle_state` |
| [`lx.compositor.surface`](../modules/lx.compositor/surface_manager.cppm) | Surface commits, content opacity, scene nodes (created in a container the caller names), input regions, O(1) id index, `wl_resource` registry | `surface_manager`, `surface_commit`, `input_region`, `id_slot_index` |
| [`lx.compositor.surface_tree`](../modules/lx.compositor/surface_tree.cppm) | Subsurface trees (Wayland-free): parentage, pending/cached/current child position and order, effective sync, mapping, one scene container per root with the tree flattened in paint order, global origins, per-node damage, tree hit-testing, bounding box | `surface_tree`, `tree_order`, `tree_hit`, `root_kind` |
| [`lx.compositor.positioner`](../modules/lx.compositor/positioner.cppm) | xdg_positioner placement, pure: the base rectangle (anchor point, offset, gravity) and constraint adjustment in the spec's precedence flip → slide → resize, per axis | `positioner_rules`, `positioner_result`, `positioner_base`, `positioner_place` |
| [`lx.compositor.popup`](../modules/lx.compositor/popup_manager.cppm) | xdg_popup records (Wayland-free; wire ops and window-manager callbacks installed): configure/ack ring, placement relative to the parent's window geometry against the constraint area, map (each popup a surface-tree root in the popup layer), reposition and reactive re-placement, the grab stack (owner events, keyboard to the topmost mapped grabbing popup), dismissal top-down | `popup_manager`, `popup_record`, `popup_ops`, `popup_host`, `popup_parent_kind` |
| [`lx.compositor.layer_shell`](../modules/lx.compositor/layer_shell.cppm) | wlr-layer-shell (Wayland-free; wire ops and window-manager callbacks installed): records and double-buffered properties, commit rules (`validate_layer_state`), configure/ack ring, arrangement against the output (`arrange_layers`, pure: two passes, exclusive zones first, zone -1 against the whole output, a smaller committed size centred) and the usable area it leaves, map/unmap/closed, `set_layer`, exclusive / on-demand keyboard queries, hit-testing per layer | `layer_shell`, `layer_record`, `layer_props`, `layer_ops`, `layer_host`, `layer_level`, `arrange_layers`, `layer_exclusive_edge`, `validate_layer_state` |
| [`lx.compositor.toplevel`](../modules/lx.compositor/toplevel_manager.cppm) | Toplevel registry, stacking order, configure state machine (single in flight, ack matching; the decoration mode and a maximize/restore position travel with the configure and apply with the commit answering it) | `toplevel_manager`, `toplevel_record`, `toplevel_configure`, `decoration_mode` |
| [`lx.compositor.decoration`](../modules/lx.compositor/decoration.cppm) | Server-side frames (Wayland-free): extents, frame rect and hit zones (title, close, resize bands with an invisible 8 px margin and corner precedence), the 64x64 atlas (baked colour swatches, close glyph, button circles), the frame scene node (the window container's underlay, 7–8 draws through the existing composite path), per-window title textures re-rasterized only when the title or its ellipsis cut changes, strip-only damage | `decoration_manager`, `frame_node`, `frame_extents`, `deco_zone`, `extents_for`, `frame_rect`, `deco_hit`, `build_deco_atlas` |
| [`lx.compositor.wm`](../modules/lx.compositor/window_manager.cppm) | Window geometry (source of truth, clamped to the window's whole surface tree), placement and constraint of the whole frame within the usable area (`output_area` minus layer-shell exclusive zones: `usable_area`), maximize/unmaximize, configure flush (decoration configure included), hit-testing (popups, overlay and top layers, each window's tree then its frame zones, bottom and background layers), pointer focus (owning window + actual surface, or a frame zone)/keyboard focus resolution (exclusive layer surface, grabbing popup, on-demand layer surface, toplevel), implicit grab, stacking (one tree container per window), interactive move/resize grabs (client, shell, keybinding and frame presses share one executor), xdg-activation requests decided by `decide_activation` (granted, or the window marked demands-attention; before map, decided at map) | `window_manager`, `wm_hit`, `hit_target`, `wm_grab`, `grab_mode`, `grab_source`, `effective_window_geometry`, `anchored_geometry`, `place_new_window`, `resize_target`, `edges_for_point` |
| [`lx.compositor.shell_bridge`](../modules/lx.compositor/shell_bridge.cppm) | Shell protocol server (zlm v2): snapshot then `focus_changed` on bridge creation, deltas, `toplevel_flags` to v2 bridges, inert handles | `shell_bridge`, snapshot/delta emitters, `toplevel_flags_of` |
| [`lx.compositor.cursor`](../modules/lx.compositor/cursor_manager.cppm) | Pointer image and position: precedence (grab override → frame hover → client shape/surface/hidden → default), UI→render mailboxes, software-cursor scene node; the staging hook for every compositor-owned texture | `cursor_manager`, `cursor_frame`, `latest_value`, `cursor_node`, `compositor_texture_ops` |
| [`lx.compositor.xcursor`](../modules/lx.compositor/xcursor.cppm) | Xcursor theme loader (`XCURSOR_THEME`/`XCURSOR_SIZE`/`XCURSOR_PATH`, `Inherits=`) and the built-in vector cursors rasterized at the requested size | `xcursor_theme`, `cursor_shape`, `parse_xcursor`, `rasterize_builtin` |
| [`lx.compositor.output`](../modules/lx.compositor/output_manager.cppm) | Outputs in stable slots (connector name, mode, refresh, physical size, scale, transform, present mode), a `generation` that moves only on a change clients can see, the virtual HEADLESS-1 without a connected connector; pure output-membership helpers (rect → slot mask, primary output by overlap, preferred buffer scale = ceiling of the largest, preferred transform) | `output_manager`, `output_state`, `present_mode`, `outputs_for_rect`, `primary_output_for_rect`, `preferred_buffer_scale`, `preferred_buffer_transform` |
| [`lx.compositor.liveness`](../modules/lx.compositor/liveness.cppm) | xdg_wm_base.ping/pong liveness with no Wayland in it (wire ops installed): when to ping (activation, presses, the configure watchdog, periodically to the focused client; one outstanding, 2 s apart), the timeout that marks a client not responding, the pong that clears it (or the ping's `xdg_wm_base` going, which cancels it); never kills | `liveness_tracker`, `liveness_ops`, `ping_reason` |
| [`lx.compositor.input_router`](../modules/lx.compositor/input_router.cppm) | Seat event delivery to the focused client's resources; the per-client input serial log (ranges of the input and focus serials each client was sent, wraparound-safe, LRU over 64 clients) that serial-quoting requests are checked against; the focus-client hook, called between a focus change's `leave`s and `enter`s | `input_router`, `input_serial_log`, `input_serial_kind`, `serial_newer` |
| [`lx.compositor.activation`](../modules/lx.compositor/activation.cppm) | xdg-activation's focus-stealing-prevention policy with no Wayland in it: whether a token may grant focus (decided at commit from the issuer's input serials and keyboard focus), what presenting it does (granted, urgent — the window is marked demands-attention for the shell —, pending until map, ignored) with the X11 user-time rule, and the token table (random 128-bit text, single use, 30 s, constant-time lookup). UI affinity | `activation_tokens`, `activation_token`, `activation_grant`, `activation_issue`, `activation_context`, `decide_grantable`, `decide_activation`, `activation_result`, `activation_reason`, `make_activation_token_text`, `k_activation_lifetime_ns` |
| [`lx.compositor.data_transfer`](../modules/lx.compositor/data_transfer.cppm) | Data-transfer pieces with no Wayland in them: a source's offered mime types stored inline (4 KiB, 64 types, never allocating) and drag-and-drop action negotiation (`wl_data_device_manager.dnd_action`) | `mime_list`, `dnd_action_bits`, `valid_dnd_mask`, `single_dnd_action`, `user_dnd_action`, `negotiate_dnd_action` |
| [`lx.compositor.dnd`](../modules/lx.compositor/dnd.cppm) | The drag-and-drop grab with no Wayland in it: target changes and motion from the window manager's pointer path, drop or cancel on release, Escape, the modifiers' user action, the icon (a `dnd_icon` surface-tree root in its own scene layer) following the pointer once per tick, the pointer shape showing the negotiated action; the wire side is installed as `dnd_ops`. UI affinity, no allocation per motion | `dnd_grab`, `dnd_ops`, `dnd_target`, `dnd_end_reason`, `dnd_end_name` |
| `lx.compositor:wire` (internal) | **Not API.** Implementation partition ([wire.cppm](../modules/lx.compositor/wire.cppm)) shared by the libwayland glue units `wire_*.cpp` (one per protocol area: surface, subsurface, buffer, xdg shell, xdg popup, decoration, layer shell, dmabuf, seat, cursor shape, zlm shell, output, selection, data device, primary selection, drag-and-drop, install): the wire object records, the tables and helpers more than one unit uses. Not exported from `lx.compositor`; nothing outside the module can import it | — |
| [`lx.portal`](../modules/lx.portal/lx.portal.cppm) | Flatpak portal (optional) | `desktop_portal`, `capability_token` |
| [`lx.app`](../modules/lx.app/lx.app.cppm) | Application framework | `lx::application`, `settings`, `clipboard` |

## Application example (target API)

```cpp
import lx.app;
import lx.ui.builder;

int main(int argc, char* argv[]) {
    lx::application app{argc, argv};

    auto win = lx::ui::make_window()
        .title("Settings")
        .size({640, 480})
        .resizable(true)
        .build();

    if (!win) return 1;

    lx::ui::make_column()
        .vertical()
        .spacing(8)
        .add<lx::ui::label>().text("Display").mount(win.value())
        .add<lx::ui::button>()
            .text("Apply")
            .on_click([] { /* ... */ })
            .mount(win.value());

    win.value().show();
    return app.run();
}
```

## Compositor example (target API)

```cpp
import lx.compositor;

int main() {
    lx::compositor::compositor comp{{.socket_name = "lumen-0"}};
    comp.start();
    return comp.run();
}
```

## Design conventions

1. **Strong IDs** — `lx::toplevel_id`, `lx::surface_id`, `lx::client_id`, `lx::texture_id`
2. **Errors** — `lx::result<T>` for operations that can fail; see [../subsystems/errors-and-logging.md](../subsystems/errors-and-logging.md)
3. **Threading (first-class)** — see [../subsystems/threading.md](../subsystems/threading.md)
   - UI: `affinity::ui` — widgets, scene mutation, `commit_frame`
   - Render: `affinity::render` — `acquire_render_snapshot`, Vulkan
   - Worker: `affinity::worker` — I/O, layout assist; post results to UI via `executor`
   - Cross-thread: `executor::post()` or thread-safe `event_loop::post()`
4. **Ownership** — `unique_fd` for kernel handles; compositor owns `zlm_toplevel_v1`
5. **Stubs** — platform/integration APIs return `not_implemented` until P0; see [../subsystems/errors-and-logging.md](../subsystems/errors-and-logging.md)

## Submodule partitions

Several modules split across `.cppm` partitions:

```
lx.foundation  → types, result, handles, error, wm_types
lx.sync        → queue
lx.runtime     → executor, memory
lx.scene       → snapshot (double-buffered immutable frames)
lx.gfx         → damage, dmabuf, syncobj, import_cache
lx.drm         → atomic (kms_damage, page_flip), cursor (kms_cursor_plane)
lx.ui          → element, node, invalidate, reconcile, decorator, reducer,
                 events, style, theme.compile
lx.compositor  → surface, surface_tree, positioner, popup, layer_shell, decoration, toplevel, wm, shell_bridge,
                 xcursor, cursor, output, input_router, data_transfer, dnd, liveness, activation;
                 internal (not exported): wire
lx.session     → privilege
lx.scheduler   → presentation, budget
lx.shell       → task_model, bridge, panel
```

Generated protocol modules: `lx.wayland.protocols.*` (from `protocols/manifest.toml`).
