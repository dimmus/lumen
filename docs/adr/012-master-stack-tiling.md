# ADR 012 — Master-stack tiling, per workspace, in the compositor

**Status:** Accepted — implemented (T1–T8).

**Context.** AGENTS.md §1 promised "stacking-first, optional tiling **plugin**", and the tree
carried the scaffold for one: `LUMEN_ENABLE_TILING_PLUGIN` (a CMake option whose only effect
was a global definition), `lx::shell::tiling_policy` and `policy_registry::load_tiling_plugin`
(every method empty, `load_tiling_plugin` false in both branches, nothing calling
`use_tiling`), and `compositor::config::enable_tiling`, which nothing read. What did work was
snapping (ADR 011 K3–K4): halves, quarters, maximize, edge zones. That is manual tiling. There
was no automatic layout.

Since ADR 011, window geometry, workspaces and key bindings are **compositor state**, so
window and workspace actions keep working while the shell restarts. A layout is window
geometry per workspace, driven by key bindings.

Prior art read for this decision:

| | Model | Where the layout runs | What it costs |
|---|---|---|---|
| dwm | master-stack (`tile`), `nmaster`, `mfact` 0.55, monocle, floating | in the WM | — |
| Hyprland | dwindle (default) and `master` (`mfact` 0.55, `new_status`, `orientation`, `swapwithmaster`, `addmaster`/`removemaster`, `cyclenext`) | in the compositor | — |
| river | main/stack from an external *layout generator* (rivertile: `main-ratio` 0.6, `main-count` 1) over `river-layout-v3` | separate client process | a round trip per relayout; river holds the new frame for it, with a timeout |
| sway / i3 | tree of split, tabbed and stacked containers; dialogs and fixed-size windows float | in the compositor | the largest action surface (split direction, focus/move by direction, layout modes) |
| niri | scrollable columns on an endless strip per workspace | in the compositor; the initial configure already carries the column size | off-screen windows, view animation |
| KWin | quick tiling plus custom tile regions (Meta+T editor); automatic tiling only from scripts (Krohnkite, Polonium) | scripts in the compositor process | script ↔ compositor marshalling; scripts break across Plasma releases |
| GNOME | edge tiling only; Pop Shell and Forge as extensions | in gnome-shell, which *is* the compositor | extensions break across releases |

---

## Decision

### T1. One layout, master-stack, chosen per workspace

Each workspace has a layout: `floating` (stacking, the default; nothing changes for anyone who
does not ask) or `master-stack`. The layout follows dwm's `tile`: the first `master-count`
windows split the master column, the rest split the stack column, and the master column
takes `master-ratio` of the width. With no stack windows, the masters take the whole width.
With `master-count` 0, the stack does. The layout lives in the workspace record
(`workspace_record::layout`, `master_count`, `master_ratio`), so it survives everything the
workspace survives: output hotplug, `assign`, a merge (the surviving workspace keeps its
own). `layout.toggle` (Super+T) flips the focused output's active workspace.

