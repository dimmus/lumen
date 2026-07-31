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
| [`lx.session.privilege`](../modules/lx.session/privilege.cppm) | Shell access control | `privilege_checker`, `shell_verification` |
| [`lx.input`](../modules/lx.input/lx.input.cppm) | Input devices | `seat`, `pointer`, `keyboard`, `input_manager` |
| [`lx.drm`](../modules/lx.drm/lx.drm.cppm) | KMS/DRM | `kms_device`, `plane_manager`, `mode` |
| [`lx.drm.atomic`](../modules/lx.drm/atomic.cppm) | Atomic KMS | `kms_atomic_commit`, `kms_damage_region`, `page_flip_event` |
| [`lx.wayland.server`](../modules/lx.wayland.server/lx.wayland.server.cppm) | Compositor protocol core | `server`, `client_connection`, `resource`, `global_descriptor` (libwayland `wl_resource_set_implementation` vtables; no opcode router) |
| [`lx.wayland.client`](../modules/lx.wayland.client/lx.wayland.client.cppm) | Client connection | `display`, `surface`, `xdg_toplevel` |
| [`lx.gfx`](../modules/lx.gfx/lx.gfx.cppm) | Vulkan RHI | `device`, `swapchain`, `render_pass`, `dmabuf_importer` |
| [`lx.gfx.import_cache`](../modules/lx.gfx/import_cache.cppm) | Import reuse + LRU | `dmabuf_import_cache`, `import_cache_key`, per-client limits |
| [`lx.gfx.semaphore_pool`](../modules/lx.gfx/semaphore_pool.cppm) | Reusable timeline semaphores | `timeline_semaphore_pool`, `timeline_semaphore_handle` |
| [`lx.gfx.syncobj`](../modules/lx.gfx/syncobj_bridge.cppm) | drm-syncobj ↔ Vulkan timelines | `syncobj_bridge`, `syncobj_timeline` |
| [`lx.layout`](../modules/lx.layout/lx.layout.cppm) | Layout engine | `flex_node`, `grid_node`, `layout_engine` |
| [`lx.text`](../modules/lx.text/lx.text.cppm) | Text shaping | `font_stack`, `text_layout` |
| [`lx.scene`](../modules/lx.scene/lx.scene.cppm) | Scene graph | `scene_graph`, `commit_frame`, `acquire_render_snapshot` |
| [`lx.scene.snapshot`](../modules/lx.scene/snapshot_buffer.cppm) | Immutable frames | `snapshot_buffer`, `immutable_frame_snapshot` |
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
| [`lx.shell.bridge`](../modules/lx.shell/bridge.cppm) | Shell state sync client | `policy_bridge`, `bridge_callbacks` |
| [`lx.shell`](../modules/lx.shell/lx.shell.cppm) | Desktop shell | `shell_app`, `panel`, `workspace_controller` |
| [`lx.compositor`](../modules/lx.compositor/lx.compositor.cppm) | Compositor | `compositor`, `output_manager`, `seat_manager` |
| [`lx.compositor.buffer_lifecycle`](../modules/lx.compositor/buffer_lifecycle.cppm) | Buffer lifetimes | `buffer_lifecycle_tracker`, `buffer_lifecycle_state` |
| [`lx.compositor.surface`](../modules/lx.compositor/surface_manager.cppm) | Surface commits | `surface_manager`, `surface_commit` |
| [`lx.compositor.toplevel`](../modules/lx.compositor/toplevel_manager.cppm) | Toplevel registry | `toplevel_manager`, `toplevel_record` |
| [`lx.compositor.shell_bridge`](../modules/lx.compositor/shell_bridge.cppm) | Shell protocol server | `shell_bridge`, snapshot/delta emitters |
| [`lx.compositor.cursor`](../modules/lx.compositor/cursor_manager.cppm) | Cursor rendering | `cursor_manager`, `cursor_kind` |
| [`lx.compositor.output`](../modules/lx.compositor/output_manager.cppm) | Output present | `output_manager`, `present_mode` |
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
lx.gfx         → dmabuf, syncobj, import_cache
lx.drm         → atomic (kms_damage, page_flip)
lx.ui          → element, node, invalidate, reconcile, decorator, reducer,
                 events, style, theme.compile
lx.compositor  → surface, toplevel, shell_bridge, cursor, output
lx.session     → privilege
lx.scheduler   → presentation, budget
lx.shell       → bridge
```

Generated protocol modules: `lx.wayland.protocols.*` (from `protocols/manifest.toml`).
