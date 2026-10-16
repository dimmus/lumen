> **Status:** Current (v0.3) — see code for implementation truth

# Compositor ↔ Shell State Synchronization

Multi-process reliability depends on the shell receiving a **complete initial snapshot**
on bind, then **delta events** for all state changes.

## Protocol surface

Implemented in `zlm_policy_bridge_v1` (`protocols/lumen/zlm_shell_v1.xml`):

| Phase | Events |
|-------|--------|
| Bind | `zlm_shell_v1.capabilities(flags, shell_protocol_version)`, sent immediately after the bind (flags 0 today: no tiling plugin, workspace drag or explicit sync yet; version 3) |
| Initial sync | `snapshot_begin` → `snapshot_workspace*` → (`snapshot_toplevel` [→ `toplevel_flags`])* → `snapshot_output*` → `snapshot_done` → `focus_changed` |
| Runtime deltas | `toplevel_created` (→ `toplevel_state`, `toplevel_geometry` [, `toplevel_flags`]), `toplevel_destroyed`, `toplevel_state`, `toplevel_geometry`, `toplevel_flags` (v2), `focus_changed`, `stacking_order_changed` |

The snapshot carries no focus, so the compositor follows `snapshot_done` at once with a
`focus_changed` naming the focused window (or null). It is an ordinary v1 delta: no XML
change and no version bump, and a shell can highlight the focused window from its first
frame instead of waiting for the next focus change.

### Version 2: attention flags

Every zlm interface is at version 2 (the family moves together: child objects take their
parent's version, and `zlm_toplevel_v1` handles take the bridge's). Version 2 adds the
bitfield enum `toplevel_flag` (`not_responding` = 1, `demands_attention` = 2) and the event
`toplevel_flags(toplevel, flags)`, sent right after `snapshot_toplevel` and after
`toplevel_created` when the flags are non-zero, and whenever they change. Only bridges bound
at version 2 receive it; a version 1 shell binds version 1 and never sees it.

### Version 3: logout

The family moves to version 3 together. `zlm_shell_v1.request_logout` asks the compositor
to end the session: it sends `xdg_toplevel.close` to every window, stops once none is left
or after a 5 s grace, and its teardown restores the console — nothing is killed. The
compositor's own Ctrl+Alt+Backspace runs the same logout, so there is a way out even with
no shell. Either way the bound shell first receives `zlm_shell_v1.session_ending`, so it
reads the disconnect that follows as the end of the session, not a crash. lumen-shell binds
`min(advertised, 3)` and shows a "Log out" button only when version 3 is bound: one click
arms it ("Confirm?"), a second sends the request, moving off disarms it.

- **`not_responding`**: the client left an `xdg_wm_base.ping` unanswered past the
  compositor's timeout (`--ping-timeout`, 5 s). The compositor only shows it — "(Not
  Responding)" in its server-side title, the window dimmed, pointer motion withheld — and
  clears it when the client answers. It never kills the client: the shell may offer "wait /
  force quit", and that is the user's decision.
- **`demands_attention`**: an `xdg_activation_v1.activate` of the window was refused focus
  (focus-stealing prevention: the token was unknown, expired, issued without a serial or
  not from the issuer's own input, or the user pressed a key or button in another window
  after it was issued). The window keeps its place in the stacking order — a new window
  refused at map opens just below the focused one — and the shell is expected to draw
  attention to its task (lumen-shell: an amber fill). Never set on the window that has the
  keyboard; cleared when the window gets the keyboard (a `request_activate` from the shell
  included) or unmaps.

## Compositor-owned handles

Shell never references client-side `xdg_toplevel` objects. All WM operations use
`zlm_toplevel_v1` handles created by the compositor when an xdg toplevel maps.

- A handle is server-created (`wl_resource_create(…, id 0)`) and introduced to the shell by
  the `new_id` argument of `toplevel_created` (a window mapping while the bridge exists) or
  `snapshot_toplevel` (a window already mapped when the bridge is created). Its user data
  is the `toplevel_id`; requests decode it with `shell_bridge::toplevel_from_handle`.
- On unmap the compositor sends `toplevel_destroyed` and makes the handle **inert** (user
  data 0): later requests naming it are ignored. The shell releases it with
  `zlm_toplevel_v1.destroy`; the compositor never destroys the shell's reference.
- A new policy bridge replaces the previous one; the previous bridge's handles go inert.
- `workspace` is **null** in `toplevel_created` and `snapshot_toplevel` until workspace
  objects exist: a compositor cannot introduce a `zlm_workspace_v1` in this protocol
  version (no event carries one as `new_id`), and an unknown object id is fatal to the
  client. No `snapshot_workspace` is sent for the same reason, and no `snapshot_output`
  (its `wl_output` is not nullable and the shell may not have bound one) — a known gap
  until per-client output objects are tracked (D5).

## Geometry and state deltas

