> **Status:** Current (v0.3) — see code for implementation truth

# Errors and Logging

Lumen separates **typed recoverable failures** (`lx::result<T>`) from **observability**
(`lx::trace`). Both are first-class at the API layer; implementations land during P0.

## Modules

| Module | Role |
|--------|------|
| `lx.foundation.result` | `error`, `error_domain`, `result<T>` |
| `lx.foundation.error` | Helpers, domain codes, `LX_RETURN_IF_ERROR`, `format_error` |
| `lx.trace` | Levels, categories, default stderr sink, spans, `log_error` |

## Error model

### Structure

```cpp
enum class error_domain { io, wayland, vulkan, drm, protocol,
                          invalid_argument, not_implemented, ... };

struct error {
    error_domain domain;
    int code;              // domain-specific (see *\_err enums)
    const char* message;   // static string literal — no heap allocation
};
```

### Domain codes

Each subsystem defines an `*_err` enum in `lx.foundation.error`:

| Enum | Used by |
|------|---------|
| `io_err` | `lx.session`, sockets, file I/O |
| `wayland_err` | `lx.wayland.server`, `lx.wayland.client` |
| `vulkan_err` | `lx.gfx` |
| `drm_err` | `lx.drm` |
| `protocol_err` | Wire protocol violations, `zlm_*` |

Example:

```cpp
return lx::make_error(lx::error_domain::wayland,
                      static_cast<int>(lx::wayland_err::bind_failed),
                      "failed to bind socket");
```

### Propagation helpers

```cpp
import lx.foundation.error;

lx::result<void> open_all() {
    LX_RETURN_IF_ERROR(lx::drm::kms_device::open());
    LX_RETURN_IF_ERROR(lx::wayland::server::bind("lumen-0"));
    return {};
}

lx::result<lx::gfx::device> pick_device() {
    lx::gfx::device dev{};
    LX_TRY_ASSIGN(dev, lx::gfx::device_selector::select_best());
    return dev;
}
```

### Stub policy

**Unimplemented platform code must not silently succeed.**

| Situation | Return |
|-----------|--------|
| Stub not yet wired | `lx::not_implemented("fully::qualified::name")` |
| Invalid caller input | `make_error(invalid_argument, 0, "...")` |
| Real failure | Domain-specific code + message |

### Intentional placeholder success (offline dev only)

These return empty objects without touching the platform — **not** `not_implemented`:

| API | Reason |
|-----|--------|
| `lx::ui::window::create` | `lumen-hello` offline UI demo |
| `lx::ui::window_builder::build` | Delegates to `window::create` |

All other `result<T>` platform/integration entry points return `not_implemented` until P0 lands.

Replace placeholders with real implementations or `not_implemented` as each module enters P0.

### Internal vs client-visible errors

| Layer | Mechanism |
|-------|-----------|
| C++ API | `lx::result<T>` |
| Wayland client | Protocol error (future: `lx.wayland.server` bridge) |
| Privilege denial | `lx::session::privilege_result` (no client protocol) |

Log internal failures before returning; send protocol errors only when the client
must be notified.

## Logging model

### Default behavior

- **Sink:** stderr via `default_stderr_sink` until `logger::set_sink()` overrides
- **Format:** `[LEVEL] category: message`
- **Min level:** `info` (Release), `debug` (Debug builds)
- **Override:** environment variable `LUMEN_LOG` — first letter selects level:
  `trace`, `debug`, `info`, `warn`, `error`, `critical`

```bash
LUMEN_LOG=debug lumen-compositor
```

### Levels and usage

| Level | When |
|-------|------|
| `trace` | Frame hot path, span enter/exit — use `LX_TRACE_SCOPE` |
| `debug` | State transitions, dispatch counts |
| `info` | Startup, config, thread spawn |
| `warn` | Recoverable anomalies (dropped frame, import fallback) |
| `error` | Failed `result`, client policy violation |
| `critical` | Invariant broken; compositor may exit |

### Categories

Use stable category strings (grep-friendly):

| Category | Source |
|----------|--------|
| `compositor` | `lumen-compositor` lifecycle |
| `compositor.render` | Render thread / Vulkan |
| `wayland` | Protocol dispatch |
| `drm` | KMS page-flip |
| `gfx` | Device/import |
| `shell` | `lumen-shell` |
| `error` | Default for `log_error()` |

### API examples

```cpp
import lx.trace;
import lx.foundation.error;

lx::trace::logger::global().log(
    lx::trace::level::info, "compositor", "vsync timer armed");

if (auto r = comp.start(); !r)
    lx::trace::logger::global().log_error(r.get_error(), "compositor");

{
    LX_TRACE_SCOPE("compositor.render", "tick_render");
    // ...
}
```

### Frame-path rule

On the vsync hot path (`tick_ui` → `commit_frame` → `tick_render`):

- **Allowed:** `LX_TRACE_SCOPE`, `trace` level (filtered out at default `info`)
- **Avoid:** `info`+ logging, heap allocation, synchronous I/O
- **Errors:** log at `warn`/`error` only for non-per-frame failures (import reject, etc.)

## Error + log integration

Recommended pattern at subsystem boundaries:

```cpp
lx::result<void> kms_device::open(const char* path) {
    // ... real open ...
    auto err = lx::make_error(lx::error_domain::drm,
                              static_cast<int>(lx::drm_err::open_failed),
                              "cannot open DRM device");
    lx::trace::logger::global().log_error(err, "drm");
    return err;
}
```

Do **not** log inside `format_error` or `result` accessors — log once at the boundary
where the failure is handled.

## Future (P1)

- Journald sink (`sd_journal_send`)
- Structured fields (key/value) on `logger`
- Wayland protocol error bridge in `lx.wayland.server`
- Trace export (Chrome trace format) from `scoped_span` timestamps
- CI: fail tests when `critical` is emitted

## Related docs

- [threading.md](threading.md) — affinity checks complement error boundaries
- [dmabuf-vulkan-import.md](dmabuf-vulkan-import.md) — domain-specific error actions
