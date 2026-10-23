> **Status:** Current (v0.3) — see code for implementation truth

# Compositor ↔ Shell State Synchronization

Multi-process reliability depends on the shell receiving a **complete initial snapshot**
on bind, then **delta events** for all state changes.

## Protocol surface

Implemented in `zlm_policy_bridge_v1` (`protocols/lumen/zlm_shell_v1.xml`):

| Phase | Events |
|-------|--------|
| Bind | `zlm_shell_v1.capabilities(flags, shell_protocol_version)`, sent immediately after the bind (flags 0 today: no tiling plugin, workspace drag or explicit sync yet; version 7), then (v5) `zlm_shell_v1.lock_state`, (v6) `capture_state`, (v7) `config` → `config_diagnostic`* → `config_done` |
| Initial sync | `snapshot_begin` → (`snapshot_toplevel` [→ `toplevel_flags`] [→ `toplevel_workspace` (v7)])* → (v6, per toplevel: [`toplevel_icon` → `toplevel_icon_buffer`* → `toplevel_icon_done`] [→ `toplevel_parent`])* → `snapshot_done` → `focus_changed` |
| Runtime deltas | `toplevel_created` (→ `toplevel_state`, `toplevel_geometry` [, `toplevel_flags`] [, icon] [, `toplevel_parent`]), `toplevel_destroyed`, `toplevel_state`, `toplevel_geometry`, `toplevel_flags` (v2), `focus_changed`, `stacking_order_changed`, (v6) the icon group, `toplevel_parent`, `bell`, (v7) `toplevel_workspace` |

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

### Version 4: key grabs

The family moves to version 4 together (`zlm_key_grab_v1` is created at its parent's
version too). `zlm_shell_v1.grab_key(id, keysym, modifiers, flags)` creates a
`zlm_key_grab_v1`: while it exists, a press of the combination reaches no client and is
reported as `activated(serial, time)`; the key's release reaches no client either (the
compositor's forwarded-key rule). Matching: exact modifiers (shift, ctrl, alt, super; Caps
Lock and Num Lock ignored); the keysym at the key's first shift level in the active layout,
or in layout 0 when the active layout has no key producing it (Super+Return still works
under a Cyrillic layout). With the `tap` flag the grab names a bare modifier pressed and
released alone and is *observed*: the key still reaches the client, and `activated` comes on
its release when no other key, button or scroll happened meanwhile (Super+drag is not a tap).
Grabs are not delivered during an interactive move, resize or drag, or while the session is
paused (VT switched away).

A grab is refused — `refused(reason)`, then inert until destroyed — when it names the
compositor's own bindings (`reserved`: Ctrl+Alt+BackSpace, the `XF86Switch_VT_*` keysyms,
Ctrl+Alt+F1…F12), a combination another grab holds (`taken`), or unknown bits or a tap grab
on a non-modifier (`invalid`). The compositor keeps its own bindings regardless, so a dead
or misbehaving shell cannot take the keyboard ways out; a shell's grabs vanish with its
connection.

