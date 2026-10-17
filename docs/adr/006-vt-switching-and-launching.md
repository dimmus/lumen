> **Status:** Accepted. Part A (VT switching, session pause/resume, key bindings,
> zlm_shell_v1 v4 key grabs) and Part B (the launcher and detached app launching)
> implemented.

# ADR-006: VT switching and launching applications

**Date:** 2026-10-16
**Scope:** `lx.session`, `lx.input`, `lx.drm`, `lx.compositor` (`:session_state`,
`:keybindings`, input router, window manager, the layer-shell focus-on-map rule),
`zlm_shell_v1` v4, `lx.shell` (`:desktop_entry`, `:app_catalog`, `:app_launch`,
`:shm_canvas`, `:launcher`)
**Deciders:** project owner

---

## 1. Context

Once Lumen owned the console, logging out was the only way back to a text console: nothing
listened to logind's PauseDevice / ResumeDevice signals or called `Seat.SwitchTo`, and a VT
switch made by other means (`chvt`, another session ending) left the compositor committing
into a CRTC it no longer owned. New windows could only come from a terminal, so an
application started inside foot died with foot. Both need the compositor to own key
bindings it sees first, while which key does what remains the shell's decision.

Prior art: logind's `logind-session-device.c` and `logind-session.c`, libseat's logind
backend, wlroots (`handle_session_active`, MR 3967 and 3794), Mutter
(`meta_backend_native_pause`/`resume`, the Clutter "flush the queue after libinput_suspend"
fix), KWin (`Session::activeChanged`, modifier-only shortcuts), niri (995 ms frame-callback
throttle), sway (`layer_shell.c` focus on map), systemd.io/DESKTOP_ENVIRONMENTS, KDE KIO's
`SystemdProcessRunner`.

## 2. Decisions

**A1 — When is the session paused?** On the first of: PauseDevice for the DRM card (any
type), or `Session.Active` turning false. On a VT seat logind has already dropped master
when the signal arrives (`session_leave_vt` → `session_device_pause_all`, type `force`),
and it revokes evdev fds before it signals; stopping early costs nothing.

**A2 — When is it resumed?** On ResumeDevice for the card; as a fallback when `Active` is
true and `drmIsMaster` holds (at the signal, or at the 1 Hz paused tick). Never on `Active`
alone: logind restores master (`drmSetMaster`) before it sends ResumeDevice, but `Active`
and ResumeDevice are not ordered, so a modeset on `Active` alone can fail with EACCES.
*Rejected:* libseat's resume on `Active` — fine for a compositor that commits later, not for
one that modesets at once.

**A3 — PauseDeviceComplete.** Only for type `pause` (seats without VTs, where logind waits
for it), only for the device named, and only after the render thread confirms it stopped
committing (one UI → render → UI round trip). Never for `force` or `gone`. The old
`resume_devices()` stub that sent `PauseDeviceComplete(0, 0)` was wrong and is gone. Every
device libinput closes goes back with ReleaseDevice, or the TakeDevice of a resume fails.

**A5 — Clients while paused** stay alive and running: buffers are applied and released,
frame callbacks are answered once a second, every toplevel gets `suspended` (xdg_wm_base
v6), the focused client gets `wl_keyboard.leave`. *Rejected:* withholding callbacks
(wlroots, sway) — cheaper, but a client blocking in `eglSwapBuffers` wedges and its pings
time out. The 1 Hz cost is niri's choice for the same reason.