`toplevel_geometry` carries the **window geometry** (`xdg_surface.set_window_geometry`, or
the whole surface when unset) in global coordinates — the compositor's
`toplevel_record::geometry`, the one source of truth. It is sent after `toplevel_created`,
and whenever a commit changes the window's size or position. `toplevel_state` follows
`toplevel_created` and every title or app-id change of a mapped window. `focus_changed`
follows every keyboard-focus change (null when nothing is focused). `stacking_order_changed`
is sent as one bottom→top batch per tick in which the order changed (a map, an unmap, a
raise-on-press or a shell request), with `done=1` on its last entry.

## Focus and stacking requests

The compositor honours these shell requests:

- `request_activate` sets keyboard focus and raises the window.
- `request_raise` and `request_lower` move it to the top or the bottom.
- `set_toplevel_stacking_index` places it at an index, where 0 is the bottom and anything
  past the top is clamped to the top.
- `request_close` sends `xdg_toplevel.close`.

A request on an inert handle is ignored, and so is a request for a window that is not
mapped. Every request is logged at `debug` (category `compositor.shell`) with what became
of it: `bridge request=<activate|raise|lower|stacking|move|resize|maximize|unmaximize|close|fullscreen|minimize> id=<toplevel> result=<ok|inert|unmapped|ignored>`
(`ignored`: a grab request that was rejected, or fullscreen/minimize, which have no
compositor side yet). Bridge creation and destruction are logged at `info`:
`bridge bound client=<cid> snapshot=<n> focus=<toplevel>` and `bridge destroyed`. The compositor's own placeholder policy stays in force alongside these requests.
Until the shell decides focus itself, a click focuses and raises the window under the
pointer, and a newly mapped window takes keyboard focus. Pointer focus is never policy. It
always follows the hit test, and while a button is held it follows the implicit grab.

## Interactive move and resize requests

`request_move(toplevel, seat, serial)` and `request_resize(toplevel, seat, serial, edges)`
start the same grab that `xdg_toplevel.move`/`.resize` and the placeholder Super+drag
binding start (`window_manager::begin_grab`). The shell is trusted to name the toplevel,
but a grab must still answer a live user press:

