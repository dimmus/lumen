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