Master-stack was chosen over the tree and the strip because it is predictable, needs no
manual split decisions, and is the smallest action set that still covers the whole model
(dwm's eight bindings). It also does not overlap with what snapping already does. A tree
would duplicate snapping's halves and quarters with more state. A strip changes how
workspaces and outputs work (windows off screen), which ADR 011's model does not have.

### T2. The compositor executes it, not a plugin and not the shell

The layout is a pure function in `lx.compositor:tiling`
(`master_stack_layout(area, count, master_count, ratio, gaps)`). `window_manager` applies it
whenever membership or the area changes (`flush_tiling`, from the end of every operation,
the same point that already flushes workspaces). There is no `dlopen`, no policy object and
no bridge traffic. The shell keeps the UI. It sees tiled windows as `toplevel_state.tiled`,
as it does for snapped ones.

*Rejected:* a `dlopen` plugin (an ABI for a layout function nobody else provides; the old
option was never built). Shell-side policy over `zlm_policy_bridge_v1`, as KWin scripts and
the GNOME extensions do: every relayout would cost a round trip, a shell restart would freeze
the layout, and ADR 011 already put geometry and bindings in the compositor. river's external
layout generator: the same round trip, which river has to hide by holding frames.

### T3. What tiles

On a `master-stack` workspace, a window joins the layout when it is mapped (or has had its
initial configure there, T4) and it has none of these:

- a parent (dialogs and transients float, as in dwm, sway and Hyprland);
- a fixed size (`min_size == max_size`, non-zero; dwm's `isfixed`);
- the sticky flag;
- the floating flag the user set (`window.float-toggle`, Super+Shift+Space, i3's and sway's
  key).

A minimized window gives up its place, and the others close the gap. When it is restored it
returns to the end of the order. A maximized or fullscreen window **keeps** its slot: the
other windows do not move, and leaving that state puts it back in its tile (Hyprland).

Joining saves the window's floating geometry (its normal geometry if it was snapped or
maximized), clears a snap, and makes the configure carry all four xdg `tiled_*` states.
Leaving (floated, the workspace toggled back, moved to a floating workspace) restores that
geometry and clears the states. A window that was never floating keeps the tile's size and
is raised (dwm). New windows go to the end of the stack (Hyprland's `new_status = slave`), so
opening a window does not move the master.

### T4. A new window is configured for its tile before it maps

On the initial commit, a window that will tile on its placement output's active workspace is
counted in the layout at once. Its initial configure carries the tile's size and place, and
the commit that answers it maps the window already in its tile. Nothing is drawn at a
cascade position first and then resized (niri does the same; sway configures after map).
The other windows of the workspace are configured for their new tiles at the same moment.
If the window never maps, its slot goes when it is destroyed.

### T5. Geometry and configures

A tile is a **frame** rect. A server-side frame's extents come out of it, exactly as for a
snap. `gaps` (default 8 logical px) is both the outer margin and the space between tiles.
Tiles split the column with no lost pixels (`⌊A·(i+1)/k⌋ − ⌊A·i/k⌋`). A window whose tile
changed size gets one configure with the size and the place, applied by the commit that
answers it, so no frame shows a new size at an old place. A window whose tile only moved (its
neighbour closed above it, a panel moved) is moved without a configure. Unchanged windows get
nothing. A client that draws smaller than its tile (a terminal's cell grid) sits at the tile's
top-left. A client whose minimum is larger than its tile keeps the minimum. All of this runs on
changes only: the frame path has no tiling work.

### T6. The pointer

- **Move** of a tiled window: it follows the pointer at its tile size. Edge snapping and
  drag-to-restore do not apply. Dropped on another tiled window of the same workspace, the
  two swap places. Dropped on another output's tiled active workspace, the window joins it
  at the end. Dropped anywhere else, or cancelled with Escape, it goes back to its tile.
- **Resize** of a tiled window floats it at its current geometry, then resizes it (dwm's
  Mod+right-drag). The other windows close the gap.
- Snap keys: Left/Right do nothing to a tiled window. Up maximizes and Down minimizes it,
  as for any window.

### T7. Actions and default bindings

| Action | Default | dwm / Hyprland master |
|---|---|---|
| `layout.toggle` | Super+T | `setlayout` / — |
| `window.float-toggle` | Super+Shift+Space | `togglefloating` / `togglefloating` |
| `layout.focus-next`, `layout.focus-previous` | Super+J, Super+K | `focusstack` / `cyclenext`, `cycleprev` |
| `layout.swap-next`, `layout.swap-previous` | Super+Shift+J, Super+Shift+K | — / `swapnext`, `swapprev` |
| `layout.promote` | Super+Shift+Return | `zoom` / `swapwithmaster` |
| `layout.master-grow`, `layout.master-shrink` | Super+equal, Super+minus | `setmfact` ±0.05 / `mfact` |
| `layout.master-more`, `layout.master-fewer` | Super+I, Super+D | `incnmaster` / `addmaster`, `removemaster` |

Focus and swap wrap around the workspace's layout order. `promote` swaps the focused window
with the first master; if the focused window is the first master, it swaps with the next
window. The ratio steps 0.05 within [0.1, 0.9]. The count goes from 0 to 16. dwm's Mod+H/L
are not used because Super+H minimizes and Super+L locks (ADR 011). On a floating workspace
the `layout.*` actions do nothing.

### T8. Configuration

```toml
[tiling]
layout = "floating"      # or "master-stack": the layout every workspace starts with
master-ratio = 0.55      # 0.1 – 0.9
master-count = 1         # 0 – 16
gaps = 8                 # logical px, 0 – 64
```

All four keys are live. A reload that changes `layout`, `master-ratio` or `master-count`
applies the new value to every workspace (the file is the user's latest word). One that leaves
them unchanged keeps the per-workspace values set with the actions.

---

## Consequences

- `LUMEN_ENABLE_TILING_PLUGIN`, `lx::shell::tiling_policy`, `policy_registry::use_tiling` /
  `load_tiling_plugin` and `compositor::config::enable_tiling` are removed.
  `zlm_shell_v1.capabilities.tiling` stays reserved and is not sent, because the shell has
  nothing to do differently.
- Stacking stays the default (AGENTS.md §1). A configuration without `[tiling]` behaves exactly
  as before.
- Tests: `test_wm_geometry` covers the pure layout (splits, gaps, no lost pixels, count and
  ratio edges). `test_tiling` covers it end to end against the in-process headless compositor:
  the initial configure carries the tile, the reflow on map, unmap and minimize, floating
  dialogs, float-toggle, swap and promote, ratio and count, a workspace toggle and back
  (floating geometry restored), maximize keeping the slot, and a drag swap. `test_config`
  covers the keys and actions.
- Trade-offs: a single orientation (master on the left). One layout, with no monocle or
  dwindle. Resizing a tile with the pointer floats it instead of moving the split. Each is a
  small addition to `:tiling` if users ask for it, and none is worth its keys before then.
