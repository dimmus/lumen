# UI architecture

How `lx.ui` turns application state into draw commands.

**Related:** [threading.md](threading.md) · [rendering-performance.md](rendering-performance.md) ·
[architecture.md](../architecture.md)

---

## 1. The model in one paragraph

A build function describes the UI it wants as a tree of cheap **descriptors**
(`lx::ui::element`). The **reconciler** folds that description onto the **retained tree**
(`lx::ui::ui_node`), reusing a node whenever its type and key match so per-node state
survives. Retained nodes then run a two-pass layout and **emit draw commands** into the
scene graph, which snapshots to the render thread. UI is a function of state; nothing
outside the tree holds a node pointer.

```
state change
  → build(state) produces descriptors        (arena, thrown away each rebuild)
  → reconciler.apply(retained, described)    (reuses nodes by type + key)
  → measure / arrange                        (two passes, hint-driven)
  → emit(draw_scope)                         (appends scene::draw_command)
  → scene_graph::commit_frame                (index swap to render thread)
```

This is the reconciling-descriptor model, adapted from the same idea used by React and
by the Karm framework. The adaptation matters as much as the borrowing: Karm paints into
a CPU canvas, allocates retained nodes with reference counting on every rebuild, and
forces a full-window repaint on any relayout. None of those hold here — see §7.

---

## 2. Descriptors vs retained nodes

These are two different trees with two different lifetimes, and keeping them distinct is
the whole point.

| | Descriptor (`element`) | Retained node (`ui_node`) |
|--|------------------------|---------------------------|
| Lifetime | One rebuild | Until the tree shape changes |
| Storage | UI frame arena | Heap, via `construct` |
| Contents | Props by value, child pointers | Geometry, scroll offset, caret, animation phase |
| Identity | `type` + `key` | Same `type` + `key`, matched against descriptors |
| Destructor | None — trivially discarded | Virtual; owns its children |

Descriptors are plain data with no destructor, so a rebuild costs a bump-pointer
allocation per node and the arena reset at the end of the frame reclaims everything.

**Allocation rule:** describing is per-frame and arena-backed. Constructing a retained
node calls `new` and happens **only when the tree shape changes** — a node appears,
disappears, or changes type. Steady-state animation and state updates reuse every node
and allocate nothing but arena bytes.

---

## 3. Reconciliation and identity

`reconciler::apply(retained, desired)` returns the node that should occupy a slot:

- **type and key match** → re-apply props onto the existing node, recurse into children,
  keep all node-local state
- **anything else** → damage the vacated bounds, destroy the retained subtree, construct
  a fresh one

Children are matched by **key first, position second**. A keyed row is found anywhere
among the previous siblings, so reordering or filtering a list moves nodes instead of
rebuilding them. Unkeyed children match by index.

```cpp
// Rows keep their scroll position and caret across a reversal.
const child kids[3] = { row(30, /*key*/3), row(20, 2), row(10, 1) };
list = rec.apply(list, with_children(parent, kids, 3));
```

**Type ids must be unique.** `apply` is taken from the descriptor, not the node, so two
types sharing an id would let one node be `static_cast` to the other. Always allocate
ids through `next_element_type_id()` from a function-local static:

```cpp
[[nodiscard]] static element_type_id static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
```

**Never store a `ui_node*` in application state.** State holds plain data; the tree is
derived from it. A stored node pointer dangles the moment its type changes.

---

## 4. Composition: decorators, not builders

Modifiers are values that wrap one descriptor in another, composed with `operator|`:

```cpp
auto content = label("Hello")
    | insets(8)
    | center()
    | pin_size({300, 300});
```

Every modifier has a direct form (`insets(padding, content)`) and a curried form
(`insets(padding)`) returning a `child -> child` callable. Because each call produces a
fresh descriptor, two subtrees can be decorated independently — which is exactly what a
`static` fluent builder shared across calls gets wrong, and why that pattern is listed as
an anti-pattern in `AGENTS.md`.

Built-in proxies: `insets`, `align` / `center`, `sizing` / `pin_size` / `min_size`,
`grow`, `opacity`, `clip`, and `keyed`.

Deferred subtrees use a `slot` thunk so overlays and collapsed panels are not described
until they are shown.

---

## 5. Layout

Two passes, mirroring the constraint model already in `lx.layout`:

1. **`measure(constraints, size_hint)`** — must not mutate observable state; it is called
   more than once per pass. `size_hint` selects `min`, `preferred`, or `max`, which is
   what lets a flex container distribute slack without guessing.
2. **`arrange(bounds)`** — assigns final geometry and recurses.

Layout invalidation is scoped to a subtree via `mark_layout_dirty()`. It deliberately does
**not** imply a full-surface repaint.

---

## 6. Painting: draw emission, never rasterization

`ui_node::emit(draw_scope&)` appends `lx::scene::draw_command`s. Widgets do not
rasterize, do not touch Vulkan, and do not receive a graphics context.

`draw_scope` keeps a fixed-depth stack of clip, opacity, and origin, applied on the way
in so a child cannot draw outside its parent. Fully clipped quads are dropped before they
reach the draw list. `fill()` emits a null-texture quad, which the render side treats as a
solid colour needing no binding.

The stack depth is capped at 32 frames; exceeding it keeps the innermost frame rather
than growing, so a pathological tree cannot allocate mid-frame.

> The previous `widget::paint(lx::gfx::render_pass&)` signature is superseded. `render_pass`
> was an empty placeholder class, so nothing painted through it could ever reach the
> screen. See §9.

---

## 7. Damage

Widgets report what changed; the root turns that into compositor damage.

```cpp
invalidate_paint(node);           // node's own bounds
invalidate_paint(node, region);   // sub-region, e.g. a caret or hover ring
invalidate_layout(node);          // subtree geometry, damages current bounds
invalidate_animate(node);         // request a frame tick
```

`damage_ledger` accumulates regions into a **fixed 16-slot array**. A region that can be
absorbed into an existing rect for free is merged in place; once full, the ledger merges
the pair whose union wastes the least area. It never grows, satisfying the overflow-cap
rule for per-frame structures, and repeated invalidation of one region stays one slot.

Two deliberate differences from the model this was adapted from:

- **Rects are merged.** Karm accumulates them unbounded and never coalesces.
- **Relayout does not damage the whole surface.** Karm marks the entire window dirty on
  any layout change, which would make the damage useless to KMS. Here relayout damages
  the affected subtree's bounds before arrange, and the root adds the new bounds after,
  so a resize repaints both the vacated and the newly covered area.

Merged regions are what should feed `kms_damage_region` on the atomic commit path.

---

## 8. Threading

Everything in this document is **UI affinity**. `build_scope` asserts it.

The render thread never sees a `ui_node` or an `element`; it reads the immutable frame
snapshot produced by `scene_graph::commit_frame`. Cross-thread results reach the UI via
`executor.post(affinity::ui, …)`, which then dispatches an action.

Descriptor storage comes from `memory_pool::ui` and is reclaimed by
`reset_frame_arenas(memory_pool::ui)`. If the arena is exhausted mid-rebuild,
`build_context::overflowed()` becomes true, a warning is logged once, and the caller
**keeps the previous retained tree** rather than reconciling against a truncated
description.

---

## 9. Migration status

The declarative core (`:element`, `:node`, `:invalidate`, `:reconcile`, `:decorator`,
`:reducer`) is in place and covered by `tests/test_ui_reconcile.cpp`.

The legacy retained-widget API (`:widgets`, `:widget_node`) is still exported so existing
callers compile. It is superseded and should not gain new widgets:

| Legacy | Replacement |
|--------|-------------|
| `widget` base with raw `widget*` children | `ui_node` with owned children + reconciliation |
| `widget::paint(gfx::render_pass&)` | `ui_node::emit(draw_scope&)` |
| `widget::invalidate()` (no-op) | `invalidate_paint` / `invalidate_layout` |
| `widget::preferred_size(constraints)` | `measure(constraints, size_hint)` |
| `lx.ui.builder` fluent builders | `operator|` decorators |

Still to build:

- Concrete widgets (`label`, `button`, `entry`, `checkbox`, `scroll_area`) as `ui_node`s
- A root host that owns the reducer rebuild → layout → emit → damage sequence per tick
- Flex and grid containers reading `grow_node::grow_factor()` off their children
- Overlay layers (dialog, popover) that defer show/close to the next frame
- Feeding merged damage into `kms_damage_region`
- A design-system layer (shell chrome: scaffold, rows, toolbars) as a separate module
  above `lx.ui`, keeping primitives and opinionated components apart
