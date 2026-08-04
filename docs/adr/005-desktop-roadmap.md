> **Status:** Proposed — where Lumen stands after ADR-004, and what it takes to become a
> desktop shell people would choose

# ADR-005: From render pipeline to desktop

**Date:** 2026-08-04
**Scope:** whole tree, forward-looking
**Deciders:** project owner
**Follows:** [ADR-004](004-architecture-evaluation.md)

---

## 1. Where the work went, and where it did not

ADR-004 rebuilt the frame pipeline, and it worked. Compositing is in linear light with a
float intermediate and an explicit encode pass, 10-bit scanout is a parameter, repaints are
scheduled against measured cost, present no longer blocks the render thread, the draw
contract carries the full payload, and the protocol boundary is fuzzed. That part of Lumen
is now genuinely modern — ahead of several shipping compositors.

The investment is also visible in the line counts:

| Area | Lines | State |
|---|---|---|
| `lx.gfx` | 5876 | Strong. Three backends, linear/HDR-capable, zero-copy import |
| `lx.compositor` | 4779 | Strong pipeline; thin protocol surface |
| `lx.ui` | 2421 | Two competing paradigms mid-migration |
| `lx.text` | 298 | One font, no line breaking, no fallback |
| `lx.shell` | 176 | Declarations only |
| `lx.shell.policy` | 112 | Declarations only |
| `lx.input` | **86** | **Empty — every body is `{}`** |
| `lx.a11y` | 74 | Stub |
| `lx.app` | 68 | Declarations only |

The compositor and graphics layers are 10,655 lines. Everything that makes the thing a
*desktop* — input, shell, toolkit, text, accessibility — is 3,235, and most of that is one
half-migrated widget tree.

**The binding constraint has moved.** It is no longer how well Lumen draws. It is that
almost nothing else exists yet.

## 2. Three findings that decide the plan

### F5 — Lumen cannot accept input at all

[modules/lx.input/lx.input.cppm](../../modules/lx.input/lx.input.cppm) is 86 lines and every
implementation is empty:

```cpp
void lx::input::seat::notify(pointer_event) {}
void lx::input::seat::notify(keyboard_event) {}
void lx::input::seat::process_events() {}
void lx::input::keyboard::set_focus(lx::surface_id) {}
lx::surface_id lx::input::pointer::focus() const { return {}; }
```

There is no libinput (CMake probes for it with `QUIET` and nothing links it), no
xkbcommon anywhere, no key repeat, no modifier state, no focus tracking, no serial
management, no touch, no tablet, no gestures. `wl_seat` is advertised, and there is nothing
behind it. The only xkb in the tree is a hardcoded US keymap *string* sent to clients so
their own libxkbcommon has something to parse.

A compositor that composites in linear light at ten bits with deadline-driven scheduling,
and into which you cannot type a character, is not yet a desktop. This is the single most
important gap and it blocks everything downstream: no input means no focus policy, no
keyboard shortcuts, no window management by the user, no text entry, no IME, no
accessibility.

### F6 — The shell protocol exists but is never advertised

`docs/architecture.md` describes the centrepiece of the design: the shell is a privileged
Wayland client, it binds `zlm_shell_v1`, and window-management intent flows over
`zlm_policy_bridge_v1` while the compositor enforces state.

Both sides are scaffolded — `lx.compositor/shell_bridge.cppm`, `lx.shell/bridge.cppm`,
`lx.session/privilege.cppm`. But `add_global` is called exactly seven times, and
`zlm_shell_v1` is not one of them:

```
wl_compositor  wl_subcompositor  wl_output  wl_seat
wl_data_device_manager  xdg_wm_base  zwp_linux_dmabuf_v1
```

So `lumen-shell` starts, connects, and finds nothing to bind. The architecture's defining
seam is not connected at runtime.

Seven of the ~38 protocols in `protocols/manifest.toml` are served. Missing and load-bearing
for a desktop: **layer-shell** (there is no way to place a panel, dock, wallpaper or
notification without it), **ext-session-lock** (no lock screen), xdg-decoration,
xdg-activation, xdg-output, presentation-time, viewporter, fractional-scale, text-input,
primary-selection, pointer-constraints, relative-pointer.

