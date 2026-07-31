# Lumen Protocol IDL Pack

Wayland protocol definitions for the Lumen desktop stack.

## Layout

```
protocols/
├── manifest.toml          # Full protocol registry (upstream + lumen)
├── lumen/                 # Private Lumen DE protocols
│   └── zlm_shell_v1.xml
└── upstream/              # Fetched by scripts/fetch-protocols.sh
```

## Lumen private protocols

| Protocol | Interfaces | Access |
|----------|------------|--------|
| `zlm_shell_v1` | shell, toplevel, workspace, policy bridge, window rules | Privileged shell only |

Interfaces in `protocols/lumen/zlm_shell_v1.xml`:

| Interface | Role |
|-----------|------|
| `zlm_shell_v1` | Root; exposes workspace manager, policy bridge, window rules |
| `zlm_toplevel_v1` | Compositor-owned handle for mapped xdg toplevels |
| `zlm_workspace_manager_v1` / `zlm_workspace_v1` | Virtual desktops |
| `zlm_policy_bridge_v1` | Shell intent → compositor execution (focus, geometry, stacking) |
| `zlm_window_rules_v1` | Session window rules (config-driven) |

Key design points:

- **`zlm_toplevel_v1`** — compositor-owned handle (shell never uses client `xdg_toplevel`)
- **State sync** — `snapshot_*` batch on bind, then delta events
- **Stacking** — `set_toplevel_stacking_index` (valid Wayland IDL)
- **Shell chrome** — panels, docks, and server decorations use upstream protocols or compositor internals, not private wire objects

See [../docs/subsystems/shell-state-sync.md](../docs/subsystems/shell-state-sync.md).

## Generate C++26 bindings

```bash
cmake --build build --target wl-scanner-cpp
./build/wl-scanner-cpp \
  --input protocols/lumen/zlm_shell_v1.xml \
  --module lx.wayland.protocols.lumen \
  --output-dir build/generated/protocols
```

Outputs per protocol:

- `*.cppm` — opcodes, enums, interface metadata
- `*.dispatch.cppm` — dispatch tables for `ProtocolDispatcher`
- `*.gen.cpp` — module implementation units

## Upstream protocols

```bash
./scripts/fetch-protocols.sh   # reads manifest.toml via protocol_manifest.py
```

See `manifest.toml` for the P0–P2 matrix. P0/P1 codegen is enabled via CMake options
`LUMEN_PROTOCOLS_P1`, `LUMEN_PROTOCOLS_P2`.

## Privileged client model

The compositor advertises `zlm_shell_v1` only to clients that pass:

1. Same session UID (`SO_PEERCRED`)
2. Shell binary path match (configurable)
3. Optional Flatpak: portal capability token (`LUMEN_BUILD_FLATPAK_PORTAL`)
