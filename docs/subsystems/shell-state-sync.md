> **Status:** Current (v0.3) — see code for implementation truth

# Compositor ↔ Shell State Synchronization

Multi-process reliability depends on the shell receiving a **complete initial snapshot**
on bind, then **delta events** for all state changes.

## Protocol surface

Implemented in `zlm_policy_bridge_v1` (`protocols/lumen/zlm_shell_v1.xml`):

| Phase | Events |
|-------|--------|
| Initial sync | `snapshot_begin` → `snapshot_workspace*` → `snapshot_toplevel*` → `snapshot_output*` → `snapshot_done` |
| Runtime deltas | `toplevel_created`, `toplevel_destroyed`, `toplevel_state`, `toplevel_geometry`, `focus_changed`, `stacking_order_changed` |

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
mapped. The compositor's own placeholder policy stays in force alongside these requests.
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

`zlm_shell_v1` global is advertised only when:

1. `SO_PEERCRED` UID matches compositor session UID
2. Client executable matches configured shell path (default: `lumen-shell`)
3. Optional Flatpak build: valid portal capability token

Enforced in `lx::wayland::server::allow_privileged_global()`.

## Window rules

Rules load from `$XDG_CONFIG_HOME/lumen/window-rules.toml` via `reload_rules`.
Typed setters replace stringly-typed key/value pairs.