### F7 — `lx.ui` is two toolkits at once

There is a retained inheritance tree (`widget`, `label`, `button`, `entry`, `checkbox`,
`panel`, `image`, `scroll_area`, `window`) and a newer declarative node system
(`box_node`, `text_node`, `button_node` with `describe<>`, reconciliation and `operator|`
decorators). `docs/subsystems/ui-architecture.md` §9 calls the builder API superseded.

Carrying both is the largest piece of debt in the tree: every widget added has to be added
twice or picked a side, and neither is complete. The declarative side is the right one — it
is the model React, SwiftUI, Flutter and Slint converged on, and it is what makes state
handling and diffing tractable. The retained tree should go.

Under both, the text stack is one font with no line breaking, no bidi, no font fallback and
no colour emoji, which caps the toolkit well below usable regardless of which API wins.

---

## 3. Plan

Sequenced by dependency, not by appeal. Each phase is independently shippable and leaves the
tree in a coherent state.

### Phase 0 — Make it accept input (blocks everything) — **DONE**

> **Landed.** `lx.input` went from 86 lines of empty bodies to a real xkb keyboard state
> plus a libinput device layer, and events now reach clients. Details under each step.

1. [x] **libinput + logind device handling.** Devices open through a `device_provider`, so
   `lx.input` does not depend on `lx.session` and the seat stays testable against fixtures;
   the compositor supplies logind `TakeDevice` with a direct-open fallback. libinput's fd is
   an `fd_source` on the event loop. `suspend`/`resume` are exposed for VT switching — the
   fds must be released or the next session cannot claim them. Device add/remove is counted.
2. [x] **Server-side xkbcommon.** Real keymap compiled from configuration, with depressed,
   latched and locked modifiers tracked separately, named-modifier lookup for compositor
   keybindings, and per-key repeat flags. Clients receive the *compiled* keymap over a
   sealed memfd instead of the hardcoded US string — sealed because every client maps it
   shared, and without `F_SEAL_SHRINK` one of them could resize it under the others. Tested
   by checking a German layout produces `z` where US produces `y`, which the old string made
   impossible. The evdev→xkb keycode offset of 8 is applied in exactly one place; wrong by
   eight produces plausible letters, so it hides.
3. [x] **`wl_seat` for real.** `compositor::input_router` tracks every seat-derived
   resource per client and delivers keyboard enter/leave/key/modifiers and pointer
   enter/leave/motion/button/axis, with `axis_source` and v120 discrete values so a wheel
   notch and a touchpad drag are distinguishable. `enter` carries the held-key array and is
   followed immediately by modifiers, or a client inherits stuck modifiers across a focus
   change. Resources unregister on destroy, so the router cannot send into freed memory.
   Serials come from the seat's single monotonic counter.
4. [~] **Focus — placeholder policy.** A newly mapped toplevel takes keyboard focus, and
   `seat_manager` is bound to the compositor's real seat rather than a function-local one
   nothing else could see. Pointer and keyboard focus are tracked separately, which is what
   lets click-to-focus and focus-follows-mouse differ. **Not yet:** focus as a shell
   decision over the policy bridge, and per-surface pointer hit-testing — the latter wants
   D5's per-output loop, which is what makes "the surface under this point" well defined
   across several displays.

*Done when:* a real client receives a keystroke and a click, and `wev`/`weston-info` show a
correct seat. This is the milestone that turns Lumen from a renderer into a compositor.

**Verification status.** Everything above is unit-tested without hardware — 15 tests over
keymap compilation, modifier tracking, locked-vs-held, serial monotonicity and the held-key
set. The end-to-end path *cannot* be verified in this environment: it needs a seat with real
devices and permission to open them. The honest test is `wev` under a real session, and it
has not been run. Treat Phase 0 as complete in construction and unproven in the field until
someone types into it.

### Phase 1 — Make it a desktop — **in progress**