**A6 — Resume restores a known-good KMS state:** one nonblocking `ALLOW_MODESET` commit
with connector → CRTC, `MODE_ID`, `ACTIVE`, the primary plane with its in-fence, the cursor plane in full and any other plane
left on the CRTC disabled (the last master's state is undefined). Every property another
master can leave set and Lumen does not manage goes back to the driver's initial value where
the object has it and it is mutable: CRTC gamma/degamma LUT, CTM, VRR; connector
link-status GOOD, Colorspace Default, no HDR_OUTPUT_METADATA, max bpc at the top of its
range (the kernel's initial value), Broadcast RGB Automatic; on the primary and cursor
planes rotate-0, opaque alpha, Pre-multiplied blending, and zpos at the bottom (primary) or
top (cursor) of its range — so KWin's HDR signalling or Mutter's rotation on another VT does
not show Lumen's SDR frame as PQ or upside down. These ride last in the request; a driver
that refuses them (EINVAL) gets the modeset once more without them. Flip events carry a
generation cookie so a pre-pause flip cannot clear the resume's flag. A full property
snapshot and restore (wlroots MR 3794) is deferred.

**A8 — Key bindings.** A Wayland-free `:keybindings` decides every key: the compositor's
own first (`XF86Switch_VT_n` → `SwitchTo`, Ctrl+Alt+BackSpace → logout; keysyms, so
`srvrkeys:none` disables VT switching), then the window manager's (Escape in a grab), then
**shell key grabs** registered over zlm_shell_v1 v4 (`grab_key` →
`zlm_key_grab_v1.activated(serial, time)`), then tap observation, then the client. The
router's forwarded-key rule (a release only for a forwarded press; `enter` lists only
forwarded keys) swallows every consumed key's release. A tap grab (Super alone) observes and
does not take the key, as KWin's modifier-only shortcuts do. A release libinput makes
because its device is going away is marked and never read as a tap, a window drop or a drag
drop. *Rejected:* a fixed "launcher key pressed" event — it puts policy (which key) in the
compositor, does not extend to workspaces or screenshots, and does not die with the shell;
the grab model is Mutter's GrabAccelerators / KWin's KGlobalAccel.

**B1 — The launcher** is a separate overlay layer surface with
`keyboard_interactivity = on_demand`, taking the keyboard on map (sway's rule), closing on
Escape, toggle, launch or keyboard leave. The rule is sway's and Hyprland's: an `on_demand`
surface on the top or overlay layer takes the keyboard as it maps (not during a move,
resize or drag, and not from a focused surface on a higher layer); before this change an
`on_demand` surface waited for a click. *Rejected:* an `xdg_popup` off the panel (the
panel's interactivity is `none`, so it cannot take the keyboard), an exclusive surface (it
would steal the keyboard from everything until closed, so no outside-click dismissal), and
a fullscreen transparent overlay (a full-output buffer blended every frame to show a
420×512 menu).

**B2 — Detaching launched apps:** a systemd transient *service* per launch
(`app-lumen-<id>@<random>.service` via `StartTransientUnit`, `Type=exec`, `ExitType=cgroup`,
`Slice=app.slice`), so systemd spawns the process and nothing is inherited from the shell;
double fork + `setsid` when systemd --user is unavailable. `ExitType=cgroup` is systemd 250's
(`Type=exec` 240's), and a service without it ends — killing its cgroup — when an app's
first process forks and exits; an older manager also refuses the whole call
(`PropertyReadOnly`). So the shell reads `Manager.Version` once, when it opens the user bus
(blocking for at most 1 s, as KIO does), and below 250, or when the version cannot be read,
launches into a transient *scope* instead (KIO's choice below 250): the double fork, its
process held before `execve` on a pipe until `StartTransientUnit` of
`app-lumen-<id>-<random>.scope` with `PIDs=[pid]` has finished (at most 1 s), so — unlike
KIO and GNOME — the app never runs or forks outside its scope. A `PropertyReadOnly` reply
despite the version switches to scopes and retries that launch (nothing was created).
A `NoReply` is *not* "no systemd": the call timed out but systemd may still start the unit,
so the launch is never repeated by fork (it would run twice); `JobRemoved` is awaited for
30 s more before it is reported as failed. Only `ServiceUnknown`, `NameHasNoOwner` and
`Spawn.ChildExited` mean there is no systemd --user. The shell resolves the program
in its own `PATH` and passes the absolute path (systemd's search path is not the user's).
The new window gets an `xdg_activation_v1` token from the triggering input's serial.
*Rejected:* scopes everywhere (GNOME) — systemd.io encourages services, and the scope path
needs the shell's double fork to clean fds, signal mask and environment itself;
`Type=simple` (KDE) — an exec failure would surface only as a later unit state change,
where `Type=exec` fails the start job itself.

## 3. Consequences

- The compositor has a keyboard way out that depends on nothing else (VT switch, logout).
- A resized VM window while away makes the resume modeset fail (`rejected`); the display
  stays off until the next switch back. Runtime mode changes belong to the outputs work.
- Sleep/resume (`PrepareForSleep`), screen locking and seats without VTs beyond the unit-
  tested `pause` acknowledgement are out of scope.
