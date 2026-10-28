> **Status:** Current (v0.3) — see code for implementation truth

# Errors and Logging

Lumen separates **typed recoverable failures** (`lx::result<T>`) from **observability**
(`lx::trace`). Both are first-class at the API layer.

## Modules

| Module | Role |
|--------|------|
| `lx.foundation:result` | `error`, `error_domain`, `result<T>` |
| `lx.foundation:error` | Helpers, domain codes, `format_error`; `LX_RETURN_IF_ERROR` / `LX_TRY_ASSIGN` in `lx.foundation/propagate.hpp` |
| `lx.trace` | Levels, `category` (the logging API), default stderr sink, spans, timeline |

## Error model

### Structure

```cpp
enum class error_domain { io, wayland, vulkan, gl, drm, protocol,
                          invalid_argument, not_implemented, ui };

struct error {
    error_domain domain;
    int code;              // domain-specific (see *\_err enums)
    const char* message;   // static string literal — no heap allocation
};
```

### Domain codes

Each subsystem defines an `*_err` enum in `lx.foundation:error`:

| Enum | Used by |
|------|---------|
| `io_err` | `lx.session`, sockets, file I/O |
| `wayland_err` | `lx.wayland.server` — `bind_failed`, `dispatch_failed`, `protocol_violation`, `client_disconnected`; `lx.wayland.client` — `connect_failed` (no compositor at the socket), `missing_global` (wl_compositor, wl_shm or xdg_wm_base absent, or the global an extension object needs), `connection_lost` (the connection died; `display::protocol_error()` names the interface, object and code, logged once), `object_failed` (libwayland could not make a proxy), and `dispatch_failed` / `protocol_violation` from the read protocol |
| `vulkan_err` | `lx.gfx` — `device_lost`, `out_of_memory`, `import_failed`; `export_failed` (a client swapchain image's dma-buf, a semaphore's opaque fd or sync_file could not be exported), `unsupported` (no hardware device where a software one is refused, no renderable and exportable modifier, a missing extension) |
| `gl_err` | `lx.gfx` GL path — `no_context` (EGL display, context or a required extension missing), `program_failed` (shader compile or program link), `target_failed` (GBM, EGLImage or FBO), `import_failed` (a client buffer that cannot be sampled), `draw_failed` (`glGetError` during a frame); `unsupported` (a software `GL_RENDERER` where hardware is required, below GLES 3.1, no feedback tranche with ARGB8888/XRGB8888 for the device), `export_failed` (a gbm_bo's planes or a frame's native fence could not be exported) |
| `drm_err` | `lx.drm` — `open_failed`, `mode_invalid`, `page_flip_failed`, `sync_failed`, `probe_failed` (a connector probe ioctl failed), `no_crtc` (no CRTC or primary plane left for an output), `test_rejected` (the kernel refused a TEST_ONLY of a configuration), `monitor_failed` (the udev hotplug monitor could not be opened), `no_atomic` (the driver refused DRM_CLIENT_CAP_ATOMIC; `no_atomic_hint` says what to do), `timeline_failed` (a DRM timeline syncobj operation failed), `no_format` (the compositor's dma-buf feedback offers no fourcc / modifier a client can allocate on its device: `lx::gfx::choose_client_format`, for every GPU tier; a window then falls back to wl_shm) |
| `protocol_err` | Wire protocol violations, `zlm_*` |
| `ui_err` | `lx.ui` — `theme_format` (a compiled theme that is truncated, has the wrong magic, a table out of bounds or a bad checksum), `theme_version` (another format version or token set: recompile it), `theme_source` (theme JSON that is not JSON or not a Zed theme family; the compiler keeps the byte offset), `icon_raster` (an icon librsvg could not render, or no librsvg) |

Each enum maps to its domain through `error_domain_of`, so `make_error` takes the code alone:

```cpp
return lx::make_error(lx::wayland_err::bind_failed, "failed to bind socket");
```

`result<T>` is a `std::expected<T, lx::error>` that also converts from an `error`, so
`transform`, `and_then` (with a callable returning `std::expected<U, lx::error>`) and
`value_or` work on it. `value()` on a failed result throws `std::bad_expected_access`.

### Propagation helpers

```cpp
import std;
import lx.foundation;

#include "lx.foundation/propagate.hpp" // macros: a named module exports none

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
| Invalid caller input | `lx::invalid_argument("...")` |
| Real failure | Domain-specific code + message |

### Intentional placeholder success (offline dev only)

These return empty objects without touching the platform — **not** `not_implemented`:

| API | Reason |
|-----|--------|
| `lx::ui::window::create` | `lumen-hello` offline UI demo |
| `lx::ui::window_builder::build` | Delegates to `window::create` |

Every other `result<T>` platform/integration entry point either does its work or returns `not_implemented`.

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
| `compositor` | `lumen-compositor` lifecycle. `warn`: `no present in 250ms on output <slot> — pacing frame callbacks by tick` (that output has a frame in flight and no flip came: its callbacks are answered by tick so clients do not wedge), `tick_ui over budget in recent frames (<n> of <window>, worst <us> us)` |
| `compositor.drm` | Outputs on the KMS device (ADR 008). `info` at start: `output <slot> pipeline crtc=<id> (index <i>) primary=<plane> connector=<id> mode=<w>x<h>@<mHz> cursor_plane=<plane, -1 none>` per output and `CRTC allocation: <n> outputs, <t> TEST_ONLY, <s> steps`. `debug`: `modeset output=<slot> frame=<f> outputs=<n>` (one line per output of a combined modeset commit), `output <slot> switched to WxH@mHz from frame <f>` (a hotplug mode change reaching render: new targets, then the modeset). A hotplugged output's pipeline line also says `tests=<TEST_ONLY calls>` and `(fallback mode)` when its preferred mode was refused. At start, `info` `card <path> (<driver>): <n> connected of <m> connectors[, boot VGA]` for the card opened; `warn` `card <path> (<driver>) not usable: <why>` for each one that was not, followed for `no_atomic` by `card <path> (<driver>): <what to do>` (for nouveau a line starting `TODO:` that names `options nouveau atomic=1`); none usable, `error` `no display device usable: running on a virtual output (HEADLESS-1), nothing on screen`. Errors (`drm_err`) are logged once at the boundary that gave up |
| `compositor.render` | Render thread / Vulkan. At `debug`, one line per tick that composited or committed: `frame=%u composite=%d rects=%u area=%llu commit=none\|content\|deferred\|failed\|paused\|cursor\|cursor-failed render_us=%u output=<slot>` (one line per output pipeline); idle ticks are silent. `commit=cursor` is a cursor-plane-only commit (pointer moved, nothing composited); `commit=paused` a commit that lost the race with a session pause (EACCES before logind's signal, or the gate closing during the composite) — expected, never logged as an error. While paused nothing is composited, so there are no lines at all; the first after a resume is the modeset, `area=` the whole output |
| `config` | The configuration file (ADR 011 Part 2), in the compositor and the shell. Once per generation, never per frame: each problem (`warn` for an error, `info` for a warning) as `<path>:<line>: <key>: <message>` (`… expected ']'; the configuration is not used`, `input.keyboard.repeat-rate: 5000 is outside 0..1000; keeping 30`, `input.touchpad.tapp: unknown key (did you mean tap?); ignored`, `bindings.Super+Z: unknown action "window.closee" (did you mean window.close?); binding ignored`; apply-time: `input.keyboard: layout "xx" … does not compile (<xkbcommon's reason>); keeping the previous keymap` (or `using the system default` when it failed at start; repeated by every later generation until the layout compiles), `cursor.theme: cursor theme "X" was not found; using Y`), then one summary: `info` `config: generation <n> from <file> (<e> errors, <w> warnings; <n> bindings)` or `warn` `config: generation <n> from <file> rejected (<e> errors); the configuration in use stays`. `info` `reload (SIGHUP): reading the configuration again`, `keymap: layout "…" variant "…" options "…" (<n> layouts, active <name>)`. lumen-shell: `idle: policy … blank-after … s …` when its idle settings change, `warn` `idle.lock-command: "…" is not found in PATH`, `background.spec: "…" is not a background …`, `info` `notice: Lumen configuration: …` (then a notification about it — `shell.notify` `notify id=<n> app="Lumen" (the shell's own)` — or, before the notification server runs, the panel's notice). Diagnostics are records (`lx::config::diagnostic`: severity, layer, line, key, a ≤192-byte message), bounded at 64 per generation (the rest counted); `lumen-ctl reload` and `lumen-ctl config check` print them as `error\|warning: <file>:<line>: <key>: <message>` |
| `compositor.input` (configuration) | `info` per pointer device as it appears and on a reload that changed its settings: `input device "<libinput name>" (touchpad\|mouse\|trackpoint): <settings applied: tap=on natural-scroll=on accel-profile=flat accel-speed=0.50 …>`; `debug` `input device "…" does not support: <settings>` |
| `compositor.bindings` | `debug`: `<action>: no shell speaks zlm v7; dropped` (a shell action from `lumen-ctl action` with no v7 shell); held actions (ADR 011 K2): `held action <action> begins (modifier 0x<bit>)`, `held action released`, `held action cancelled (session locked\|session paused\|interactive grab\|keyboard grab\|click elsewhere)`; `compositor.shell` `debug`: `held action <action> to the shell`, `held action ended by the shell` |
| `compositor.snap` | Snapping (ADR 011 Part 4). `info`, on a user action only: `snap id=<toplevel> to=left\|right\|top-left\|…\|none output=<slot> source=binding\|lumen-ctl`, `drag restore id=… from=maximized\|left\|… x= y= w= h=`, `keyboard move\|resize id=…`; `compositor.wm` `info`: `edge snap id=… zone=<edge bits, 16 maximize> output=<slot>`, `debug`: `quick switch id=… (no shell)`. `debug`: `edge zone id=… zone=<name> output=<slot>` (each zone change during a move: never per motion), `snap preview zone=… output=… x= y= w= h=`, `snap preview hidden` |
| `compositor.workspace` | Workspaces (ADR 011 Part 3). `info`, on a user action only: `workspace activate output=<name> name=<n> id=<id> source=binding|ext-workspace|activate|activation|test`, `workspace move window id=<toplevel> to=<name> id=<id> follow=0|1 source=…`, `workspace assign id=<id> output=<slot>`. `debug`: `window id=<toplevel> hidden|shown workspace=<id> minimized=0|1 sticky=0|1` (each visibility change), `minimize id=<toplevel> on=0|1 source=client|shell|binding|lumen-ctl`, `sticky id=<toplevel> on=0|1`, `ext-workspace bind client=<cid> version=<v> workspaces=<n>`, `ext-workspace: commit while locked; ignored`, `ext-workspace: too many requests before commit; dropped` |
| `compositor.cursor` | `info` once at start: `cursor theme=%s size=%u scale=<n>/120 from_theme=%u builtin=%u` per distinct output scale (up to four), then `hardware cursor planes=<outputs with one> size=%dx%d hotspot=%d` or `software cursor: <reason>`; `info` `software cursor: the cursor plane rejected the image` when the plane fails at runtime (preceded by one `warn` `cursor plane rejected: …`). `debug`: `cursor shape=%u source=override\|hover\|client-shape\|client-surface\|hidden\|default hardware=0\|1` whenever the resolved image changes — never per motion event |
| `compositor.surface` (content updates, ADR 009) | `debug`: `content update queued surface=<id> cause=<commit\|desync-commit\|…> depth=<n> waits=<fences> fifo=<0\|1> barrier=<0\|1> target=<ns> sealed=<children>` (an update held back), `surface solid …`, `surface alpha surface=<id> multiplier=<f>`, `surface content type surface=<id> type=<none\|photo\|video\|game>`, `surface presentation hint surface=<id> hint=<vsync\|async>`, `dma-buf EXPORT_SYNC_FILE unavailable …` (once). `warn` (once): `content-update queue full …` (the newest updates of a surface merged), `readiness poller unavailable …`. A DRM timeline operation that fails is a `drm_err::timeline_failed` error, logged at most every 600th time; a client's `import_timeline` of a non-syncobj fd is its protocol error (`invalid_timeline`), not a log line. `compositor.gfx` `info` at start: `explicit sync (linux-drm-syncobj-v1): offered\|not offered (…); async page flips (tearing-control-v1): available\|unsupported by the card`; `debug` once: `tearing hint async on a fullscreen window: async page flip unsupported by the card, presenting with vsync` |
| `compositor.hotplug` | Display hotplug (ADR 008 Part B). `info`: `hotplug: udev monitor on drm <maj>:<min>` at start (or a `monitor_failed` error and `no hotplug monitor — connectors are probed again only when the session resumes`), `output <slot> connecting name=… preferred=WxH@mHz modes=<n>`, `output <slot> add cancelled (…)`. `warn`: `output NAME not enabled: no CRTC, or none of its <n> modes passed TEST_ONLY…`, `output <slot> NAME: no mode of its new list passed TEST_ONLY — staying at WxH`, `VT resume: the connector probe took over 250 ms…`. `error`: `DRM card removed…`. `debug`: `hotplug: uevent connectors=all\|hinted`, `hotplug: probe connectors=<n> hint=<id> took=<us> us`, `hotplug: no change` (an identical scan: nothing else happens), `output <slot> pipeline retired`. Nothing is logged without a uevent |
| `compositor.output` | Outputs and output membership. `info`: `output 0 mode=%dx%d refresh_mhz=%u scale=%g (<n>/120) logical=%dx%d` once at start (the prefix is byte-stable: the smokes read it), and `output <slot> name=<connector> mode=%dx%d refresh_mhz=%u scale=… (<n>/120) logical=<w>x<h>+<x>+<y>` per output at start and whenever what clients see of the outputs changes. `debug`: `output enter\|leave surface=… output=<slot> client=… resources=<wl_outputs sent to>` (a surface's membership changed — map, move across an edge, unmap), `output preferred surface=… scale=… transform=…` (wl_surface v6, on change only), `output bind client=… output=<slot> version=… name=… replayed=<enters re-sent>`, `xdg-output create client=… output=<slot> version=… x= y= w= h= name=…`. `debug` `output global add\|remove output=<slot> name=…` (a `wl_output` global per output; a removed one is destroyed 5 s later). Hotplug, `info`: `output <slot> added name=… identifier="…" mode=WxH@mHz scale=… logical=WxH+X+Y windows_returned=<n>`, `output <slot> removed name=… identifier="…" windows_moved=<n>`, `output <slot> mode WxH@mHz (was WxH) — hotplug\|resume\|link-retrain`, `output <slot> HEADLESS-1 stands in (no display connected)`, `output <slot> disabled name=… identifier="…" windows_moved=<n> (disabled by an output configuration\|the setup)`; `output <slot> mode WxH@mHz (was WxH) — output configuration`. Never per motion or frame: a moving window logs only when it crosses onto or off an output |
| `compositor.outputcfg` | Output configuration (ADR 008 Part C). `info`: `output configuration <path>: <n> monitors, <n> setups` (read at start), `output configuration at start: <n> monitors, setup restored\|new, <n> disabled`, `connected monitors changed: their saved setup applied`, and one line per apply or test: `output configuration apply\|test succeeded\|failed\|cancelled: <every head: NAME WxH@mHz [(custom)] auto\|at X,Y xSCALE tTRANSFORM [vrr], or NAME off> (<why: committed, no modeset needed, nothing to change, TEST_ONLY refused, every head disabled, …>)`; `output configuration is not persisted (…)`. `warn`: a file that does not parse (`<path>:<line>: <reason> — ignored (renamed to .bad at the next change)`), `output <slot> NAME: its configured mode WxH failed TEST_ONLY — preferred mode instead` (start), `apply: no modeset report within 3 s …`. `error`: a failed save step (`… mkdir\|open\|write\|fsync\|rename failed: <errno text>`). `debug`: `heads done serial=<n> heads=<n> added=… removed=… changed=…` (one `done` to every manager), `apply\|test cancelled: serial <n>, current <n>`. A sandboxed client's first filtered global logs `wayland` `info` `client pid=<n> is sandboxed (Flatpak): session-wide globals hidden` |
| `compositor.wm` | Window management. `info`: `map id=… app_id=… x= y= w= h=`, `unmap id=… reason=null-buffer\|destroyed`. `debug`: `geometry id=… x= y= w= h= cause=map\|commit serial=…`, `configure id=… serial=… w= h= states=… edges=… watchdog=0\|1`, `ack id=… serial=…`, `configure watchdog id=… serial=…`, `focus keyboard id=…`, `focus pointer id=… sx=… sy=…` / `focus pointer none` (on change only, never per motion; when the surface hit is a subsurface the id is its window's and the next line is `focus pointer-surface surface=… root=…`; a popup surface logs `focus pointer popup surface=… sx=… sy=…` instead, a layer surface `focus pointer layer surface=… sx=… sy=…`, a server-side frame `focus pointer decoration id=… zone=title\|close\|top\|…` — no client has the pointer then), `focus keyboard-surface kind=toplevel\|popup\|layer\|none surface=…` (the surface that actually receives keys, when it changes), `raise\|lower\|restack id=… index=…`, `geometry … cause=move\|cancel serial=0` (a grab moved or Escape restored the window, at most once per tick), `grab begin id=… mode=move\|resize edges=… source=client\|shell\|keybinding\|decoration button=… x= y= w= h=`, `grab end id=… mode=… reason=release\|escape\|destroyed\|unmapped x= y= w= h=`, `grab rejected id=… source=… reason=serial\|unmapped\|no-button\|state\|edges\|grab-active\|dnd-active`, `usable area x= y= w= h= output=<slot>` (that output minus its layer-shell exclusive zones, on change), `fullscreen id=… on=0\|1 output=<slot\|-1> source=client\|shell`, `maximize id=… on=0\|1 source=client\|shell\|decoration` (a request; the configure that answers it shows `states=…maximized`, the commit `geometry … cause=commit`), `geometry … cause=usable serial=0` (a maximized window moved to a new usable area without a size change). Never per motion event. Formats are stable — smoke tests grep them |
| `compositor.scale` | wp_fractional_scale_v1, `debug`: `fractional preferred surface=… scale120=<n>` when a surface's preferred scale is sent (at object creation, then on change only) |
| `compositor.idle` | ext-idle-notify and idle inhibitors. `debug`: `idle notification create id=… timeout_ms=… obey_inhibitors=0\|1 count=… inhibited=0\|1`, `idle notification idled id=…`, `idle notification resumed id=…` (one per event — a zero-timeout notification sends a pair per input event), `idle inhibitor create id=… surface=… count=…`, `idle inhibitor destroy id=… count=…`, `idle inhibited=1\|0` on each edge of inhibition (evaluated after a publish). `warn`, once per run: `idle notification refused: …` / `idle inhibitor refused: …` when a client's share (16) or the table (64) is full (the client gets no_memory) |
| `compositor.power` | Display power (zwlr_output_power_v1). `info`: `display power off (CRTC ACTIVE=0, cursor plane off)` (or `(session away: nothing to commit)`), `display woken by input`, `display power on`. `warn`: `display power off refused by the driver — showing black instead (no power saving)` (soft blank; the kernel error is logged once before it). `error`: `display power on: the modeset was refused …` (stays dark until the next input). `debug`: `set_mode output=<slot> mode=on\|off client_object=<id>` |
| `compositor.lock` | ext-session-lock-v1. `info`: `session lock: <from> -> <to>` on every transition (`unlocked`, `awaiting_surfaces`, `locking`, `locked`), `lock surface create surface=… output=… WxH`, `lock surface map surface=… output=…`, `session lock: finished sent`, `logind Lock: shell asked to lock` (or `: no shell to start a locker — ignored`, `: already locked`), `logind Unlock: unlocking the session`, `system going to sleep: holding it until the session is locked (at most 2 s)`, `sleep may proceed (<why>)`. `warn`: `session lock: <from> -> abandoned` and `session lock abandoned (the locker exited) — staying locked until a new locker takes over or logind unlocks`. `debug`: `lock surface configure surface=… output=… WxH serial=…`. Protocol errors are the client's (posted, not logged) |
| `compositor.ime` | text-input-v3, input-method-v2, virtual-keyboard-v1. `info`: `input method bound`, `input method unavailable: the seat has one already`, `input method gone`. `warn` (once a run, then `debug`): `virtual keyboard keymap refused: it does not map or compile`; `warn`: `input popup map failed: layer full`. `debug`: `text input enter ti=… surface=…`, `text input leave ti=…`, `input method activate` / `deactivate`, `input method grab keyboard routing=0\|1`, `input method grab replaced`, `input method commit serial=… stale (dones=…): dropped`, `ime popup show surface=… popup=… x=… y=… WxH`, `ime popup hide surface=…`, `ime popup text_input_rectangle x,y WxH`, `text input surrounding text of N bytes cut to the protocol's 4000`, `virtual keyboard keymap N bytes differs from the seat's`. Protocol errors are the client's (posted, not logged) |
| `compositor.presentation` | wp_presentation. `debug`, once per content flip that resolved feedback, never otherwise: `flip frame=<tagged frame> t=<s.ns> seq=<MSC> refresh_ns=<n> flags=0x<kind> presented=<n> discarded=<n> output=<slot>` (`discarded` counts every discard since the previous line — superseded commits, unmaps, the pause edge). `warn`, at most once a minute: `presentation feedback table full — new feedback is discarded at once` (1024 objects in flight, or 16 on one content update) |
| `compositor.surface` | Surfaces and subsurfaces. `error` (rate-limited) when a committed buffer cannot be used. `debug`: `surface geometry surface=… buffer=<w>x<h> scale=<n> transform=<t> src=<x>,<y>,<w>x<h>\|-… dst=<w>x<h>\|-… size=<w>x<h>` when a surface's logical size or buffer size changes (buffer equal to round(size·output scale) means a 1:1 draw), `client bug: buffer … is not divisible by scale … (tolerated for this role)` once per cursor or role-less surface; `subsurface create surface=… parent=… root=…`, `subsurface commit surface=… mode=sync\|desync cached=0\|1`, `subsurface apply surface=… parent=… cause=parent-commit\|desync-commit\|set-desync\|unlink\|parent-destroyed buffer=0\|1 w= h=`, `subsurface sync surface=… mode=… effective=…`, `subsurface map surface=… root=… x= y= w= h=`, `subsurface unmap surface=… reason=null-buffer\|unlinked\|parent-unmapped\|parent-destroyed\|destroyed`, `subsurface position surface=… x= y=`, `subsurface restack parent=… order=<sid>,self,…`. Per commit only at `debug`; never per frame |
| `compositor.popup` | xdg_popup, `debug`: `popup create surface=… parent=… parent_kind=toplevel\|popup\|layer\|none`, `popup position surface=… parent=… x= y= w= h= anchor=… gravity=… adjust=… flipped=-\|x\|y\|xy slid=… resized=… constraint=<x>,<y>,<w>x<h> serial=… reason=initial\|reposition\|reactive token=…` (parent-relative), `popup ack surface=… serial=…`, `popup map surface=… x= y= w= h=` (global window geometry), `popup map-deferred surface=… reason=parent-unmapped`, `popup grab surface=… result=ok\|denied reason=-\|serial\|wm-grab\|parent-dismissed\|stack-full` (`wm-grab`: a move/resize or a drag owns the pointer), `popup dismiss surface=… reason=outside-click\|parent-unmapped\|parent-destroyed\|grab-denied\|wm-grab\|focus-change\|replaced`, `popup unmap surface=… reason=dismissed\|null-buffer\|destroyed`, `popup destroy surface=…`. Per request or commit, never per frame; `warn` once if the popup layer is full |
| `compositor.layer` | wlr-layer-shell. `info`: `layer map surface=… layer=background\|bottom\|top\|overlay namespace=… x= y= w= h=` (global surface rect), `layer unmap surface=… reason=null-buffer\|destroyed\|closed`. `debug`: `layer create surface=… layer=… namespace=… output=<slot>`, `layer configure surface=… serial=… w= h=`, `layer ack surface=… serial=…`, `layer arrange output=<slot> usable=<x>,<y>,<w>x<h> surfaces=…` (once per arrangement: a layer commit that changed something, map, unmap, destroy, an output mode change), `layer exclusive surface=… edge=top\|bottom\|left\|right\|none zone=…` (on change), and at `info` the zone hold (ADR 011 S6): `zone hold on output <slot>: top= bottom= left= right= (the shell went without a logout)`, `zone hold released on output <slot>[: a new zone mapped]`, `layer closed surface=… reason=negative-size`, `layer focus surface=… interactivity=on_demand\|exclusive cause=click`, `layer focus surface=… interactivity=on_demand cause=map` (an on_demand top/overlay surface took the keyboard as it mapped) / `layer focus surface=0 interactivity=none cause=click\|unmap previous=…` (an on-demand surface took or lost the keyboard). Never per frame or motion; `warn` once if a layer container is full |
| `compositor.deco` | Server-side decorations, `debug`: `decoration create id=<toplevel> version=1\|2`, `decoration request id=… mode=client\|server\|unset`, `decoration mode id=… requested=… sent=client\|server serial=…` (a decoration configure went out with that xdg configure), `decoration apply id=… mode=client\|server frame=<x>,<y>,<w>x<h>` (the commit answering it; global frame rect), `decoration press id=… zone=… button=<evdev>`, `decoration close id=…`, `decoration active id=… previous=…` (the frame drawn active changed — keyboard focus), `decoration double-click id=… maximize=0\|1`, `decoration title id=… width=<px> ellipsized=0\|1 title="…"` (once per rasterization: a title change, a resize that moved the ellipsis cut, or the client turning not responding — the title then ends in ` (Not Responding)` — or responsive again), `decoration destroy id=…` (the decoration object). Per request, press or title change, never per frame or motion; `warn` once when lx.text cannot shape (no title text drawn) or the decoration table is full |
| `compositor.shell` | The shell bridge (server side). `info`: `bridge bound client=<cid> snapshot=<n> focus=<toplevel> pid=<pid> binds=<n since start>` (a shell created its policy bridge; the snapshot and the `focus_changed` after it went out), `bridge destroyed`; `warn` `bridge replaced: …` when a second instance of the shell binary binds one. `debug`: `bridge request=activate\|raise\|lower\|stacking\|move\|resize\|maximize\|unmaximize\|close\|fullscreen\|minimize id=<toplevel> result=ok\|inert\|unmapped\|ignored`, one per request; `grab_key keysym=0x… mods=0x… flags=0x… result=none\|reserved\|taken\|invalid` (zlm v4), `grab activated serial=…` (a grabbed key or a tap reached the shell) |
| `compositor.selection` | Clipboard (`wl_data_*`) and primary selection, `debug`: `selection set kind=clipboard\|primary client=… source=<wl id> mimes=… serial=…`, `selection unset kind=… client=… serial=…`, `selection reject kind=… client=… serial=… reason=serial\|stale\|used`, `selection offer kind=… client=… offer=<wl id, 0 for none> mimes=… cause=focus\|set\|device\|clear\|full`, `selection inert kind=… client=… offers=… cause=focus-lost`, `selection clear kind=… cause=source-destroyed`, `selection receive kind=… client=… source_client=… mime=… result=sent\|ignored[ reason=inert\|finished\|mime\|source-gone]`, `selection offer-late …` / `selection offer-overflow …` (a source offering after use, or past its 64 types / 4 KiB). `warn` once per client: `selection offer table full client=… — sending no selection`. On state change only, never per frame or motion |
| `compositor.selection` (data control) | `debug`: `selection set kind=… client=… source=… mimes=… by=data-control`, `data-control offer proto=ext\|wlr kind=clipboard\|primary client=… mimes=… cause=device\|change`. Errors are the client's (`used_source`, `invalid_offer`), not log lines |
| `compositor.wm` (dialogs) | `debug`: `parent id=… parent=… modal=0\|1` (set_parent, xdg-foreign), `modal id=… on=0\|1 effective=0\|1`, `focus redirect id=… modal=…` (a blocked window asked for the keyboard), `modal press id=… dialog=…` (a press on a blocked parent, consumed) |
| `compositor.icon` / `compositor.bell` / `compositor.foreign` / `compositor.inhibit` | Window and session protocols (ADR 009 Part B). `debug`: `icon staged\|applied toplevel=… name=… buffers=…`; `bell client=… toplevel=… result=sent\|locked\|rate-limited` (at `info` once per client that hits the 4/s limit when not debugging); `export toplevel=… handle=<8 hex>… registered=0\|1`, `import handle=<8 hex>… result=ok\|destroyed`, `set_parent_of child=… parent=…`; `shortcuts inhibitor client=… surface=… active\|irrelevant`. `info`: `shortcuts inhibitor … created`, `… turned off\|on by the user (Super+Escape)` |
| `compositor.constraint` | pointer-constraints (ADR 009 Part 3). `info`: `pointer lock\|confine client=… surface=… persistent\|oneshot created\|created inert (limit)\|active\|inactive (session locked\|session paused\|grab\|pointer focus left\|keyboard focus left\|surface destroyed)\|released by the user (Super+Escape)\|destroyed` — once per change, never per motion. `debug`: `… warped inside the new region` |
| `compositor.capture` | Screen capture (ADR 009 Part 4). `info`: `screen capture active` / `screen capture idle` (the privacy indicator: a session not owned by the shell delivered a frame in the last 2 s, or none has). `debug`: `session <n> client=… kind=output\|toplevel\|none paint_cursors=0\|1[ stopped: <why>]` (created; a session over the limits, or of a gone source, is stopped at once), `session <n> stopped (<why>)` (`source gone` — an output unplugged or a window gone, `window unmapped`, `no capture view` — all eight window views in use; at creation also `session limit`, `no capture backend`, `window capture needs the GL or CPU backend`), `frame ready session=… serial=… damage=[full/]<rects> frame=<scene frame>[ curtain]` per delivered frame — debug, never per composite. `warn`: `completion queue full: a capture frame is lost` (cannot happen with one frame per session). `error` once at the render boundary when the GL capture objects fail (`gl_err::no_context` without ES 3.0, a dma-buf the driver refuses as a render target, a read-back that fails): the frame fails `unknown` and the session lives on. Client mistakes are protocol errors (`duplicate_frame`, `no_buffer`, `invalid_buffer_damage`, `already_captured`, `invalid_option`, `duplicate_session`; screencopy `already_used`, `invalid_buffer`), not logs |
| `compositor.control` | The control socket's stream, `info`: `subscriber dropped: not reading` (64 events sent while its socket stayed full after a dispatch; a burst queued within one UI iteration is not counted). Method errors are replies (`io.lumen.Compositor.UnknownAction`, `InvalidArgument`, `ShellNotConnected`, `SessionLocked`, `TooManySubscribers`; `org.varlink.service.InvalidParameter` before a handler runs), not log lines |
| `compositor.input` (devices) | `debug`: `input device "<libinput name>": keyboard pointer touch gesture` (the capabilities it has), once per device as it appears |
| `wayland` (security context) | `info`: `security context listener: engine=… app_id=… instance=…` (commit), `client <id> connected sandboxed (security context engine=… app_id=…): session-wide globals hidden`, `security context <engine>/<app>: listener closed by the sandbox engine`. `warn`: `sandboxed client refused: client table full`. `error` (`.error(err)` at the boundary): a listener the table cannot take (`bind_failed`: the protocol has no error for it, the engine's connections are never accepted) |
| `shell.icon` / `shell.bridge` (v6) | lumen-shell. `info`: `icon theme <name> (<n> themes in the chain; png=0\|1 svg=0\|1)` once, `icon handle=… name=… buffers=…`, `bell handle=…`, `screen capture active\|ended`. `debug`: `icon <name> <px>px -> <file>\|(not found)[ (undecodable)]` once per (name, size) — results are cached |
| `compositor.dnd` | Drag-and-drop, `debug`: `dnd start client=… source=<wl id, 0 for none> origin=<surface> icon=<surface, 0 for none> button=<evdev> actions=… mimes=…`, `dnd start rejected client=… reason=serial\|no-button\|grab-active\|dnd-active`, `dnd focus surface=… client=… sx=… sy=… offer=<wl id, 0 for none>` / `dnd focus none[ surface=… client=… devices=0]` (a target change, never per motion), `dnd accept client=… mime=<type or ->` (on change), `dnd action action=… source=<mask> offer=<mask> preferred=… user=…` (on change), `dnd drop surface=… client=… action=… mime=…`, `dnd end reason=drop\|no_target\|not_accepted\|no_action\|escape\|source_destroyed\|client_gone`, `dnd finish client=… action=…`, `dnd cancelled source=… cause=<an end reason>\|offer-destroyed\|ask-dismissed`. Never per motion: motion is one `wl_data_device.motion` per device and nothing else |
| `compositor.liveness` | xdg_wm_base ping/pong. `info`: `not-responding client=… pid=… after_ms=… toplevels=…` (a ping went unanswered past the timeout; its toplevels are marked), `responsive client=… pid=… was_ms=…` (it answered, or destroyed the xdg_wm_base of the ping). `debug`: `ping client=… serial=… reason=activate\|press\|key\|watchdog\|periodic`, `pong client=… serial=… rtt_ms=…`, `ping-cancel client=… serial=…` (the xdg_wm_base it went out on was destroyed before the pong). At most one ping per client per 2 s; never per frame or motion. The compositor never kills a client: "not responding" is shown (title suffix, dim, the shell's flag) and nothing else |
| `compositor.session` | Session lifecycle, `info`: `logout requested by shell|Ctrl+Alt+Backspace: closing N windows`, then `logout: stopping (N windows still open)` when none is left or the 5 s grace ends; `session <id> vt=<n> can_switch_vt=0\|1 active=0\|1 drm=<major>:<minor> found=XDG_SESSION_ID\|process\|display\|active-seat\|none type=<logind type at start>` once at start; the session and control socket (ADR 011): `control socket <path> (io.lumen.Compositor)`, `session environment exported: WAYLAND_DISPLAY=<name> XDG_SESSION_TYPE=wayland`, `ready: Serving <name> (<n> outputs)` (only under a service manager), `placement hold ended (<why>): <n> windows mapped meanwhile, <m> moved into the usable area`, `reload (SIGHUP): …`, `session environment unset` at exit; `warn`: `the user manager refused the session environment (or did not answer in 2 s)`, `render thread stuck in a tick: watchdog ping withheld`; `VT switch to <n> requested` (Ctrl+Alt+F<n>); one line per edge, never per frame: `session paused (drm=<major>:<minor> kind=pause\|force)` or `session paused (active=false)`, and `session resumed` once the resume modeset went through. `warn`: `VT switch failed: <reason>` (logind refused SwitchTo). No logind signal is ever dropped: every `pause` must be acknowledged, or logind holds the seat's switch. `error`: `DRM device removed — no display` (PauseDevice `gone` for the card), `resume modeset rejected (the mode is no longer offered?) — …` once per switch back. `debug`: `PauseDevice <major>:<minor> type=pause\|force\|gone`, `ResumeDevice <major>:<minor>`, `Active=0\|1`, `PauseDeviceComplete <major>:<minor>` |
| `compositor.activation` | xdg-activation, `debug`: `token create client=… token=<first 8 of 32> serial=<0 when none> surface=<requester, 0 for none> app_id=<or -> grantable=0\|1 reason=ok\|no_serial\|invalid_serial\|not_focused` (at xdg_activation_token_v1.commit), `activate token=<first 8> surface=… id=<toplevel, 0 for none> result=granted\|urgent\|pending\|ignored reason=ok\|already_focused\|no_serial\|invalid_serial\|not_focused\|unknown_token\|expired\|user_moved_on\|not_toplevel\|nothing_focused\|new_window[ cause=map[ refused=<the token's own reason>]]` (one per request, and one more at map for a window that presented its token before mapping; `refused=` when the map overrode the token's verdict), `urgent id=… on=0\|1` (`demands_attention` changed). `warn`: `token create failed: no randomness available` (getrandom and /dev/urandom both failed; the client gets a token that matches nothing). Tokens are capabilities: only 8 of their 32 characters are ever logged |
| `compositor.scene` | Surface-tree scene sync, `debug`: one `tree root=… kind=toplevel\|popup\|layer\|dnd_icon id=<toplevel> cause=map\|commit\|move\|unmap\|… nodes=<sid>[*]@<x>,<y>,<w>x<h>;…` whenever a window's drawn surfaces changed — every node in paint order with its global rect, the root marked `*`. The app smoke judges frame-perfectness from these lines. `tree root=… dim=0\|1` when a tree is dimmed or undimmed (its client stopped or resumed answering pings) |
| `compositor.latency` | Input-to-present latency, `debug` only. `present kind=content\|cursor input_us=… present_us=… latency_us=… submit_us=… fence_us=…` per present whose commit changed what the pointer shows (position, image, visibility), timed from the newest libinput event it reflects (µs, CLOCK_MONOTONIC) to the KMS flip timestamp. `submit_us` is when that commit's ioctl was issued and `fence_us` when the composite's sync_file signalled (kernel fence timestamp; 0 for no fence, a cursor-only commit, or a timing that cannot belong to this flip), which splits a present into compositor, GPU and display time; `latency summary n=… p50_us=… p95_us=… max_us=…` every 600 samples and at exit. On a virtual GPU the host draws the pointer, so "present" is the guest flip, not photons |
| `compositor.gfx` | The composite backend, at start. `info`: which path runs — `Vulkan composite path active[ (no scanout)]`, `GL composite path active (EGL/GLES on GBM)`, `CPU composite path active` — with the renderer or device name, `no hardware Vulkan — Vulkan device not created`, `Vulkan device <name> (<id>): no card to match`, `Vulkan device released (unused on the GL path)`, `shm: uploaded from client memory (no staging)`, `explicit sync (linux-drm-syncobj-v1): …; async page flips (tearing-control-v1): …`. `warn`: `second scanout slot unavailable — single-buffer`, `third scanout slot unavailable — double-buffered`, `GLES <maj>.<min>: no UI primitive renderer — solid square quads drawn as blits, other primitives dropped`. `error`: a backend that fails to start, allocate targets or present (`vulkan_err`, `gl_err`), once at the boundary |
| `compositor.texture` | `info`: `texture upload\|import\|retire-applied id=<n>[ FAILED]` for the first 16 texture operations of a run, then `(trace ends here)` |
| `compositor.memory` | `warn`: `critical pressure — evicting caches` (the memory-budget coordinator reached `critical`) |
| `compositor.seat` | `error`: the seat's keymap failed to compile or to export as a memfd (`.error(err)`; clients then get no keymap) |
| `wayland` | Protocol dispatch. `info` once at start: `shell binary <canonical path>` (the only executable allowed to bind `zlm_shell_v1`); `warn` `shell binary … not found: zlm_shell_v1 binds will be denied`; `warn` per refused privileged bind: `privileged global zlm_shell_v1 denied: client pid=… exe=<path> is not the shell binary <path>` (or `… client uid=… pid=…` for another user) |
| `gfx` | Device/import. `error`: `ui_gl: shader compile failed:` / `ui_gl: <name> program failed to link:` followed by the driver's info log (once, at start-up) |
| `gfx.swapchain` | A client window's pixel path (`lx.gfx:swapchain`, ADR-010 §4.10). `info` once per window: `client tier <vulkan\|gl\|cpu>: <device> on <node> (<the device asked for\|the first render node\|no node>) — vulkan: <taken\|refusal> gl: <taken\|refusal> explicit sync: <timeline syncobjs\|none>`; `client tier forced: <tier> (settings\|LUMEN_UI_BACKEND[, software allowed])`; `explicit sync unavailable …: implicit sync and wl_buffer.release` (GL without a native fence or syncobj export, Vulkan whose timeline semaphore is not a DRM syncobj, a render node without timeline syncobjs); `gbm: no modifier allocation, gbm_bo_create with the <linear\|implicit> modifier`. `warn` (once): `LUMEN_UI_BACKEND=…: not vulkan, gl or cpu — ignored`, `native fence export failed: glFinish and a CPU-signalled acquire point`, `no sync_file for the dma-buf …: the frame's fence is waited on the CPU before commit`, `SYNCOBJ_EVENTFD failed: release points are polled per acquire`, `<n> resized images still with the compositor: destroying the oldest before its release`. Nothing per frame |
| `input` | lx.input. `error`: `xkb: <xkbcommon's message>` for a keymap error no caller captured |
| `probe` | lumen-input-probe's report (its sink prints it bare, to the terminal and its log file; lines from other categories get `[LEVEL] category:`) |
| `runtime` | `error` (debug builds): `affinity violation` (a call on the wrong thread affinity; `assert_affinity`) |
| `runtime.memory` | `error`: `arena capacity too large to map`, `arena mmap failed — arena has zero capacity` |
| `scene.draw_list` | `warn`: `draw command cap reached`, `primitive cap reached`, `batch cap reached` (the rest of that frame's draws are dropped) |
| `text` | lx.text caps. `warn`: `shape cache full: runs are shaped but not kept`, `glyph_cache: set full, glyph not cached`, `itemize: run cap reached; the rest of a paragraph is one run`, `text_layout: a storage cap cut the text` |
| `trace` | lx.trace's timeline. `info`: `timeline recording to <path> (up to <n> events)`, `timeline: <n> events written to <path> (<d> dropped: buffer full)`. `error`: `timeline: buffer mapping failed`, `timeline: cannot open the output file` |
| `ui` | lx.ui. `warn`: `UI frame arena exhausted — keeping previous tree`, `descriptor without a construct hook`, `text_input: an insertion was cut to the capacity` |
| `ui.icon` | `warn`: `librsvg-2.so.2 cannot be loaded: icons are not drawn`, `librsvg lacks an entry point Lumen needs (2.52+): …`. `error`: `icon_cache: UI strand full, raster lost` |
| `ui.text` | `warn`: `text_block: worker strand full, shaping inline`. `error`: `text_block: UI strand full, shape result lost` |
| `ui.theme` | `info`: `theme reloaded: <path>`. `warn`: `theme_watcher: worker strand full, reload skipped`. `error`: `theme_watcher: UI strand full, reload lost`, a theme that does not load (`ui_err`) |
| `shell.background` | `info`: `background <w>x<h> px, rendered in <ms> ms`. `warn`: `cannot read background image <path>: showing the default` |
| `shell` | `lumen-shell` lifecycle. `info`: `connected to <display>; panel requested`, `signal <n>: exiting`, `launcher only from the panel (no key grabs)` (a compositor below zlm_shell_v1 v4). `error` (then exit 1): `zlm_shell_v1 bind rejected (…): protocol error on <iface>#<id>, code <c>`, `missing globals: …`, `compositor gone: …`, `panel closed; …` |
| `shell.bridge` | lumen-shell's task model from `zlm_policy_bridge_v1`. `info`: `capabilities flags=0x… version=…`, `snapshot toplevels=<n>`. `debug`: `snapshot toplevel handle=<slot> app_id=… title="…"`, `add handle=<slot> count=<n>`, `remove handle=<slot> count=<n>`, `title handle=<slot> app_id=… title="…"`, `state handle=<slot> state=<n>`, `focus handle=<slot>\|none`, `flags handle=<slot> flags=<toplevel_flag bits>` (zlm v2: 1 not responding, 2 demands attention; on change), `key grab activated keysym=0x… serial=<n>` (zlm v4). `info` once: `compositor has no key grabs (v<n>): no shell keybindings` (bound below v4). `warn`: `key grab keysym=0x… refused: reserved by the compositor\|already taken\|invalid`. Geometry and stacking deltas are stored, never logged (geometry arrives once per frame during a move) |
| `shell.panel` | lumen-shell's panel. `info`: `configure serial=… w= h=` when its size changes. `debug`: `layout idx=<i> x=<x> w=<w> handle=<slot> title="…"` (each entry, whenever the arrangement changes), `draw entries=<n> hidden=<k> focused=<i\|-1> damage=<rects>[(full)] repaint=<rects> buffer=<0\|1>` (one per commit: rects damaged against the last commit, rects repainted in that buffer), `click idx=<i> handle=<slot> title="…"` (a click that sends raise + activate). `info` `workspaces [1* 2+ 3!] output=<name>` when what its workspace indicator shows changes (`*` active, `+` occupied, `!` urgent). Never per motion event: hover redraws only when the entry under the pointer changes |
| `shell.workspaces` | lumen-shell's ext-workspace client. `info`: `ext-workspace: <n> workspaces` (its first `done`), `activate workspace <name>` (a click or the wheel on the indicator) |
| `shell.switcher` | lumen-shell's window switcher (ADR 011 K1). `info`, once per switch: `switcher: activate (released\|return\|click\|switcher.next…, shown\|never shown) "<title>"`, `switcher: nothing (escape\|cancelled by the compositor\|locked\|…, shown\|never shown)`, `switcher: <action>: no window to switch to`; `switcher: open (<n> windows, <w>x<h>, <c> columns)`, `switcher: close (<n> frames)` when its view appears and goes. `debug`: `switcher: hold <action> windows=<n> selection=<i> "<title>"`, `switcher: draw selection=… first=… damage=<rects>`; `shell.bridge` `debug`: `held action <action> serial=…`, `hold key keysym=0x… mods=0x… binding=…`, `hold released`, `hold cancelled` |
| `shell.bus` | lumen-shell's user-bus connection (ADR 011 N1). `info`: `bus name <name>: owned\|queued (another owner has it; taken when it exits)\|lost`. `warn`: `no user bus (<why>): no notifications, no tray` once, `user bus connection lost: <why>`, `bus name <name>: request failed`, `RequestName <name>: <error>` |
| `shell.notify` | lumen-shell's notification server and popups (ADR 011 N2–N4). `info`: once at start `notifications and tray on the user bus as <unique> (next id <n>, do not disturb on\|off)`; per notification `notify id=<n>[ (replaced)] app="…" urgency=low\|normal\|critical timeout=<ms> actions=<n>[ image][ merged] summary="…" popup\|queued (locked)\|history (do not disturb)\|dropped`, `notify id=<n> closed reason=<1-4> (expired\|dismissed\|CloseNotification\|undefined)`, `notify id=<n> action "<key>" invoked token=yes\|no`, `notify id=<n> link opened`; `notifications: shown (<n> cards, top\|overlay layer)`, `notifications: overlay\|top layer (a critical one shown\|gone)`, `notifications: hidden (<n> frames)`, `notifications: do not disturb on\|off[ (screen captured)]`, `notifications: do not disturb set\|cleared (<why>)`. `warn`: `notify: <app>'s image-data (<w>x<h>, stride <s>, <c> channels): <why>; ignored`, `notify: <sender> sends more than 20 notifications a second; the excess replaces its newest` (once per sender), `notifications: cannot export …`. `debug`: `notifications: draw cards=<n> more=<k> <w>x<h>`, `notifications: card id=<n> box=<x,y,w,h> close=<x,y,w,h> buttons=<x,y,w,h;…>` (the smoke test clicks these), `CloseNotification <id>: no such notification`, `notify: <sender> left the bus: its actions are gone` |
| `shell.tray` | lumen-shell's StatusNotifierWatcher and host (ADR 011 N5). `info`: `tray: watcher and host (org.kde.StatusNotifierWatcher)` or `tray: another StatusNotifierWatcher runs: host only`, `tray: item registered <bus/path> (by <sender>)`, `tray: item <bus/path> id="…" title="…" status=… icon=<name\|->[ (pixmap)] menu=<path\|->` (its first properties), `tray: item <bus/path> left the bus`, `tray: item <bus/path> not reachable (<error>): dropped`, `tray: <bus/path> Activate\|SecondaryActivate x= y= token=yes\|no`, `tray: <bus/path> ContextMenu x= y=`, `tray: <bus/path> menu Event id=<n> clicked`, `tray: menu of item <key>: <error>`. `warn`: `tray: item <bus/path>: malformed properties`, `tray: menu of item <key>: malformed layout`. `debug`: `tray: <bus/path> signals <member>`, `tray: item <bus/path> updated status=… icon=…`, `tray: menu of item <key> revision <r> items <n>`, `tray: tooltip "<title>"`, `tray: call failed: <error>` |
| `shell.menu` | lumen-shell's popup menus (the tray's, the notification history, a notification's further actions). `info`: `menu <name>: open (<n> items, <w>x<h>)`, `menu <name>: activate id=<n> "<label>"`, `menu <name>: closed`. `error`: `cannot allocate menu buffers` |
| `shell.launcher` | lumen-shell's launcher. `info`: `launcher: open (<n> applications, <w>x<h>, <rows> rows)`, `launcher: close`. `debug`: `launcher: close (escape\|keyboard leave\|closed by the compositor)`, `launcher: draw matches=<n> selection=<i> first=<i> damage=<rects>` (at most once per frame callback, only when something changed) |
| `shell.idle` | lumen-shell's idle policy. `info` once at start: `blanking the outputs after <s> s without input`, or `no screen blanking: …` (why: `--blank-after 0`, a missing global), or `idle policy none: an external daemon decides …`; `info`: `idle for <s> s: powering the outputs off`. `warn`: `output power control failed (the output went?) …`. `debug`: `resumed`, `output power on\|off` |
| `shell.lock` | lumen-shell's lock policy. `info` once at start: `lock command: <cmd> (Super+L, logind Lock, before sleep[, before blanking])` or `compositor below zlm_shell_v1 v5: no session-lock integration`; `info`: `lock: starting <path> (<why>)` (`Super+L`, `logind Lock`, `before sleep`, `idle`, `the locker exited`, `relaunch after the locker exited`), `lock requested, but no --lock-command is set`. `error`: `the locker keeps exiting: not starting it again — the session stays locked (loginctl unlock-session unlocks it)`. `shell.idle` adds `locked: powering the outputs off`, `locked and idle for <s> s: …`, `idle: no lock within 5 s — powering the outputs off anyway`; `shell.bridge` logs `session lock <state>` and `lock requested (logind\|the system is going to sleep)` |
| `shell.apps` | The application catalog and launching. `info`: `apps: catalog <n> entries, <m> listed (<files> files, <dirs> dirs) in <ms> ms; terminal <source>` (each scan: after the panel's first frame, then on launcher open when stale), `apps: launched <id> as app-lumen-<id>@<random>.service` (the start job finished `done`), `apps: launched <id> pid=<pid> in app-lumen-<id>-<random>.scope` (systemd < 250) or `apps: launched <id> pid=<pid> (double fork)`, `apps: systemd --user <version>: one transient scope per launch (no ExitType=cgroup before 250)` once (at `debug` for 250 and later: `… one transient service per launch`), `apps: systemd --user refused the service properties — launching into transient scopes` once, `apps: systemd --user unavailable (<why>) — launching with double fork (not resource-tracked)` once. `warn`, once per (desktop ID, reason) per catalog generation: `apps: Could not start <Name>: <reason> (<id>)`, with `journalctl --user -u '<unit>'` for a failed start job. `debug`: `apps: catalog stale`, `apps: starting <id> (<program>) token=yes\|no`, `apps: StartTransientUnit <unit>: <error>: <message>`, `apps: scope <unit>: <result>`, `apps: <id>: scope not ready in time — started outside it for now`, per-file parse skips (`desktop file <path>: <why>`) |
| `portal` | xdg-desktop-portal-lumen (stderr; `run-desktop.sh` writes `portal-lumen.log`, and its `--choose` runs land in `xdpw.log`). `info`: `serving … (Screenshot v3, targets 0x7) on display <name>` once; per request `Screenshot app='<id>' interactive=<0|1> target=<n>` / `PickColor …`, `screenshot (<all outputs|output|area|window>) WxH written to <path> in <ms> ms`, `colour picked #RRGGBB`, `request answered <response> in <ms> ms[ uri=…]`, `the session is locked: nothing is captured`, `request closed by the caller`; the chooser: `chooser: chose '<line>'` / `chooser: declined`. `warn`: a request while another runs (answered 2), `overlay failed: …`, a broken Wayland connection. `.error(err)` at the boundary for a failed connection, capture (`wayland` domain: no capture global, `stopped`, timeout) or file (`io` domain) |
| `portal.wayland` | The portal's Wayland side. `debug`: `captured output <name> WxH transform=<t> layout=x,y WxH` per output and `captured <n> output(s) in <ms> ms` per request |
| `portal.overlay` | The portal's overlay and lists. `debug`: `overlay result <kind>` |

### API examples

Code logs through an `lx::trace::category`: one object per category string, declared once,
with a method per level. It is the only logging form outside `lx.trace` itself — no
per-file `foo_log` helpers, no `foo_debug()` level checks, no `logger::global()` calls.

```cpp
import lx.trace;
import lx.foundation;

#include "lx.trace/scope.hpp" // LX_TRACE_SCOPE: a named module exports no macros

// Once per file, in an anonymous namespace; or once per module as `inline constexpr` in a
// partition its units import (`log_surface` in lx.compositor:wire).
constexpr lx::trace::category log_wm{"compositor.wm"};

log_wm.info("vsync timer armed");
log_wm.debug("geometry id={} x={} y={} cause={}", id.id(), g.x, g.y, cause);
log_wm.log(changed ? lx::trace::level::info : lx::trace::level::debug, "focus id={}", id.id());
log_wm.warn("{}", why);                 // text known only at run time

if (auto r = comp.start(); !r)
    log_wm.error(r.get_error());        // a failed result, logged once at the boundary

if (log_wm.enabled(lx::trace::level::debug)) {
    // work that only feeds a debug line
}

{
    LX_TRACE_SCOPE("compositor.render", "tick_render");
    // ...
}
```

Name the object `log_<last part of the category>` (`log_wm`, `log_surface`, `log_tray`); the
constructor is `consteval`, so the name is always a string literal. Every message argument is
a `std::format` string checked against the arguments at compile time — a literal brace is
`{{`, and text that is not a literal goes through `"{}"`. Do not build a line with `snprintf`
into a stack buffer. A call returns before formatting when the level is filtered out, formats
into a 4 KiB stack buffer (longer lines are truncated) and never allocates. The formatting
itself is type-erased into `lx.trace` (`logger::vlog`), so a call site costs a compile-time
format check and a few hundred bytes of code rather than an instantiation of `std::format`.
Two conventions differ from plain `std::format`, to keep lines as they were under `printf`:
a `bool` argument prints as `1`/`0`, and a null `const char*` as `(null)`.

Smoke scripts grep these lines, so a change to a format string is a change to an interface.

A helper that builds a domain message (`log_geometry`, `log_grab_reject`) is fine; one that
only forwards to a category is not. `lx::trace::logger` stays as the layer under categories:
its public side is the sink and the minimum level (`set_sink`, `set_min_level`); its raw
`log`/`logf`/`log_error` are private to `category` and `scoped_span`, so a call site cannot
bypass the category. A tool that owns its output installs a sink instead of printing:
`lumen-input-probe` routes every line — its report and lx.input's own — to the terminal and
its timestamped log file that way.

### Timeline

`LUMEN_TRACE_FILE=<path>` records a Perfetto-readable timeline of a compositor run (Chrome
trace-event JSON; ui.perfetto.dev and trace_processor load it): every `LX_TRACE_SCOPE` span,
`timeline_counter` samples and `timeline_instant` events, with thread names from
`name_thread`. `compositor::start` calls `timeline_start_from_env`; events go into one
preallocated buffer (`LUMEN_TRACE_EVENTS` caps it, default 2,000,000) with a lock-free append
and no allocation, and `timeline_flush` writes the file once the threads have stopped.
Timestamps are CLOCK_MONOTONIC, the clock of `perf record -k mono` and KMS. Off, a span costs
one relaxed load. The `trace` category reports the recording (see the table).

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
    auto err = lx::make_error(lx::drm_err::open_failed, "cannot open DRM device");
    log_drm.error(err);
    return err;
}
```

Do **not** log inside `format_error` or `result` accessors — log once at the boundary
where the failure is handled.

## Future (P1)

- Journald sink (`sd_journal_send`)
- Structured fields (key/value) on `logger`
- Wayland protocol error bridge in `lx.wayland.server`
- CI: fail tests when `critical` is emitted

## Related docs

- [threading.md](threading.md) — affinity checks complement error boundaries
- [dmabuf-vulkan-import.md](dmabuf-vulkan-import.md) — domain-specific error actions