5. [x] **`zlm_shell_v1` is advertised**, and the registry now lists nine globals rather
   than seven. The protocol was already generated and both sides scaffolded; only the
   `add_global` call was missing, which is why `lumen-shell` connected and found nothing.
   `zlm_shell_v1`, `zlm_policy_bridge_v1`, `zlm_workspace_manager_v1` and
   `zlm_window_rules_v1` all bind, and `request_activate` /
   `set_toplevel_stacking_index` route to `shell_bridge`; binding the bridge emits the
   startup snapshot. Advertised to every client but bound only by one that passes the
   privilege check, so a rejected bind is a protocol error naming the reason rather than a
   global that mysteriously does not exist. Requests whose compositor-side action does not
   exist yet (interactive move/resize, workspace objects, rule persistence) accept and do
   nothing, so the shell can bind and drive what does work instead of failing at bind.
6. [ ] **wlr-layer-shell.** Next. Without it there is no panel, dock, wallpaper,
   notification or OSD — the shell has nowhere to put anything.
7. **ext-session-lock.** Security-critical and easy to get subtly wrong: the lock surface
   must survive a crashing locker, and input must not leak to normal clients while locked.
8. **xdg-decoration, xdg-activation, xdg-output, primary-selection.** The unglamorous set
   that makes real applications behave.

*Done when:* `lumen-shell` draws a panel, switches workspaces, and locks the session.

### Phase 2 — Cash in the pipeline work

ADR-004 made these implementable; they are now mostly protocol code.

9. **viewporter + wp-fractional-scale-v1** — D1 delivered `src`/`dst`, so this is
   negotiation plus plumbing.
10. **color-management-v1** — D2 delivered linear blending, PQ/HLG and 10-bit output. This
    is the differentiator: almost nothing ships working HDR on Linux.
11. **presentation-time, wp-fifo, wp-commit-timing** — D3 delivered the deadline model.
12. **D5, the per-output frame loop** — required for multi-monitor, per-output scale and
    mixed refresh rates. Plan is in ADR-004's "Needs revisiting"; step 1 (grouping
    per-output state into a struct) pays for itself alone.

### Phase 3 — A toolkit worth using

13. **Finish the declarative migration and delete the retained tree.** One paradigm.
14. **Real text.** Line breaking (UAX #14), bidi (UAX #9), font fallback chains, colour
    emoji, subpixel positioning. This is the difference between "renders text" and "renders
    text people read all day".
15. **Focus and input in the toolkit** — keyboard navigation, focus rings, IME via
    text-input-v3 and input-method-v2. IME is not optional; it is how most of the world
    types.
16. **Accessibility, from the start.** ADR-001 covers the AT-SPI bridge. Every toolkit that
    deferred this paid more later and shipped worse. Doing it while the widget set is small
    is the cheapest it will ever be.
17. **Scrolling, gestures, animation** driven by the frame clock D3 already provides.

### Phase 4 — Differentiate

18. **HDR end to end** — the pipeline is ready; it needs KMS `HDR_OUTPUT_METADATA` and
    `COLOR_ENCODING`, plus the protocol from step 10.
19. **VRR / adaptive sync** — the deadline scheduler is the hard part and it exists.
20. **ext-image-copy-capture + portal** — screencast and screenshot, the thing every
    meeting needs.
21. **wp-security-context and sandboxing** — per-client capability gating. Lumen's
    privilege checker is already the right shape for it.
22. **Xwayland**, rootless, for the long tail.

### Cross-cutting, start now rather than later

- **Input latency measurement.** Instrument end to end (event → present) and track it like
  the frame budgets already are. It is the number users feel and the one Lumen is currently
  best positioned to win.
- **An interaction test harness.** Golden images cannot test focus, drag, grabs or IME. A
  scripted Wayland client that drives real sequences is needed before Phase 0 lands, or none
  of Phases 0–3 will have meaningful coverage.
- **Finish D4's Vulkan 1.3 move** before the renderer grows more passes, not after.

---

## 4. What to do first

**Phase 0, step 1–3.** Nothing else changes what Lumen *is*. Every remaining item either
depends on input or is invisible without it, and the gap between "renders beautifully" and
"is a desktop" is exactly one working seat.

The honest framing for anyone reading the repo today: Lumen has an excellent display
pipeline and the skeleton of a desktop around it. ADR-004 made the first part true. This
plan is about the second.