**Serial semantics.** `activated`'s serial is a fresh seat serial that the compositor records
in its per-client input serial log as a *key press delivered to the shell*: the shell may
quote it to `xdg_activation_token_v1.set_serial`, and the focus-stealing policy grants the
token (the shell's last key press), so an application launched from a key binding takes the
keyboard. It also satisfies `xdg_popup.grab`.

**Startup sequence (v5 shell):** bind `zlm_shell_v1` (min(advertised, 5)) → `capabilities`
(version 5) → `lock_state` → `get_policy_bridge` → snapshot … `focus_changed` → `grab_key`
×N. lumen-shell
registers the Super_L and Super_R taps (toggle the launcher) and Super+Return (start a
terminal; its `activated` serial is the one the launch's activation token quotes). Bound below
version 4, `policy_bridge::grab_key` returns `not_implemented` and logs once at info that the
compositor has no key grabs; the launcher is then reachable from the panel only.

### Version 5: the session lock

The family moves to version 5 together. Two `zlm_shell_v1` events, both Wayland-free on the
compositor side (`:session_lock` decides, `wire_shell.cpp` sends):

- `lock_state(state)` — `unlocked` 0, `locking` 1 (a locker asked: input has left normal
  clients; the scene locks within 1 s), `locked` 2, `abandoned` 3 (locked, and the locker
  died: the session stays locked on dark red until a new locker takes over or logind's
  `Unlock`). Sent on bind (after `capabilities`) and on every change of that value. While it
  is not `unlocked` **no shell surface is shown and none receives input, and key grabs and
  taps are not delivered** — the compositor guarantees that by construction (hidden layers,
  lock-surface-only hit-testing and keyboard focus, `keybindings::set_locked`); the shell
  only reacts.
- `lock_requested(reason)` — `logind` 0 (`loginctl lock-session`, a lid policy: logind's
  `Session.Lock`) or `sleep` 1 (`Manager.PrepareForSleep(true)`; the compositor holds a
  logind sleep delay until `locked` or 2 s). Not sent while already locked. Lumen ships no
  locker: the shell starts its configured one; with none, the compositor logs that nothing
  locks.

lumen-shell binds `min(advertised, 5)` and, below 5, logs once at info that there is no
session-lock integration. Its lock policy (`lx.shell:idle_policy`, pure; `--lock-command`,
`--[no-]lock-on-blank`, `--blank-locked-after`):

- on `locking`/`locked`/`abandoned`: close the launcher, drop pointer and key-repeat state;
- on `abandoned`: start the locker again — at once the first time, then at most once per
  5 s, giving up after the third relaunch (an `error`: `loginctl unlock-session` unlocks);
  the count resets at the next unlock;
- on `lock_requested`, and on Super+L (a v5 key grab registered only with a lock command):
  start the locker when unlocked;
- the blank timeout (`--blank-after`) starts the locker first and powers the outputs off
  once `locked` arrives (5 s at most), so the screen never goes dark unlocked (GNOME's
  order); while locked a second `ext_idle_notification_v1` (`--blank-locked-after`, 60 s)
  blanks the lock screen; a blank request is not repeated while the outputs are off.

The locker is an ordinary client launched like any application (`app-lumen-locker@<random>
.service`, no activation token: lock surfaces take the keyboard as such).

### Version 6: icons, parents, the bell, capture state, non-inhibitable grabs

The family moves to version 6 together (design-app-compat-capture B.8). On the policy bridge:

- `toplevel_icon(toplevel, name)`, `toplevel_icon_buffer(toplevel, fd, size, scale)`*,
  `toplevel_icon_done(toplevel)` — a window's icon (xdg-toplevel-icon-v1) as applied by its
  commit: the XDG icon-theme name ("" for none) and zero or more pixel buffers, each a
  **sealed memfd** of `size`² premultiplied ARGB8888 the shell maps read-only (pixels never
  travel over the wire; the compositor's copy is immutable under the mapping). An empty name
  and no buffers means the default icon. Sent after `snapshot_toplevel` / `toplevel_created`
  when the window has one, and on every change.
- `toplevel_parent(toplevel, parent?)` — the window this one is a dialog or child of
  (`xdg_toplevel.set_parent`, or xdg-foreign across clients); null for none, or when the
  parent is not mapped. In the snapshot after every handle exists (a parent can come later
  in creation order). A child in state `modal` (sent for an effectively modal dialog: modal,
  with a mapped parent) blocks its parent.
- `bell(toplevel?)` — xdg-system-bell-v1 rang for that window or for none; at most 4 a second
  per client, none while the session is locked.

On `zlm_shell_v1`: `capture_state(active)`, sent on bind and on change — 1 while a client
other than the shell is capturing the screen: an ext-image-copy-capture session not owned by
the shell has delivered a frame and is still open, or a wlr-screencopy frame was delivered
within the last 2 s (ADR 009 Part 4). The indicator's own repaint is damage, so a rule of "a
frame in the last 2 s" would make an idle cast blink it and copy a frame every ~2 s. lumen-shell draws a red dot left of its logout button while it is 1. On `zlm_key_grab_v1`: the flag `not_inhibitable` (0x2) — the grab fires also while
the focused window inhibits shortcuts (keyboard-shortcuts-inhibit-unstable-v1); all other
grabs and taps are skipped then and their keys reach that window.

lumen-shell binds `min(advertised, 6)`. Its task model keeps each window's icon name and
parent (the bridge keeps the mapped memfds per task slot and unmaps them with the task); the
panel draws a 16 px icon left of the title — the name resolved in the XDG icon theme
(`lx.shell:icon_theme`: the GTK theme setting, its Inherits chain, hicolor, then the
unthemed directories; PNG via libpng, SVG via librsvg, each optional; cached per name and
pixel size), else the window's own buffer nearest the size (resampled once per icon
revision), else the app id as an icon name, else its desktop entry's `Icon=`, else a letter
tile — and does not list a modal dialog beside the window it blocks (that window's entry
activates the dialog, as the compositor redirects focus to it). The bell flashes the window
(`lx.shell:visual_bell`: an overlay layer surface over its rect, a 1×1 translucent buffer
scaled by `wp_viewport`, 120 ms, no input region) or, for a bell of no window, the panel strip
of the active output. Super+L (with a lock command) is grabbed `not_inhibitable`.

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

### Version 7: the configuration and actions

The compositor reads, watches and validates `config.toml` (ADR 011 Part 2); the shell is
handed what it read, and the key bindings are the compositor's:

- **`config(fd, size, generation)`** — a sealed, read-only memfd of the accepted bytes: the
  layers concatenated, each after a `# lumen-layer: PATH` line, so a line number maps back to
  its file; size 0 means no file (every default). Sent on bind (before the shell sets
  anything up) and after a reload that accepted different bytes. A rejected reload (a syntax
  error) sends no `config`: the previous one stays in use. The shell parses the bytes with
  `lx.config` — the compositor's parser — and applies `[idle]` and `[background]` live,
  `[switcher]` at the next hold, and its focus-follows-mouse debounce from `[focus]`
  (notifications later); its command-line options override the file. A
  restarted shell starts from the current file.
- **`config_diagnostic(generation, severity, file, line, text)`** then
  **`config_done(generation, applied, errors, warnings)`** — the compositor's report of the
  generation (parse, schema, bindings, apply-time: a keymap that does not compile, a cursor
  theme not found). Sent on bind and after every reload. lumen-shell shows one panel notice
  per generation with problems ("Lumen configuration: 1 error, 0 warnings — config.toml:4:
  input.keyboard.repeat-rate: …") and not again for an identical report; notifications
  (design Part 5) replace the notice.
- **`action(action, argument, serial, time)`** — a configured binding (or `lumen-ctl action`,
  serial 0) named a shell action: `shell.launcher` (the Super tap by default),
  `shell.terminal` (Super+Return), `shell.lock` (Super+L, bound only with
  `idle.lock-command`). The serial is an input serial noted as the shell's key press, as a
  grab's `activated` was. The compositor matches shell bindings only while a v7 shell is
  bound; without one (a restart, an older shell) those keys go on to legacy grabs or the
  client (except `switcher.next`: the compositor's quick switch). Held actions (the
  switcher) come as `held_action` instead — see below.

lumen-shell binds v7 and grabs no keys from a v7 compositor — except Super+L
(`not_inhibitable`) when its lock command comes from the command line and the configuration
has none. Against a v4–v6 compositor it grabs as before. Every interface of the family is at
version 7.

### Version 7: workspaces (ext-workspace-v1 and membership)

Workspaces are the compositor's (ADR 011 Part 3) and are served as the public
**ext-workspace-v1**: one group per output, its workspaces with name, position and state,
`activate` and (dynamic mode) `assign`. lumen-shell binds `ext_workspace_manager_v1` in the
same registry round trip as `zlm_shell_v1`, so before `get_policy_bridge`, and its panel
indicator is a client of it like any bar. zlm adds only what ext-workspace does not carry:

- **`toplevel_workspace(toplevel, workspace)`** — the window's workspace as the shell's own
  `ext_workspace_handle_v1` (looked up per client), null for a sticky window and while the
  shell holds no handle for it. Sent after every `snapshot_toplevel`, after
  `toplevel_created` (once the map's ext-workspace changes are out), and whenever the window
  moves (a binding, a hotplug merge, `assign`). The compositor always sends the
  ext-workspace `done` that introduced a workspace before a `toplevel_workspace` naming it.
  When the shell binds ext-workspace after its bridge, every window's workspace is sent again
  after that manager's first `done`. A handle the shell gets `removed` for is forgotten by
  its task model (no window names it).
- **`toplevel_flags` `sticky` (0x4)** — the window is shown on every workspace of its output.
- **`toplevel_state` `minimized`** — now sent (it wins over the xdg state while minimized).

`request_minimize` minimizes; `request_activate` brings a window back (unminimized, its
workspace switched to). A window is shown iff it is mapped, not minimized, and sticky or on
its output's active workspace; the shell's task list per output shows its active workspace's
windows and sticky ones, minimized ones dimmed. A restarted shell rebuilds all of it from
ext-workspace's bind and the snapshot.

`zlm_shell_v1.get_workspace_manager`, `zlm_workspace_manager_v1`, `zlm_workspace_v1` and
`snapshot_workspace` are deprecated: never implemented, superseded by the above. The
`workspace` argument of `snapshot_toplevel` / `toplevel_created` stays null. Because zlm now
names `ext_workspace_handle_v1`, ext-workspace-v1 is a P0 protocol (the zlm glue links its
interface tables).

### Version 7: held actions (the window switcher)

A binding of a **held** action (`switcher.next`, `switcher.previous`, `-app`; ADR 011 K2) is
not sent as `action` but as **`zlm_shell_v1.held_action(id, action, argument, serial,
time)`**, which creates a `zlm_held_action_v1` — a hold on the keyboard for as long as the
binding's primary modifier (its highest: Super, Alt, Ctrl, Shift) is down:

- **`key(keysym, modifiers, binding, serial, time)`** — every key press during the hold goes
  here instead of the focused client: the first-level keysym (Tab, not ISO_Left_Tab), the
  `zlm_key_grab_v1.modifier` bits, and the action the user's configured binding of that key
  names ("switcher.previous" for Alt+Shift+Tab), so the shell follows the user's keys; repeated
  at the keyboard's rate while held. Modifier presses and the releases of keys pressed earlier
  still reach the client: it keeps the keyboard focus (no leave, no enter) and a correct
  modifier state.
- **`released(serial, time)`** — the modifier is up: the switcher commits its selection
  (`request_activate`). **`cancelled`** — the session locked, a VT switch, an interactive
  move, resize or drag began, or a button was pressed on a window: close without activating. After either the object is inert; the
  shell destroys it.
- **`destroy`** (the shell) — ends the hold at once (Escape, Return, a click on the
  switcher): key presses go to the focused client again although the modifier is still down.

Diversion runs after the compositor's own keys and the window manager's (a keyboard move's)
and before the configured bindings, so a second held binding during a hold is a `key` of the
first. With no v7 shell bound, `switcher.next` is the compositor's own quick switch (the
topmost other window); `lumen-ctl action switcher.next` sends a plain `action`, which the
shell runs as an immediate switch to the window it would select.

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
- `workspace` is **null** in `toplevel_created` and `snapshot_toplevel`: `zlm_workspace_v1`
  is deprecated (a compositor cannot introduce one — no event carries it as `new_id`); the
  window's workspace is v7 `toplevel_workspace` (an ext-workspace handle). No
  `snapshot_workspace` is sent, and no `snapshot_output`
  (its `wl_output` is not nullable and the shell may not have bound one): the shell learns
  the outputs from `wl_output` and `zxdg_output_v1` (one global per output, ADR 008) and
  places each window on the output its geometry overlaps most.

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
  `zlm_shell_v1` (v7, or what the compositor advertises) and, if present, `wp_cursor_shape_manager_v1`; the bridge listens on
  `zlm_shell_v1` in the same dispatch as the bind, because `capabilities` follows at once
  and libwayland drops events for a proxy with no listener. A second roundtrip surfaces a
  refused bind as a protocol error: the shell logs why and exits 1. Then
  `get_policy_bridge`, and the panel's layer surface. `wl_seat` (v5 at most) gets its
  listener in the bind's own dispatch, for the same reason as `zlm_shell_v1`; the shell owns
  the seat's pointer and keyboard and routes them by surface to the panel or the launcher.
  Also bound: **every** `wl_output` (v4, each with a `zxdg_output_v1` v3 for its logical
  rect; outputs advertised later are bound as they come, and a `global_remove` drops that
  output's panel and records) and `xdg_activation_v1` (tokens for launched windows). Each
  output gets its panel at its first `wl_output.done` (after the bridge is up).
- **Loop.** `wl_display_prepare_read` / `dispatch_pending` / `flush` (POLLOUT on EAGAIN),
  then `ppoll` on the display fd, a `signalfd` for SIGINT/SIGTERM, the catalog's inotify
  fd, one `timerfd` (earliest of: key repeat, panel-notice expiry, catalog debounce,
  activation-token timeout, the switcher's open delay — disarmed when none is due) and, after the first launch opened
  it, the user bus (`sd_bus_get_events` / `sd_bus_get_timeout`) plus any fallback-launch
  pipes. An idle shell sleeps. SIGINT/SIGTERM exit 0 after destroying every object; a refused bind, a lost
  compositor exits 1, and `lumen-shell.service` (`Restart=on-failure`, 300 ms, at most 5
  times in 30 s; run-desktop.sh the same in its direct mode) starts it again. The new bridge
  gets the snapshot of every window; until the new panel maps (5 s at most) the compositor
  keeps the dead shell's exclusive zones (the zone hold, ADR 011 S6), and the bind is logged
  `bridge bound client=… snapshot=<n> focus=… pid=… binds=<n since start>`. A `closed` panel (its output went away) is destroyed; the shell runs on.
- **Window switcher** (`lx.shell:switcher`, ADR 011 K1). A hold begins a session over the
  task model's **most-recently-used order** (`task_model::mru`: a snapshot seeds it from the
  stacking order, top first; each `focus_changed` moves that window to the front; under
  `focus.mode = "sloppy"` a focus shorter than 750 ms that moved on does not count, unless the
  switcher chose it) filtered by `[switcher] scope` and `include-minimized` (modal dialogs
  never: the parent stands for them; Alt+grave keeps the focused window's application). The
  selection starts on the second entry (the last for `previous`); `key` steps (the user's
  bindings, Tab/Shift+Tab, arrows, Home/End), Return commits, Escape cancels, `released`
  commits. The view — an overlay layer surface (`lumen-switcher`) centred on the focused
  window's output, no keyboard interactivity, exclusive zone -1, ARGB with rounded corners,
  fractional scale — is created only when the hold has lasted `open-delay` (150 ms: a quick
  Alt+Tab draws nothing) and destroyed with it; it repaints only the cells whose selection
  changed and the title. Hover selects once the pointer has moved; a click commits.
- **Task model** (`lx.shell:task_model`, pure). A fixed table of 256 entries — app_id, title
  (cut at 255 bytes on a UTF-8 code point boundary), state, geometry, stacking — addressed
  by a `task_handle {index, gen}` whose generation never repeats, so a handle kept across a
  removal cannot name the entry that reuses its slot. Each `zlm_toplevel_v1` proxy's user
  data is its slot. Entries keep creation order, the conventional task-bar order, not
  stacking order: the compositor lists the snapshot in creation order (each with its
  `stacking_index`), so a restarted shell shows the order it had. While a snapshot is in progress the
  model reports `in_snapshot()` and the panel keeps what it last drew, so half a snapshot is
  never shown. On `toplevel_destroyed` the shell removes the entry and destroys its proxy.
- **Panel** (`lx.shell:panel`), **one per output** (waybar's default; ADR 008 M13). A layer
  surface on `top` of its `wl_output`, anchored top/left/right, 32 px, exclusive zone 32,
  keyboard interactivity `none`, namespace `lumen-panel`. Each lists the windows on its
  output (`task_output`: the output their geometry overlaps most, Plasma's "tasks from the
  current screen"); pointer input is routed to the panel under it. One entry per
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
- **Apps button and notices.** An "Apps" button at the left end (64 px) toggles the
  launcher and stays latched while it is open. A launch that fails while the launcher is
  closed (Super+Return without a terminal, a start job that failed) shows "Could not start
  <Name>: <reason>" in place of the task entries for 6 s or until clicked.
- **Launcher** (`lx.shell:launcher`). On the focused window's output, else the output of
  the panel last pointed at, else the first. A separate layer surface on `overlay`, anchored
  top/left with 4 px margins and exclusive zone 0 (so it sits under the panel), keyboard
  interactivity `on_demand`, namespace `lumen-launcher`; 420 px wide (less on a narrow
  output), a 40 px search row, 4–14 rows of 32 px, a 24 px status row. It exists only while
  open (its surface, pool and buffers are created on open and freed on close). The
  compositor gives it the keyboard on map (an `on_demand` top/overlay surface does); it
  closes on Escape with an empty query, on its toggle, on launch, and on
  `wl_keyboard.leave` — a click on a window or on nothing takes the keyboard back, so an
  outside click dismisses it with standard protocol only. Typing filters (Name prefix, a
  word of Name, Name substring, GenericName/Keywords, the Exec program), Up/Down,
  Tab/Shift+Tab, Page Up/Down, Home/End move, Return launches, BackSpace / Ctrl+BackSpace /
  Ctrl+W / Ctrl+U edit; keys repeat per `repeat_info`. A press and release on a row
  launches it; the wheel scrolls three rows a notch. Layout, diff and model are pure, as the
  panel's.
- **Catalog and launching** (`lx.shell:desktop_entry`, `:app_catalog`, `:app_launch`). The
  catalog is read once after the panel's first commit, then inotify only marks it stale;
  it is rescanned when the launcher opens (or, while open, 250 ms after a burst of
  changes). A launch is checked first (the program in `PATH`, a terminal for
  `Terminal=true`) — a failure shows in the launcher's status row and it stays open — then
  an `xdg_activation_v1` token is requested with the triggering input's serial (the Return
  key press, the row's button press, or the grab's `activated`), the launcher closes, and
  on `done` (or after 100 ms without one) the app starts as its own systemd user service
  (`StartTransientUnit`, `app-lumen-<id>@<random>.service`, `Type=exec`, `ExitType=cgroup`,
  `Slice=app.slice`, `CollectMode=inactive-or-failed`, the launch environment, the absolute
  program path); below systemd 250 (`Manager.Version`, read once) as a double fork held
  before `execve` until it is in its transient scope `app-lumen-<id>-<random>.scope`; or,
  without systemd --user, by double fork + `setsid`. A `NoReply` is never retried by fork
  (systemd may still start the unit).
- **Input.** A left press and release on the same entry sends `request_raise` then
  `request_activate` (activate raises too; raise is sent so the intent stays explicit if
  focus and stacking policy ever separate). The pointer shape is `default` through
  `wp_cursor_shape_v1` on enter.
- **Notifications and the tray** (`lx.shell:user_bus`, `:notifications`, `:notify_view`,
  `:menu_popup`, `:tray`; ADR 011 Part 5). Not part of the zlm protocol: the shell is an
  ordinary D-Bus service (`org.freedesktop.Notifications`, `org.kde.StatusNotifierWatcher`)
  and an ordinary layer-shell client for its popups (`lumen-notifications`, on the output of
  the task model's focused window), with xdg_popups of the panel for menus and tooltips (it
  binds `xdg_wm_base` for them). What it uses from the bridge: `lock_state` (nothing pops
  up while locked; notifications wait for the unlock), `capture_state` (do-not-disturb while
  the screen is captured), the configuration (`[notifications]`, applied live) and the
  diagnostics (one notification per generation with problems). A restarted shell (S6)
  continues the notification ids after the persisted block
  (`$XDG_RUNTIME_DIR/lumen/shell-state`, also do-not-disturb) and gets the tray back as items
  register again when the watcher's name changes owner; notifications that were on screen
  when it died are lost.
- **Logs** (`LUMEN_LOG` as for the compositor): `shell` (startup, exit), `shell.bridge`
  (`capabilities`, `snapshot toplevels=<n>` at info; `snapshot toplevel`, `add`, `remove`,
  `title`, `state`, `focus handle=<slot|none>`, `flags handle=<slot> flags=<bits>` at debug), `shell.panel` (`configure` at info;
  `layout idx=<i> x=<x> w=<w> handle=<slot> title="…"` on every re-layout, `draw entries=<n>
  hidden=<k> focused=<i> damage=<rects>[(full)] repaint=<rects> buffer=<b>` and
  `click idx=<i> handle=<slot>` at debug), `shell.launcher` (`launcher: open (…)` and
  `launcher: close` at info; `launcher: close (<why>)` and `launcher: draw …` at debug),
  `shell.apps` (`apps: catalog <n> entries, <m> listed (…) in <ms> ms; terminal <source>`
  and `apps: launched <id> as <unit>` at info; failures at warn, once per desktop ID and
  reason per catalog generation), `shell.lock` (`lock command: …`, `lock: starting <path>
  (<why>)` at info; giving up on a locker that keeps exiting at error), `shell.bridge`
  (`session lock <state>`, `lock requested (…)` at info).

## Window rules

Rules load from `$XDG_CONFIG_HOME/lumen/window-rules.toml` via `reload_rules`.
Typed setters replace stringly-typed key/value pairs.