- `serial` must be the serial of the last button press the compositor delivered, to any
  client (normally the shell's own panel surface), and that button must still be held;
- the toplevel must be mapped and in the normal state, and no grab may be in progress;
- `edges` must be one of the nine `resize_edge` values and non-zero for a resize.

A request that fails any of these is ignored and logged at `debug` as
`grab rejected id=… source=shell reason=serial|unmapped|no-button|state|edges|grab-active`.
It is not a protocol error: the shell is part of the desktop, and killing it over a stale
serial would take the panel down with the request. (A client's invalid `xdg_toplevel.resize`
edge *is* a protocol error, as xdg-shell specifies.)

While the grab runs, the window's `toplevel_geometry` is sent at most once per tick for a
move, and once per committed size for a resize. The grab ends when the grabbing button is
released, on Escape (which restores the start geometry), or when the window unmaps or its
client disconnects.

## Stacking order wire format

Invalid (removed):

```xml
<!-- length + implicit array — NOT valid Wayland IDL -->
```

Valid:

```xml
<request name="set_toplevel_stacking_index">
  <arg name="toplevel" type="object" interface="zlm_toplevel_v1"/>
  <arg name="index" type="uint"/>
</request>
```

Compositor emits `stacking_order_changed` with `done=1` on the last entry in a batch.

## Sequence diagram

See [../uml/21-shell-state-sync.mmd](../uml/21-shell-state-sync.mmd).

## Privilege model

`zlm_shell_v1` is advertised to every client, so a refused shell gets an error it can
report rather than a global that silently does not exist. Binding it requires:

1. `SO_PEERCRED` UID matches the compositor's UID;
2. the client's executable — `/proc/<pid>/exe`, as the kernel names it — is the
   configured shell binary;
3. (optional Flatpak build, not built by default) a valid portal capability token.

The shell binary is `config::shell_binary_path`, set with `lumen-compositor --shell PATH`.
A bare name (the default, `lumen-shell`) is looked up next to the compositor's own
executable, so an installed or freshly built pair matches; the path is resolved once, at
start, to its canonical form (`realpath`) and logged (`wayland: shell binary <path>`).
`lx::wayland::server::allow_privileged_global()` reads the peer's executable and decides
through `lx::session::privilege_checker::check_shell_bind`. A refused bind ends the
connection with the display's `implementation` error ("privileged global denied"), and the
compositor warns with both paths:
`privileged global zlm_shell_v1 denied: client pid=<pid> exe=<path> is not the shell binary <path>`.

The pid is the one `SO_PEERCRED` recorded at connect; the connection is still open, so
the process is alive and its pid cannot have been reused. This is the check KWin applies
to its restricted interfaces. It is not a boundary against a same-UID process that can
ptrace the shell — nothing at this layer is — but it stops any other same-UID client from
binding the shell's global and replacing its bridge. A second instance of the real shell
binary does replace the bridge (the newest wins, the previous bridge's handles go inert,
and the compositor warns `bridge replaced`).

## The shell: lumen-shell

`executables/lumen-shell` runs `lx::shell::shell_app` (module `lx.shell`), a single-thread
libwayland-client program (`lx.wayland.client` is still a stub; see ADR-005 Phase 3):

- **Startup.** Bind `wl_compositor` (v4+), `wl_shm`, `zwlr_layer_shell_v1` (v4),
  `zlm_shell_v1` (v2, or what the compositor advertises) and, if present, `wp_cursor_shape_manager_v1`; the bridge listens on
  `zlm_shell_v1` in the same dispatch as the bind, because `capabilities` follows at once
  and libwayland drops events for a proxy with no listener. A second roundtrip surfaces a
  refused bind as a protocol error: the shell logs why and exits 1. Then
  `get_policy_bridge`, and the panel's layer surface. `wl_seat` is bound just before the
  panel is created, for the same reason as `zlm_shell_v1`.
- **Loop.** `wl_display_prepare_read` / `dispatch_pending` / `flush` (POLLOUT on EAGAIN),
  then `ppoll` on the display fd and a `signalfd` for SIGINT/SIGTERM. No timers: an idle
  shell sleeps. SIGINT/SIGTERM exit 0 after destroying every object; a refused bind, a lost
  compositor or a `closed` panel exit 1, and `lumen-shell.service` (`Restart=on-failure`)
  starts it again.
- **Task model** (`lx.shell:task_model`, pure). A fixed table of 256 entries — app_id, title
  (cut at 255 bytes on a UTF-8 code point boundary), state, geometry, stacking — addressed
  by a `task_handle {index, gen}` whose generation never repeats, so a handle kept across a
  removal cannot name the entry that reuses its slot. Each `zlm_toplevel_v1` proxy's user
  data is its slot. Entries keep creation order, the conventional task-bar order, not
  stacking order: the compositor lists the snapshot in creation order (each with its
  `stacking_index`), so a restarted shell shows the order it had. While a snapshot is in progress the
  model reports `in_snapshot()` and the panel keeps what it last drew, so half a snapshot is
  never shown. On `toplevel_destroyed` the shell removes the entry and destroys its proxy.
- **Panel** (`lx.shell:panel`). A layer surface on `top`, anchored top/left/right, 32 px,
  exclusive zone 32, keyboard interactivity `none`, namespace `lumen-panel`. One entry per
  window, left to right: 24 px tall, 4 px apart, width `(W − 8 − 4·(n−1)) / n` clamped to
  48–220 px; entries that do not fit at 48 px collapse into a "+k" label. The focused
  entry is highlighted; hover and press are shown; an entry demanding attention gets an
  amber fill, and a not-responding window's label is drawn dimmed (`toplevel_flags`). Labels are the title (else the app_id),
  rasterized with `lx::text` (`shape_line` / `fit_line` / `render_line`, Sans 10, ellipsized
  at a cluster boundary). XRGB8888 in two buffers of one sealed memfd pool, re-created only
  when the width changes.
- **Drawing is event-driven and incremental.** Model changes, pointer motion and configures
  only mark the panel dirty; after each dispatch it draws at most once per frame callback
  (a configure is always answered by a commit). The layout is computed and compared twice:
  against what the chosen buffer already holds (only differing entries are repainted) and
  against what was last committed (only differing entries are damaged with
  `wl_surface.damage_buffer`). A focus change damages two entries, a retitle one, a hover
  change one or two; adding or removing a window re-lays out and damages the panel.
- **Input.** A left press and release on the same entry sends `request_raise` then
  `request_activate` (activate raises too; raise is sent so the intent stays explicit if
  focus and stacking policy ever separate). The pointer shape is `default` through
  `wp_cursor_shape_v1` on enter.
- **Logs** (`LUMEN_LOG` as for the compositor): `shell` (startup, exit), `shell.bridge`
  (`capabilities`, `snapshot toplevels=<n>` at info; `snapshot toplevel`, `add`, `remove`,
  `title`, `state`, `focus handle=<slot|none>`, `flags handle=<slot> flags=<bits>` at debug), `shell.panel` (`configure` at info;
  `layout idx=<i> x=<x> w=<w> handle=<slot> title="…"` on every re-layout, `draw entries=<n>
  hidden=<k> focused=<i> damage=<rects>[(full)] repaint=<rects> buffer=<b>` and
  `click idx=<i> handle=<slot>` at debug).

## Window rules

Rules load from `$XDG_CONFIG_HOME/lumen/window-rules.toml` via `reload_rules`.
Typed setters replace stringly-typed key/value pairs.
