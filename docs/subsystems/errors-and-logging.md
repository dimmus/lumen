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
| `compositor.render` | Render thread / Vulkan. At `debug`, one line per tick that composited or committed: `frame=%u composite=%d rects=%u area=%llu commit=none\|content\|deferred\|failed\|cursor\|cursor-failed render_us=%u`; idle ticks are silent. `commit=cursor` is a cursor-plane-only commit (pointer moved, nothing composited) |
| `compositor.cursor` | `info` once at start: `cursor theme=%s size=%u from_theme=%u builtin=%u`, then `hardware cursor plane=%u size=%ux%u hotspot=%d` or `software cursor: <reason>`; `info` `software cursor: the cursor plane rejected the image` when the plane fails at runtime (preceded by one `warn` `cursor plane rejected: …`). `debug`: `cursor shape=%u source=override\|hover\|client-shape\|client-surface\|hidden\|default hardware=0\|1` whenever the resolved image changes — never per motion event |
| `compositor.output` | Outputs and output membership. `info`: `output 0 mode=%dx%d refresh_mhz=%u` once at start (byte-stable: the smokes read it), and `output <slot> name=<connector> mode=%dx%d refresh_mhz=%u` per output at start and whenever what clients see of the outputs changes. `debug`: `output enter\|leave surface=… output=<slot> client=… resources=<wl_outputs sent to>` (a surface's membership changed — map, move across an edge, unmap), `output preferred surface=… scale=… transform=…` (wl_surface v6, on change only), `output bind client=… output=<slot> version=… name=… replayed=<enters re-sent>`, `xdg-output create client=… output=<slot> version=… x= y= w= h= name=…`. Never per motion or frame: a moving window logs only when it crosses onto or off an output |
| `compositor.wm` | Window management. `info`: `map id=… app_id=… x= y= w= h=`, `unmap id=… reason=null-buffer\|destroyed`. `debug`: `geometry id=… x= y= w= h= cause=map\|commit serial=…`, `configure id=… serial=… w= h= states=… edges=… watchdog=0\|1`, `ack id=… serial=…`, `configure watchdog id=… serial=…`, `focus keyboard id=…`, `focus pointer id=… sx=… sy=…` / `focus pointer none` (on change only, never per motion; when the surface hit is a subsurface the id is its window's and the next line is `focus pointer-surface surface=… root=…`; a popup surface logs `focus pointer popup surface=… sx=… sy=…` instead, a layer surface `focus pointer layer surface=… sx=… sy=…`, a server-side frame `focus pointer decoration id=… zone=title\|close\|top\|…` — no client has the pointer then), `focus keyboard-surface kind=toplevel\|popup\|layer\|none surface=…` (the surface that actually receives keys, when it changes), `raise\|lower\|restack id=… index=…`, `geometry … cause=move\|cancel serial=0` (a grab moved or Escape restored the window, at most once per tick), `grab begin id=… mode=move\|resize edges=… source=client\|shell\|keybinding\|decoration button=… x= y= w= h=`, `grab end id=… mode=… reason=release\|escape\|destroyed\|unmapped x= y= w= h=`, `grab rejected id=… source=… reason=serial\|unmapped\|no-button\|state\|edges\|grab-active\|dnd-active`, `usable area x= y= w= h=` (output minus layer-shell exclusive zones, on change), `maximize id=… on=0\|1 source=client\|shell\|decoration` (a request; the configure that answers it shows `states=…maximized`, the commit `geometry … cause=commit`), `geometry … cause=usable serial=0` (a maximized window moved to a new usable area without a size change). Never per motion event. Formats are stable — smoke tests grep them |
| `compositor.surface` | Surfaces and subsurfaces. `error` (rate-limited) when a committed buffer cannot be used. `debug`: `subsurface create surface=… parent=… root=…`, `subsurface commit surface=… mode=sync\|desync cached=0\|1`, `subsurface apply surface=… parent=… cause=parent-commit\|desync-commit\|set-desync\|unlink\|parent-destroyed buffer=0\|1 w= h=`, `subsurface sync surface=… mode=… effective=…`, `subsurface map surface=… root=… x= y= w= h=`, `subsurface unmap surface=… reason=null-buffer\|unlinked\|parent-unmapped\|parent-destroyed\|destroyed`, `subsurface position surface=… x= y=`, `subsurface restack parent=… order=<sid>,self,…`. Per commit only at `debug`; never per frame |
| `compositor.popup` | xdg_popup, `debug`: `popup create surface=… parent=… parent_kind=toplevel\|popup\|layer\|none`, `popup position surface=… parent=… x= y= w= h= anchor=… gravity=… adjust=… flipped=-\|x\|y\|xy slid=… resized=… constraint=<x>,<y>,<w>x<h> serial=… reason=initial\|reposition\|reactive token=…` (parent-relative), `popup ack surface=… serial=…`, `popup map surface=… x= y= w= h=` (global window geometry), `popup map-deferred surface=… reason=parent-unmapped`, `popup grab surface=… result=ok\|denied reason=-\|serial\|wm-grab\|parent-dismissed\|stack-full` (`wm-grab`: a move/resize or a drag owns the pointer), `popup dismiss surface=… reason=outside-click\|parent-unmapped\|parent-destroyed\|grab-denied\|wm-grab\|focus-change\|replaced`, `popup unmap surface=… reason=dismissed\|null-buffer\|destroyed`, `popup destroy surface=…`. Per request or commit, never per frame; `warn` once if the popup layer is full |
| `compositor.layer` | wlr-layer-shell. `info`: `layer map surface=… layer=background\|bottom\|top\|overlay namespace=… x= y= w= h=` (global surface rect), `layer unmap surface=… reason=null-buffer\|destroyed\|closed`. `debug`: `layer create surface=… layer=… namespace=… output=0`, `layer configure surface=… serial=… w= h=`, `layer ack surface=… serial=…`, `layer arrange output=0 usable=<x>,<y>,<w>x<h> surfaces=…` (once per arrangement: a layer commit that changed something, map, unmap, destroy, an output mode change), `layer exclusive surface=… edge=top\|bottom\|left\|right\|none zone=…` (on change), `layer closed surface=… reason=negative-size`, `layer focus surface=… interactivity=on_demand\|exclusive cause=click` / `layer focus surface=0 interactivity=none cause=click\|unmap previous=…` (an on-demand surface took or lost the keyboard). Never per frame or motion; `warn` once if a layer container is full |
| `compositor.deco` | Server-side decorations, `debug`: `decoration create id=<toplevel> version=1\|2`, `decoration request id=… mode=client\|server\|unset`, `decoration mode id=… requested=… sent=client\|server serial=…` (a decoration configure went out with that xdg configure), `decoration apply id=… mode=client\|server frame=<x>,<y>,<w>x<h>` (the commit answering it; global frame rect), `decoration press id=… zone=… button=<evdev>`, `decoration close id=…`, `decoration active id=… previous=…` (the frame drawn active changed — keyboard focus), `decoration double-click id=… maximize=0\|1`, `decoration title id=… width=<px> ellipsized=0\|1 title="…"` (once per rasterization: a title change, a resize that moved the ellipsis cut, or the client turning not responding — the title then ends in ` (Not Responding)` — or responsive again), `decoration destroy id=…` (the decoration object). Per request, press or title change, never per frame or motion; `warn` once when lx.text cannot shape (no title text drawn) or the decoration table is full |
| `compositor.shell` | The shell bridge (server side). `info`: `bridge bound client=<cid> snapshot=<n> focus=<toplevel>` (a shell created its policy bridge; the snapshot and the `focus_changed` after it went out), `bridge destroyed`; `warn` `bridge replaced: …` when a second instance of the shell binary binds one. `debug`: `bridge request=activate\|raise\|lower\|stacking\|move\|resize\|maximize\|unmaximize\|close\|fullscreen\|minimize id=<toplevel> result=ok\|inert\|unmapped\|ignored`, one per request |
| `compositor.selection` | Clipboard (`wl_data_*`) and primary selection, `debug`: `selection set kind=clipboard\|primary client=… source=<wl id> mimes=… serial=…`, `selection unset kind=… client=… serial=…`, `selection reject kind=… client=… serial=… reason=serial\|stale\|used`, `selection offer kind=… client=… offer=<wl id, 0 for none> mimes=… cause=focus\|set\|device\|clear\|full`, `selection inert kind=… client=… offers=… cause=focus-lost`, `selection clear kind=… cause=source-destroyed`, `selection receive kind=… client=… source_client=… mime=… result=sent\|ignored[ reason=inert\|finished\|mime\|source-gone]`, `selection offer-late …` / `selection offer-overflow …` (a source offering after use, or past its 64 types / 4 KiB). `warn` once per client: `selection offer table full client=… — sending no selection`. On state change only, never per frame or motion |
| `compositor.dnd` | Drag-and-drop, `debug`: `dnd start client=… source=<wl id, 0 for none> origin=<surface> icon=<surface, 0 for none> button=<evdev> actions=… mimes=…`, `dnd start rejected client=… reason=serial\|no-button\|grab-active\|dnd-active`, `dnd focus surface=… client=… sx=… sy=… offer=<wl id, 0 for none>` / `dnd focus none[ surface=… client=… devices=0]` (a target change, never per motion), `dnd accept client=… mime=<type or ->` (on change), `dnd action action=… source=<mask> offer=<mask> preferred=… user=…` (on change), `dnd drop surface=… client=… action=… mime=…`, `dnd end reason=drop\|no_target\|not_accepted\|no_action\|escape\|source_destroyed\|client_gone`, `dnd finish client=… action=…`, `dnd cancelled source=… cause=<an end reason>\|offer-destroyed\|ask-dismissed`. Never per motion: motion is one `wl_data_device.motion` per device and nothing else |
| `compositor.liveness` | xdg_wm_base ping/pong. `info`: `not-responding client=… pid=… after_ms=… toplevels=…` (a ping went unanswered past the timeout; its toplevels are marked), `responsive client=… pid=… was_ms=…` (it answered, or destroyed the xdg_wm_base of the ping). `debug`: `ping client=… serial=… reason=activate\|press\|key\|watchdog\|periodic`, `pong client=… serial=… rtt_ms=…`, `ping-cancel client=… serial=…` (the xdg_wm_base it went out on was destroyed before the pong). At most one ping per client per 2 s; never per frame or motion. The compositor never kills a client: "not responding" is shown (title suffix, dim, the shell's flag) and nothing else |
| `compositor.session` | Session lifecycle, `info`: `logout requested by shell|Ctrl+Alt+Backspace: closing N windows`, then `logout: stopping (N windows still open)` when none is left or the 5 s grace ends |
| `compositor.activation` | xdg-activation, `debug`: `token create client=… token=<first 8 of 32> serial=<0 when none> surface=<requester, 0 for none> app_id=<or -> grantable=0\|1 reason=ok\|no_serial\|invalid_serial\|not_focused` (at xdg_activation_token_v1.commit), `activate token=<first 8> surface=… id=<toplevel, 0 for none> result=granted\|urgent\|pending\|ignored reason=ok\|already_focused\|no_serial\|invalid_serial\|not_focused\|unknown_token\|expired\|user_moved_on\|not_toplevel\|nothing_focused\|new_window[ cause=map[ refused=<the token's own reason>]]` (one per request, and one more at map for a window that presented its token before mapping; `refused=` when the map overrode the token's verdict), `urgent id=… on=0\|1` (`demands_attention` changed). `warn`: `token create failed: no randomness available` (getrandom and /dev/urandom both failed; the client gets a token that matches nothing). Tokens are capabilities: only 8 of their 32 characters are ever logged |
| `compositor.scene` | Surface-tree scene sync, `debug`: one `tree root=… kind=toplevel\|popup\|layer\|dnd_icon id=<toplevel> cause=map\|commit\|move\|unmap\|… nodes=<sid>[*]@<x>,<y>,<w>x<h>;…` whenever a window's drawn surfaces changed — every node in paint order with its global rect, the root marked `*`. The app smoke judges frame-perfectness from these lines. `tree root=… dim=0\|1` when a tree is dimmed or undimmed (its client stopped or resumed answering pings) |
| `compositor.latency` | Input-to-present latency, `debug` only. `present kind=content\|cursor input_us=… present_us=… latency_us=… submit_us=… fence_us=…` per present whose commit changed what the pointer shows (position, image, visibility), timed from the newest libinput event it reflects (µs, CLOCK_MONOTONIC) to the KMS flip timestamp. `submit_us` is when that commit's ioctl was issued and `fence_us` when the composite's sync_file signalled (kernel fence timestamp; 0 for no fence, a cursor-only commit, or a timing that cannot belong to this flip), which splits a present into compositor, GPU and display time; `latency summary n=… p50_us=… p95_us=… max_us=…` every 600 samples and at exit. On a virtual GPU the host draws the pointer, so "present" is the guest flip, not photons |
| `wayland` | Protocol dispatch. `info` once at start: `shell binary <canonical path>` (the only executable allowed to bind `zlm_shell_v1`); `warn` `shell binary … not found: zlm_shell_v1 binds will be denied`; `warn` per refused privileged bind: `privileged global zlm_shell_v1 denied: client pid=… exe=<path> is not the shell binary <path>` (or `… client uid=… pid=…` for another user) |
| `drm` | KMS page-flip |
| `gfx` | Device/import |
| `shell` | `lumen-shell` lifecycle. `info`: `connected to <display>; panel requested`, `signal <n>: exiting`. `error` (then exit 1): `zlm_shell_v1 bind rejected (…): protocol error on <iface>#<id>, code <c>`, `missing globals: …`, `compositor gone: …`, `panel closed; …` |
| `shell.bridge` | lumen-shell's task model from `zlm_policy_bridge_v1`. `info`: `capabilities flags=0x… version=…`, `snapshot toplevels=<n>`. `debug`: `snapshot toplevel handle=<slot> app_id=… title="…"`, `add handle=<slot> count=<n>`, `remove handle=<slot> count=<n>`, `title handle=<slot> app_id=… title="…"`, `state handle=<slot> state=<n>`, `focus handle=<slot>\|none`, `flags handle=<slot> flags=<toplevel_flag bits>` (zlm v2: 1 not responding, 2 demands attention; on change). Geometry and stacking deltas are stored, never logged (geometry arrives once per frame during a move) |
| `shell.panel` | lumen-shell's panel. `info`: `configure serial=… w= h=` when its size changes. `debug`: `layout idx=<i> x=<x> w=<w> handle=<slot> title="…"` (each entry, whenever the arrangement changes), `draw entries=<n> hidden=<k> focused=<i\|-1> damage=<rects>[(full)] repaint=<rects> buffer=<0\|1>` (one per commit: rects damaged against the last commit, rects repainted in that buffer), `click idx=<i> handle=<slot> title="…"` (a click that sends raise + activate). Never per motion event: hover redraws only when the entry under the pointer changes |
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
