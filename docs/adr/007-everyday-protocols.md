> **Status:** Accepted. Part 1 (wl_surface completeness, wp_viewporter,
> wp_fractional_scale_v1, fractional output scale), Part 2 (wp_presentation, the
> visibility pass, frame-callback throttling) and Part 3 (ext-idle-notify, idle inhibitors,
> display power, lumen-shell's idle policy), Part 4 (ext-session-lock, logind lock
> integration, zlm_shell_v1 v5, lumen-shell's lock policy) and Part 5 (text-input-v3,
> input-method-v2, virtual-keyboard-v1) implemented.

# ADR-007: The everyday protocols

**Date:** 2026-10-16
**Scope:** `lx.foundation` (`rect2f`), `lx.scene` (output projection), `lx.gfx` (source
basis, float source boxes, buffer transforms in every backend), `lx.compositor`
(`:viewport`, `:output`, `:surface`, `:cursor`, `:decoration`, `wire_surface.cpp`,
`wire_viewporter.cpp`, `wire_fractional_scale.cpp`, `wire_output.cpp`), `lx.shell`
(`:shm_canvas`, panel, launcher); Part 2: `lx.drm` (flip tag, vblank sequence and
timestamps), `lx.scheduler:presentation`, `lx.scene` (visibility pass), `lx.compositor`
(`:presentation_feedback`, `wire_presentation.cpp`, frame callbacks in `wire_surface.cpp`);
Part 3: `lx.drm` (`commit_power_off`), `lx.compositor` (`:idle`, `:display_power`,
`wire_idle.cpp`, `wire_output_power.cpp`, the display gate, `window_manager` suspend reasons),
`lx.shell` (`:idle_policy`); Part 4: `lx.compositor` (`:session_lock`, `wire_session_lock.cpp`,
the lock layer, the window manager's lock gate, `keybindings::set_locked`), `lx.scene` (clear
colour per snapshot), `lx.session` (Lock, Unlock, PrepareForSleep, LockedHint, the sleep
delay inhibitor), `protocols/lumen/zlm_shell_v1.xml` (v5), `lx.shell` (bridge, lock policy);
Part 5: `protocols/manifest.toml` (a `[wlroots]` source), `lx.input` (`normalize_keymap`),
`lx.compositor` (`:text_input`, `wire_text_input.cpp`, `wire_input_method.cpp`, the input
router's grab routing and keymap switching, the IME popup layer, `root_kind::input_popup`)
**Deciders:** project owner

---

## 1. Context

Real clients expect a handful of protocols every desktop compositor has: viewporter and
fractional scale (foot, GTK 4, Qt 6, Chromium, Firefox), presentation-time (mpv, games,
GTK's frame clock), idle notification and inhibition (swayidle, video players), session
lock (swaylock, hyprlock), text input and input methods (fcitx5, ibus). Lumen advertised
none of them; `protocol_managers` held placeholder state for several, and `wl_surface`
ignored `set_buffer_scale` and `set_buffer_transform` — so the preferred buffer scale had
to be clamped to 1, and a fractional output scale did not exist at all (the per-tick
`protocols_.fractional_scale()` loop stored 1.0 forever).

Prior art read in full: wlroots (`wlr_compositor.c`, `wlr_viewporter.c`,
`wlr_fractional_scale_v1.c`, `scene/wlr_scene.c`, `scene/surface.c`, `util/box.c`), niri
(`utils/scale.rs`, `round_logical_in_physical`), KWin, Mutter, the protocol XML
(wayland-protocols 1.49).

## 2. Decisions (Part 1)

**D1 — One logical space; the projection in one place.** The window manager, the surface
tree, scene nodes, input and output membership work in logical layout coordinates
(integers, as before). The output projection (origin, scale) is applied only when the scene
emits draw commands and converts damage; draw destinations, scene damage and KMS damage stay
output pixels, so the render thread is unchanged by scale. This is wlroots' scene model.
Every consumer that meant "output pixels" was found by renaming `output_manager::geometry`
into `logical_rect` and `mode_rect`.

**D2 — Edge rounding relative to the window root.** A root lands on `round((r − o)·s)`;
every node of its tree rounds both edges relative to it (`R + round(Δ·s)`,
`R + round((Δ + w)·s)`). A fractional-scale client renders `round(w·s)` pixels, so its draw
is exactly that wide: a 1:1 texel mapping (unit-tested for every root offset 0..119 and
width 1..2000 at six scales; live-tested by scale-smoke's screenshot of a one-pixel
checker at 1.25, 1.5, 1.75 and 2). The window's surfaces and its server-side frame (one
anchor: the tree container's origin) tile without gaps. Round is half away from zero,
fractional-scale-v1's rule. *Alternative rejected:* rounding each node independently
(`round(x·s)`, `round(w·s)`) — adjacent nodes then overlap or gap by a pixel.

**D3 — Output scale from configuration, n/120.** `--scale` (`config::output_scale`) for
every output, rounded to the nearest n/120 (niri's `closest_representable_scale`): a
client told 150 renders exactly what the compositor projects. The dead placeholder loop is
gone. Runtime changes (an output-management protocol, a shell setting) are a follow-up; the
generation/notify path already handles a change (full damage, membership, preferred scales).

**D4 — wl_surface conformance.** `invalid_scale`, `invalid_transform`, `invalid_offset`
(v5+ attach with an offset) and `invalid_size` (buffer not divisible by the scale without a
viewport source; tolerated with a debug line for cursors and role-less surfaces, as wlroots
does, wayland#194). `damage` and `damage_buffer` are kept apart and each converted into the
other's space when applied (wlroots' `surface_update_damage` semantics), so shm uploads stay
partial for scaled, cropped and rotated clients.

**D5 — Viewport errors at commit.** `bad_size` and `out_of_buffer` are checked on the state
a `wl_surface.commit` produces (pending over cached over current), before any state moves,
exactly as wlroots does at client commit. The values are fixed at that point, so this is the
spec's "when applied" for every client that does not rely on a synchronized parent changing
its buffer underneath. `wl_fixed` sentinels and the integer test compare raw 24.8 values.

**D6 — Sampling.** The source box is a float rect plus the buffer transform, handed to
every backend as an affine basis (origin, +x, +y in the box's normalized coordinates) from
one shared table (`gfx::source_basis_for`), checked against the spec's own description of
the eight transforms in the GL and CPU tests. GL UVs are `highp` (mediump cannot address
texels past ~2048 on fp16 hardware). Filtering stays linear: a 1:1 draw on whole pixels
samples texel centres exactly; a resampled draw is filtered like wlroots and KWin.
Resampled content widens its damage by half a source texel in output pixels. The CPU
backend keeps its row-copy fast path for identity draws and inverse-maps the rest (nearest
for integer magnification, bilinear otherwise); Vulkan takes the same basis in its push
constants (compile-tested; no Vulkan ICD on the development host).

**D7 — Preferred scales.** `wp_fractional_scale_v1.preferred_scale` = the largest scale of
the outputs a surface is on, in 120ths; `wl_surface.preferred_buffer_scale` = its ceiling
(wlroots). The first fractional value is sent when the object is created, from the output
the surface will map on (its outputs, its parent's, else output 0), so the first frame is
drawn at the right scale instead of at 1 and then again (GTK would otherwise do exactly
that).

**Compositor-drawn content** follows the scale: xcursor themes are loaded at
`round(XCURSOR_SIZE·s)`; client cursor images are resampled once per image change to their
logical size in output pixels (hotspots scaled); server-side titles are shaped at
`10 pt·s` and drawn 1:1; lumen-shell's panel and launcher use the same two protocols as any
client and render through `shm_canvas` at the preferred scale.

**Exposure (X1).** Both globals are public, as everywhere.

## 2b. Decisions (Part 2: presentation-time)

Prior art: wlroots `types/wlr_presentation_time.c` (feedback sampled into an output commit,
`presented = false` → discarded) and `backend/drm/drm.c handle_page_flip` (flags, the
kernel's sequence); niri (`FRAME_CALLBACK_THROTTLE = 995 ms`); the kernel's
`drm_crtc_send_vblank_event` (what a driver without vblank support reports).

**P1 — "Presented" means sampled into a frame that reached the screen.** A content update is
*sampled* in published frame N when its surface's draw in snapshot N is on screen: emitted,
on the output and not covered by an opaque draw. It is *presented* by the first content flip
of a frame F ≥ N that shows the surface; if a newer update of the surface was already in F
(applied at frame M ≤ F) it was never seen and is *discarded*. "Shows" is checked in F
itself: render composites only the newest snapshot, so N may never have been composited and
F may cover the surface or move it off the output. Every publish after N records the
surface's visibility in a 64-frame bitmask per slot; a flip of a frame that hides the surface
leaves the feedback waiting (wlroots likewise keeps feedback a commit did not render). An update replaced before it was ever sampled is
discarded at once, as is one replaced in a synchronized subsurface's cache, and everything a
surface had in flight when it is unmapped or destroyed. The frame index is the key: render
tags each KMS commit with the frame it composited (`atomic_commit_request::user_tag`), the
flip event echoes it, so frames composited but never committed (newest wins) need no special
case. A covered (occluded) update is *not* discarded — occlusion is not in the spec's list;
it waits for the surface to be uncovered or the update superseded. The table is a fixed 1024
slots (16 per update) walked through two intrusive in-flight lists; nothing allocates.

**P2 — Flags, MSC and refresh from the kernel, honestly.** A kernel flip event is `vsync |
hw_completion`; `hw_clock` and the CRTC's vblank counter as MSC are added only when the
driver timestamps vblanks — probed once with `drmCrtcGetSequence` after the first modeset.
A driver without vblank support (vmwgfx here) sends sequence 0 and a time sampled when it
sent the event (`drm_crtc_send_vblank_event` without vblank: `ktime_get()`), which the spec's
`hw_clock` excludes ("sampling a clock in software is not acceptable"); its MSC stays 0 as
the spec asks for outputs without a retrace counter. wlroots reports `hw_clock` regardless;
Lumen is stricter. A blocking modeset's completion is timed in software: `vsync` only.
Refresh is the mode's period; `zero_copy` is never set (no direct scanout). With no display
at all (headless, nested), a published frame counts as presented at the tick, with no flags
(wlroots' headless backend). `kms_atomic_commit::emit_flip` no longer invents a counter.

**P3 — One visibility pass, three users.** `scene::mark_occlusion` runs once per published
frame on the draws in paint order (top-down, up to the eight largest opaque rects) and marks
covered or off-output draws `occluded`; every backend skips them (identical pixels). Each
on-screen surface node records the frame (O(1) query). Presentation samples by it; frame
callbacks of surfaces not on screen are throttled to one per 995 ms (niri) and do not keep
the frame clock running (the housekeeping timer answers them); Parts 3 and 4 use it for
idle inhibitors and for hiding windows under the lock. Eight rects are conservative: an
overlap they cannot express counts as visible (an inhibitor honoured, callbacks unthrottled).

**Stopped display.** At the pause edge (VT switched away) everything in flight is discarded
(wlroots reports such flips `presented = false`); while stopped, each 1 Hz paused tick
discards what was queued, so clients pacing by feedback (mpv) fall back to their own timer.
Nothing is sampled while stopped.

**Exposure (X1).** `wp_presentation` is public, as everywhere.

## 2c. Decisions (Part 3: idle, inhibition, display power)

Prior art: wlroots `types/wlr_idle_notify_v1.c`, `types/wlr_output_power_management_v1.c`;
sway `desktop/idle_inhibit_v1.c` (visibility rule); niri (`activate_monitors`,
`should_activate_monitors`); KWin `DpmsInputEventFilter`; GNOME gsd-power → Mutter
`PowerSaveMode`, KDE PowerDevil → KWin DPMS; the kernel's atomic `ACTIVE` property.

**I1 — Mechanism in the compositor, policy in a client, public protocols between them.**
The compositor tracks activity and inhibitors, serves `ext_idle_notifier_v1` v2 and
`zwlr_output_power_manager_v1`, powers outputs off and on and wakes them on input. It has no
timeouts of its own (as sway, niri and Weston without `idle-time`). lumen-shell is the
default policy — `--blank-after 600`, through the very same public protocols — and
`--idle-policy=none` hands the job to swayidle/hypridle (`swayidle -w timeout 600 'wlopm
--off "*"'`). A compositor setting would fight an external daemon; a private zlm request
would duplicate what the public protocols already say; GNOME and KDE keep timeouts out of
the compositor too. AGENTS §3.2: the shell decides intent, the compositor executes.

**I2 — Activity is real input.** libinput key presses and releases, motion, buttons,
scrolling and touch, noted before any binding or routing (a consumed key still counts), and
the VT resume edge. Never key repeat, synthesised motion, a release made by a device going
away, or (Part 5) a virtual keyboard: no client can keep the session awake by typing for the
user. One timer serves every notification and is armed lazily (activity never re-arms it
while nothing is idle) — one wakeup per timeout period at most.

**I3 — Inhibitors count only while visible, and never resume.** Honoured while the
inhibiting surface is on screen in the newest published frame (the visibility pass: not
covered, on the output, on a running display), evaluated after each publish and only while
one exists. Becoming inhibited does not resume an idle notification — the protocol: "isn't
honored if the system was already idled" (wlroots resumes; Lumen follows the text). When
inhibition ends, obeying timers restart from that moment (wlroots and KWin). v2's
`get_input_idle_notification` ignores inhibitors.

**I4 — Display power is the atomic DPMS off, executed by render.** `ACTIVE=0` with
`MODE_ID` and routing kept, the cursor plane off: one blocking `ALLOW_MODESET` commit by the
render thread (the only committer) once no flip is queued. The resources stay reserved, so
power-on cannot fail on them; blocking needs no event (drivers differ on events for a
disabled CRTC). Refused, it is retried with the primary plane detached; refused again, a
black frame is committed instead (soft blank, logged). From the off edge the display is
*stopped* exactly as at a VT pause — the display gate generalises the pause gate into bits
(`gate_vt | gate_power`), so render, the frame clock (1 Hz paused tick, frame callbacks
still answered), `suspended` toplevels and presentation discards are one machinery. Power-on
is the resume procedure: full repaint, full modeset with `ACTIVE=1`. Every resume takes its
render generation from one UI counter, so VT and power resumes cannot be confused.

**I5 — Input wakes, and the waking press is consumed.** Real input wakes a dark output
inside the compositor, before bindings (KWin, niri); the waking key press or button press —
and its release — reach no client, so Enter that wakes the screen does not also confirm a
dialog. Compositor bindings still act (VT switch, logout); motion and scroll are delivered.
A VT resume is activity and powers the output on with its own modeset; a power request while
the VT is away only changes the target and what clients are told.

**Exposure (X1).** All three globals are public to session-UID clients (sway, niri); with
sandboxing, idle notification and output power will be filtered for sandboxed clients.

## 2d. Decisions (Part 4: session lock)

Prior art: wlroots `types/wlr_session_lock_v1.c` (every error, `invalid_serial`); sway
`lock.c` (abandoned lock on red, replaced by a new locker — but `locked` sent at once); niri
(wait ≤ 1 s for lock surfaces, `locked` after every output presented a locked frame, a dead
lock replaced, `update_locked_hint`); GNOME and KDE (logind `Lock`/`Unlock`, a shorter
blank timeout on the lock screen, lock before blanking).

**L1 — Input leaves at the request; `locked` only after a locked frame.** A `lock` from an
unlocked session takes pointer and keyboard focus from normal clients at once (grabs end as
on Escape, a drag is cancelled, popups get `popup_done`, held buttons' releases go nowhere,
shell grabs and taps stop) while their content stays drawn for at most 1 s: when every live
output has a lock surface with content, or at the deadline, the scene locks (every layer
below the lock layer hidden, a solid clear colour, toplevels `suspended`) and the next
snapshot's index is remembered. `locked` is sent from the flip handler once a content flip of
that frame or a later one completed (from the publish without KMS) — or at once when nothing
of the session can be on screen (VT away, powered off or soft-blanked, nothing ever
committed). That is the spec's hard requirement; sway's immediate `locked` violates it.
Hit-testing then sees lock surfaces only and the keyboard goes to the lock surface that first
got content, then to the last one clicked. A lock that starts while the VT is away replaces
the keyboard focus the VT resume restores (the router compares the window manager's decision
with the focus it will deliver, not with the null focus of a suspended keyboard, and the
resume re-resolves first): keys typed after switching back never reach the window that had
them before the switch. Activation cannot focus a window behind the lock
(it is marked demands-attention), and a window mapping meanwhile opens marked.

**L2 — A dead locker leaves the session locked; recovery is another locker or logind.** The
lock object destroyed with its client — after `locked`, and also before it (fail closed: the
lock was asked for) — leaves `abandoned`: dark red where the lock surfaces were, `warn` once,
the shell told. A new `lock` then takes over and is `locked` at once (the solid colour is
what is on screen). logind's `Unlock` (`loginctl unlock-session`, polkit-gated) unlocks from
any state and sends `finished` to a live locker (whose `destroy` is then allowed). Only an
explicit `destroy` before `locked` — the locker gave up — returns to unlocked. A second
locker while one holds the lock gets `finished` at once.

**L3 — The compositor guarantees, the shell reacts (zlm_shell_v1 v5).** `lock_state`
(unlocked, locking, locked, abandoned; sent on bind and on change) and `lock_requested`
(logind `Lock`, `PrepareForSleep(true)`). No shell surface is shown or reachable while
locked and key grabs are not delivered, by construction. lumen-shell's policy: a
`--lock-command` (none by default — Lumen ships no locker) started by Super+L,
`lock_requested`, and the blank timeout (lock, then blank once `locked`, 5 s at most), started
again when `abandoned` (at most once per 5 s, three times); `--blank-locked-after 60`.
Before sleep the compositor holds a logind delay inhibitor until `locked` or 2 s; logind's
`LockedHint` follows `locked`/unlock; resuming from sleep is activity.

**Exposure (X1).** `ext_session_lock_manager_v1` is public to session-UID clients (sway,
niri): swaylock and friends are ordinary clients. Safety does not rest on trusting the
locker (L2). With sandboxing, sandboxed clients lose it.

## 2e. Decisions (Part 5: text input and input methods)

Prior art: wlroots `types/wlr_text_input_v3.c`, `wlr_input_method_v2.c` (a stale commit
serial resets pending; `unavailable` for a second input method), `wlr_virtual_keyboard_v1.c`
(`invalid_keymap_format`, `no_keymap`); sway `input/text_input.c` (text-input focus only
while an input method exists, popup below the cursor, flipped above / right-aligned when
short) and `input/keyboard.c` (the grab skips the input method's own virtual keyboard);
fcitx5 `waylandimserverv2.cpp` (grab on activate, released on deactivate; the grab keymap
forwarded verbatim to its virtual keyboard; a no-op commit after every done).

**T1 — input-method-v2 + virtual-keyboard-v1, text-input-v3 v2.** What fcitx5 5.1 and ibus
1.5 speak to wlroots-family compositors, from wlroots' `protocol/` (tag 0.20.2, a new
`[wlroots]` manifest source). The experimental `xx-input-method` is not generated: nothing
ships that speaks it. text-input-v3 is advertised at v2 (GTK 4.24 binds it), with v2's
two-stage cursor rectangle; `action`, `language` and `preedit_hint` are never sent (an
input-method-v2 client cannot produce them), `show/hide_input_panel` are ignored hints.

**T2 — keys reach the grab only while the input method is active.** Mutter and KWin route keys
to the input method only for focused input contexts; wlroots sends every key to any grab.
Lumen routes physical keys to the grab only while a focused, enabled text input makes the
input method active — after the compositor's bindings, the window manager's keys and shell
grabs — and a release always follows its press. The input method's own virtual keyboard
never reaches the grab, compositor bindings or idle activity.

**Text-input focus only with an input method (sway).** An entered text input tells a toolkit
an input method is present; GTK and Qt then route keys through it and lose their own compose
handling. So text inputs are entered only while an input method is bound, and left when it
goes (which also clears their preedit).

**Popups at the cursor through `:positioner`.** Below the cursor rectangle, flipped above and
right-aligned when it does not fit the output (sway's rule), slid as the last resort; a first
v2 rectangle still staged keeps the popup hidden until the surface commit applies it.

**Exposure (X1).** The input method and virtual keyboard managers are public to session-UID
clients (sway, niri); sandboxed clients lose them when `wp_security_context_v1` exists.

## 3. Consequences

- New partition `lx.compositor:viewport` (pure geometry and checks), `lx.scene`
  `output_projection` / `project_node` / `project_damage` / `emit_context`,
  `lx::rect2f`, `gfx::source_basis_for` / `effective_source`, `draw_command::scaled` and
  `occluded` (the latter is used by Part 2's visibility pass).
- `scene_node::emit_draws` takes an `emit_context`; tree containers are anchors.
- `output_state` has `position` (logical) and `mode` (pixels) instead of `geometry`.
- Part 2: new partition `lx.compositor:presentation_feedback` (pure), `scene::mark_occlusion`
  and `surface_node::visible_in`, `page_flip_event` carries `tag`, `content`, `hw`,
  `vblank_timestamp`, the kernel `sequence` and the CRTC; `fire_visible_frame_callbacks`
  replaces "answer every callback at the flip" outside the paused tick.
- Part 3: new partitions `lx.compositor:idle` and `:display_power` (pure),
  `lx.shell:idle_policy` (pure), `kms_atomic_commit::commit_power_off`,
  `kms_cursor_plane::append_off_props`; `display_paused_` became `display_gate_` (bits);
  `window_manager::set_session_suspended` became `set_suspend_reason(vt|power|lock)`, plus
  `swallow_button_press` for the waking press. No per-frame cost; per input event one clock
  read and a store.
- Part 4: new partition `lx.compositor:session_lock` (pure; its bodies in `session_lock.cpp`
  — see the build note in architecture.md "Session lock"), `wire_session_lock.cpp`, the
  scene's lock layer and a clear colour per snapshot (`immutable_frame_snapshot::
  clear_color`), `window_manager::set_session_locked` / `wm_lock_ops`,
  `keybindings::set_locked`, `activation_reason::session_locked`, `root_kind::lock`,
  `surface_role::lock`; `logind_session` gains Lock/Unlock/PrepareForSleep callbacks,
  `set_locked_hint`, `take_sleep_inhibitor` / `release_sleep_inhibitor`; zlm_shell_v1 is
  version 5; `protocol_managers` lost its session-lock placeholder. No per-frame cost while
  unlocked (one state check in the flip handler); while locked, hidden clients' commits
  still damage (their callbacks are throttled to 1 Hz, so a well-behaved client repaints a
  region of the lock screen at most once a second).
- Part 5: new partition `lx.compositor:text_input` (pure; bodies in `text_input.cpp`, the
  session-lock build rule), `wire_text_input.cpp`, `wire_input_method.cpp`,
  `ime_popup_layer_` above the lock layer, `surface_role::input_popup` /
  `root_kind::input_popup`, `hit_owner::ime_popup` and `wm_ime_ops`, the input router's
  `im_grab_ops` / `set_im_routing` / `send_virtual_key` / focus-surface hook,
  `lx::input::normalize_keymap`; `protocol_managers` lost its text-input placeholder. One
  branch per key while no input method is active; no per-frame cost.
- Debug aid: `SIGUSR1` makes lumen-compositor write the next fully repainted frame as a PPM
  (`$LUMEN_SCREENSHOT_DIR`), which scale-smoke and app-smoke group S judge pixels from.
- **Trade-offs recorded.** A subsurface at a fractional offset inside a window rounds its
  edges relative to the root and may be one pixel wider or narrower than its buffer (then
  resampled) — the wlroots behaviour. Mipmapped downscaling of a scale-2 client on a 1.25
  output is not done (linear only, as wlroots and KWin). The atlas glyphs of server-side
  frames (close button) are still drawn from a 1x atlas. Cached client cursor images are not
  re-resampled when the output scale changes at runtime (no runtime change exists yet).
