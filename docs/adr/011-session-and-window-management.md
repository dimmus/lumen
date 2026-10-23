# ADR 011 — The login session and everyday window management

**Status:** Accepted — implemented: Part 1 (the session), Part 2 (the configuration file
and key bindings), Part 3 (workspaces, minimize, sticky), Part 4 (switching and snapping)
and Part 5 (notifications and the tray).

**Context.** Until now Lumen started only through `scripts/run-desktop.sh`, which ran the
compositor, the shell and a client as its children and set up the portals per run. The
unit files under `deploy/systemd/` were never configured (`%bindir%` is not a systemd
specifier), the socket unit described socket activation the compositor does not do, nothing
told systemd when the compositor was ready, a shell that crashed outside a unit stayed dead,
and `lx.health` existed only in `docs/subsystems/health.md`. A display manager could not
start Lumen and a console login had no launcher.

Prior art read for this decision: niri (`niri-session`, `niri.service`,
`niri-shutdown.target`), systemd.special(7) and systemd.io/DESKTOP_ENVIRONMENTS, GNOME
(gnome-session, `org.gnome.Shell@wayland.service`), KDE Plasma 6 (`startplasma-wayland`,
`plasma-plasmashell.service`), uwsm, Mutter's and libseat's logind session discovery, and
the control sockets of sway, Hyprland and niri.

---

## Part 1 — the login session

### S1. `lumen-session` starts the session

A bash launcher in `bindir` (`scripts/lumen-session.in`, configured at install time). A
console login runs it from the shell; a display manager runs it from
`share/wayland-sessions/lumen.desktop` (`Exec=lumen-session`). It re-executes once through
the login shell (`$SHELL -l`, niri-session's rule) so a display-manager start sees
`~/.profile` as a console login does; refuses a nested start (a live `WAYLAND_DISPLAY`), a
second graphical session (`graphical-session.target` active) and a console session that is
not in front; stops a portal frontend or backend left running with an old environment;
imports the login's environment into the user manager (minus a denylist) and into D-Bus
activation, **recording each variable's previous value** in
`$XDG_RUNTIME_DIR/lumen/session-env`; runs `systemctl --user --wait start
lumen-compositor.service`; then starts `lumen-shutdown.target` with
`--job-mode=replace-irreversibly` and puts the manager's environment back **exactly**
(values that were set are set again, variables that were absent are unset). The record
survives a crash of the script itself: the next start, or `lumen-session --cleanup`,
restores it. A value `systemctl show-environment` had to escape (`$'…'`) cannot be read back
exactly, so such a variable is left alone instead of being imported over.

Without a systemd user manager (a container, a distribution without systemd) it runs the
compositor and the shell itself, restarts a failing shell (at most 5 times in 30 s, as the
unit does) and starts the XDG autostart entries for `Lumen` (Hidden, OnlyShowIn/NotShowIn,
TryExec, X-GNOME-Autostart-enabled); the session ends when the compositor exits.

*Rejected:* a compiled session leader (gnome-session's model: nothing it would do needs more
than systemctl); uwsm's generic wrappers (heavier, and the compositor exports its own
environment, S4); `DesktopNames=` in the session entry (not a Desktop Entry key —
`desktop-file-validate` rejects it; lumen-session supplies `XDG_CURRENT_DESKTOP=Lumen` and
`XDG_SESSION_DESKTOP=lumen` when the display manager has not).

### S2. The unit graph

```
lumen-compositor.service  Type=notify-reload, BindsTo= + Before=graphical-session.target,
                          Wants= + After=graphical-session-pre.target,
                          Wants=lumen-shell.service xdg-desktop-autostart.target,
                          Before=xdg-desktop-autostart.target, Slice=session.slice,
                          Restart=no, WatchdogSec=30s, TimeoutStopSec=10s,
                          ExecStart=lumen-compositor --session $LUMEN_DISPLAY
lumen-shell.service       Type=exec, PartOf= + After=graphical-session.target,
                          Requisite=graphical-session.target, Slice=session.slice,
                          Restart=on-failure, RestartSec=300ms, StartLimitBurst=5/30s
lumen-shutdown.target     Conflicts= + After= both session targets, StopWhenUnneeded=true
```

No unit has an `[Install]` section: nothing is ever enabled; the compositor pulls in the rest
with `Wants=`. `graphical-session.target` is `RefuseManualStart=` and is pulled in only by
the `BindsTo=`, so the session's units (the shell, autostart applications, the portals, all
`PartOf=` it) end with the compositor. `$LUMEN_DISPLAY` (the socket name) is expanded from
the manager's environment: unset, it expands to nothing and the compositor serves
`lumen-0`.

*Rejected:* socket activation (the old `lumen-compositor.socket`, now deleted): a client
connecting after a logout would start a compositor again; and the compositor must take the
logind session and the display before it can serve anyone, so there is nothing to gain from
holding the socket early.

### S3. READY after the environment

`READY=1` (with `STATUS=Serving <socket> (n outputs)`) is sent only after the Wayland socket
is served, the control socket listens and the user manager has acknowledged the environment
export. Units ordered after `graphical-session.target` — the shell, the
`app-*@autostart.service` units (their `ExecCondition=` reads `XDG_CURRENT_DESKTOP` from the
manager), portal backends with `ConditionEnvironment=WAYLAND_DISPLAY` — therefore always
find it. Both D-Bus calls are asynchronous with a 2 s bound: READY is sent anyway when they
fail (a session without portals beats none), and nothing waits on the UI thread (PR #29's
start-up time is kept; measured 720 ms from `lumen-session` to `active` on this VM, the
compositor's own start included).

### S4. The compositor exports its environment

With `--session` the compositor calls `org.freedesktop.systemd1.Manager.SetEnvironment` and
`org.freedesktop.DBus.UpdateActivationEnvironment` itself, over sd-bus, for
`WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP` (kept when it already names Lumen), `XDG_SESSION_TYPE=
wayland`, `XDG_SESSION_DESKTOP=lumen` and `LUMEN_SOCKET`; at exit it calls
`UnsetEnvironment` for them (synchronously, 1 s bound). One process knows exactly what it
set and no `systemctl` is forked on the start-up path (niri forks a shell for this).
lumen-session restores everything it recorded afterwards, which also covers a compositor
that crashed before its own unset. dbus-broker cannot unset an activation variable; every
D-Bus service the session uses names a `SystemdService=`, so the manager's environment is
the one that counts. `DISPLAY` (XWayland) and `XCURSOR_*` (Part 2) join the same export.

### S5. Finding the logind session from a user unit

A compositor in `user@.service` is in no session scope. `logind_session::open` now asks a
pure `discover_session` (injected getters, unit-tested): `$XDG_SESSION_ID`; else the
process's own session; else — **only for a process that is in no session at all** —
logind's display session of the user, then the user's one active seat session. A candidate
must be this UID's, on a seat, of class `user`. A process inside a seatless session (an SSH
login, a test run from one) gets no session, as before, and never takes over the console
session it does not belong to. After `TakeControl` the compositor calls `Session.SetType
("wayland")` when the session was a `tty` login; logind restores the type when control is
released.

### S6. systemd supervises the shell

`lumen-shell.service` restarts a failed shell after 300 ms, at most 5 times in 30 s; a clean
exit (the session ending) is not restarted. The compositor never spawns it (KDE's model with
`plasma-plasmashell.service`: rate limiting, the journal and the cgroup come from systemd).
A reconnecting shell gets the snapshot replay over the policy bridge as before. New: the
**zone hold** — when the shell's panel goes away other than at a logout or with its output
(a crash, or a stop to restart it — whether the shell destroyed the surface first or its
connection took it), the exclusive zone it reserved stays in the usable area until a
surface with an exclusive zone maps on that output (the restarted panel) or 5 s pass, so maximized windows do not grow
into the panel's strip and shrink back 300 ms later. `run-desktop.sh` restarts the shell the
same way in its direct mode (`--no-restart-shell` turns it off).

The **placement hold**: in a session the shell starts in parallel with autostart
applications. A window that maps in the 750 ms after READY while no shell has bound is
fitted into its output's usable area once the shell has reserved its zones (or the 750 ms
pass) — moved the least distance, only if it reaches outside. A frame is never delayed.

### S7. The control socket: Varlink

`$XDG_RUNTIME_DIR/<display>.varlink`, interface `io.lumen.Compositor` (provisional name),
served with **sd-varlink** (public in libsystemd since 257; built against an older one,
the compositor has no control socket and lumen-ctl is not built) on a private sd-event loop whose
epoll fd is one source of the UI loop; the user bus of S4 is attached to the same loop. Mode
0600 and `SD_VARLINK_SERVER_MYSELF_ONLY` (peer UID checked by sd-varlink), at most 16
connections. Methods: `GetVersion`, `GetHealth`, `ListOutputs` (with each output's active
workspace), `ListWorkspaces` (Part 3), `ListWindows` (with the workspace, minimized, sticky),
`Reload`, `RunAction`, `Logout`, and `Subscribe` (with `more`: the focus first, then window,
focus, workspace, health and configuration events; a subscriber that does not read is
dropped after 64 events sent while its socket stays full). Parameters are validated against the IDL
before a handler runs, and `varlinkctl introspect` describes the interface. `lumen-ctl` is
its client.

*Rejected:* a JSON socket of our own (sway, Hyprland, niri): no schema, no introspection, its
own framing and peer checks to get right. D-Bus: ties the compositor to the session bus,
which may not exist yet when the compositor starts, and needs bus policy for what is a
private per-user channel. systemd itself now speaks Varlink
(`/run/user/<uid>/systemd/io.systemd.Manager`).

### S8. Health, pulled

`lx.health` (Runtime layer, pure) turns facts into levels: parts (wayland, display,
graphics, render, input, session, presentation, systemd), processes (compositor, shell with
its pid and bind count), memory (pressure, RSS, import cache, staging) and per-output pacing
(presented, missed, misses among the last 128 frames). Nothing is sampled per frame except
one bit written per presented frame; the report is built when `GetHealth` asks. The render
thread's figures are read on the render thread (a post and a post back): an answer missing
after 500 ms makes `render` critical, so the request doubles as a liveness probe of that
thread.

### S9. The watchdog

`WatchdogSec=30`: pinged at most every quarter period from wake-ups that happen anyway (the
1 Hz housekeeping tick on an idle desktop, the frame ticks otherwise; the control source
carries the next ping as a timeout only if nothing else woke the loop first), and only while
the render thread's heartbeat moved or it is idle. A compositor stuck in its UI or render
thread is aborted by systemd and the session ends to the login instead of a frozen screen.

### S10. Logout, stop, reload

Logout (the panel, Ctrl+Alt+Backspace, `lumen-ctl logout`, `RunAction("session.logout")`)
keeps its semantics (every window asked to close, 5 s, stop) and sends `STOPPING=1` first.
SIGTERM stays an immediate, clean stop. SIGHUP (`systemctl --user reload
lumen-compositor`) answers `RELOADING=1` / `READY=1` (Type=notify-reload); with Part 2 it
re-reads the configuration.

### Install layout

Every install destination is relative to the prefix and the session files are configured at
install time (`cmake/LumenInstallSession.cmake`), so `cmake --install build --prefix DIR`
gives a complete tree for DIR: `bin/{lumen-compositor,lumen-shell,lumen-ctl,lumen-session}`,
`lib/systemd/user/` (systemd's `systemduserunitdir` for `/usr`, or
`LUMEN_SYSTEMD_USER_UNIT_DIR`), `share/wayland-sessions/lumen.desktop`, the portal files.
The raw copy of `deploy/systemd/` under `share/lumen/systemd` is gone.

### Consequences

- A session started from a console login or a display manager is the same `lumen-session`
  invocation; `run-desktop.sh --systemd` runs exactly that path from a build tree or a
  scratch prefix, with the units in the runtime unit directory for one run, and checks that
  the user manager ends as it began; `scripts/session-smoke.sh` judges it on the console.
- The idle desktop's cost is unchanged: one more fd in the UI loop that sleeps, a user-bus
  connection with no matches, the watchdog ping inside existing wake-ups.
- Trade-offs: the D-Bus activation environment keeps the session's values after it ends
  (dbus-broker); a shell's notifications on screen at a crash will be lost (Part 5); the
  Varlink interface name is provisional.

---

## Part 2 — one configuration file

Prior art read: niri (KDL, one file, live reload keeping the old config on a parse error,
`niri msg action`), sway (`bindsym`, `input` blocks, `swaymsg reload`), Hyprland, KDE's
kcfg cascade (system file under the user's, key by key), GNOME's gsettings schemas
(typed keys with defaults and ranges), libinput's configuration API.

### C1. TOML, layered, one file

`$XDG_CONFIG_HOME/lumen/config.toml` over each `$XDG_CONFIG_DIRS/lumen/config.toml`, key by
key over compiled-in defaults (tables merge, `[bindings]` per combination,
`[[input.device]]` per device name, arrays replace). `LUMEN_CONFIG=PATH` reads only that
file, `LUMEN_CONFIG=0` none (tests and smokes: ctest sets it for every test). Same reader as
`outputs.toml`: `lx.compositor:toml_lite` became `lx.config:toml` and learned single-line
inline tables (for a binding with options). `outputs.toml` stays separate and
machine-written; the compositor never writes `config.toml`. The documented file of every
default (`deploy/lumen/config.toml`, installed to `share/lumen/config.toml`, printed by
`lumen-ctl config defaults`) is checked by `test_config` to parse to exactly the defaults
and to name every key.

### C2. The compositor reads; the shell gets the bytes

`lx.config` (Runtime layer, pure) holds the schema (one descriptor per key: type, range or
choices, owner, reload class), the parser and its diagnostics, the layers and an inotify
watch. Only the compositor reads and watches the files; zlm_shell_v1 v7 hands the shell the
accepted bytes as a sealed memfd (`config`, with `# lumen-layer: PATH` lines so lines map
back), and the compositor's diagnostics (`config_diagnostic`, `config_done`). The shell
parses the same bytes with the same code and applies its sections (idle, background; later
the switcher and notifications); its command-line options keep precedence. A restarted
shell gets the configuration on bind. Rejected: each process reading the file (two
watchers, two reads, a window in which they disagree).

### C3. Errors keep what works

A syntax error (or a layer that cannot be read, or one over 256 KiB) rejects the
configuration: the one in use stays (at start-up, the defaults). A bad value rejects only
its key, which keeps its value in use ("`5000 is outside 0..1000; keeping 30`"). Unknown keys
and sections warn with a "did you mean" (Damerau–Levenshtein ≤ 2). Bindings report unknown
actions (with a suggestion), bad key names, reserved combinations, duplicates within a file,
held actions without a modifier, `allow-when-locked` on an action that is not lock-safe, and
actions this version does not execute yet (not bound: the key keeps reaching clients).
Apply-time problems join the generation's report: a keymap that does not compile (with
xkbcommon's own reason; the previous keymap stays, and every later generation compiles it
again and reports it until it compiles), a cursor theme that is not found, a lock
command not in `PATH` (the shell). Each generation is logged once (`config` category) and
the shell shows one notice per generation with problems (Part 5 turns it into a
notification); `lumen-ctl reload` prints them and exits 1 on a rejected file; `lumen-ctl
config check [FILE]` validates offline, keymap included.

### C4. The compositor owns the binding table

`[bindings]` maps a combination (`Mod+…+Key` with the key at its first shift level, a
pointer button, or a bare modifier = a tap) to an action of one namespace shared with
`lumen-ctl action` and the control socket's `RunAction` (`lx.config:actions`). The
compositor matches keys and executes `window.*`, `keyboard.*`, `session.*` itself and sends
`shell.*` actions to the shell (zlm v7 `action`, with a serial noted as the shell's key
press so activation tokens work). Dispatch order: internal (VT, logout) → Super+Escape →
window-manager keys → configured bindings → legacy `grab_key` grabs → taps (configured,
then legacy) → the input method → the client. Locked: only `allow-when-locked` bindings;
inhibited: only `allow-when-inhibited`. Shell actions match only while a v7 shell is bound:
without one (a shell restarting, a v4–v6 shell) the key goes on to a legacy grab or the
client instead of being swallowed. `shell.lock` is bound only with `idle.lock-command` set
(Super+L otherwise reaches applications, as without a locker); a lock command given only on
lumen-shell's command line keeps its legacy grab. The hard-coded Super+drag became the
default `Super+BTN_LEFT/BTN_RIGHT = window.move-interactive/resize-interactive`. Rejected:
the shell owning all bindings (window keys would die with the shell, and two matchers would
have to agree); this refines "the shell decides intent": the key → action mapping is user
configuration the compositor interprets, every UI decision stays the shell's.

### C5. What applies, and when

Live: keyboard layouts (compiled on the worker, swapped on the UI thread keeping Caps and
Num Lock by name, the locked layout clamped, held keys pressed again; every `wl_keyboard`,
the input method's grab and the focus get the new keymap and modifiers), repeat
(`repeat_info` to every keyboard), libinput settings on every device (class sections, then
per-device overrides; unsupported settings skipped and logged), the scroll factor, the
cursor theme and size (themes read on the worker; `XCURSOR_*` re-exported in a systemd
session), focus mode (click or sloppy) and raising, the bindings, and the shell's idle and
background. Start-up only: `numlock`. `[workspaces]` from Part 3 (live: mode, count, names,
switch-on-activate, move-follows). Applied by later parts: `[windows]`, `[switcher]`,
`[notifications]` (parsed and validated now). A reload changes only what
differs (`diff_config`).

### C6. Reload pipeline

inotify on each layer's directory (and a symlinked file's target directory; a missing
directory through its nearest ancestor) → 100 ms debounce → read and parse on the UI thread
(a few KB from the page cache, ~0.1 ms) → apply; keymap and cursor jobs on the worker → when
they finish, the generation is reported (log, shell, the control socket's `Reload` reply and
`Subscribe` event, `READY=1` after SIGHUP's `RELOADING=1`). Unchanged bytes after an inotify
event: nothing. A reload arriving while one is applied runs after it. Nothing runs while the
files do not change: the inotify fd sleeps.

### Consequences

- One file configures the session; edits apply as they are saved, mistakes are reported
  where they are and never cost the working configuration.
- zlm_shell_v1 is version 7 (config, config_diagnostic, config_done, action); the shell
  binds 7 and grabs no keys from a v7 compositor.
- Default repeat changed from the old hard-coded 40/400 to 25/600 (X's, sway's and KDE's
  defaults); the touchpad defaults to tap, natural scrolling and disable-while-typing
  (GNOME and KDE).
- Trade-offs: the file is read on the UI thread (bounded, page cache; a networked home could
  stall it — the read is 256 KiB at most); notices are the panel's until Part 5; a virtual
  keyboard whose keymap equalled the seat's keeps the old layout until its client sends a
  new one (fcitx5 does on the grab's new keymap).

## Part 3 — workspaces, minimize, sticky

### W1. Per output, dynamic by default

Each output has an ordered list of workspaces, one of them active. **Dynamic** (default):
there is always exactly one empty workspace at the end of each list, and an empty workspace
that is not active goes (when it is left, or when its last window closes); names are the
positions, renamed when one goes. **Static** (`workspaces.mode = "static"`): `count` per
output (1–16), named from `names` or by position, never removed. Index bindings act on the
focused output (the keyboard-focused window's, else the pointer's); in dynamic mode an index
past the end is the empty last workspace, as in niri. Each workspace remembers the window
that last had the keyboard on it; switching there focuses it if it is still shown, else the
topmost window there, else nothing — unless the keyboard is on a shown window of another
output, which keeps it. New windows join the active workspace of their output; children
join their parent's and move with it; a window dropped by an interactive move joins the
active workspace of the output it was dropped on.

*Rejected:* a global grid (KDE): one switch changes every monitor at once, which fits a
multi-monitor desk worse than independent outputs (sway, Hyprland, niri, COSMIC); GNOME's
"workspaces on the primary only" default as the only mode: a second monitor that never
switches is half of a workspace model. Per-output lists also map 1:1 onto ext-workspace
groups.

### W2. One visibility rule: workspace, minimize, sticky

A window is shown iff it is mapped, not minimized, and sticky or on its output's active
workspace (or it is being dragged: it comes along). A change hides or shows its tree
container (`scene_node::set_visible`) and damages its rects; hidden windows take no input
(skipped by the hit test), lose the keyboard to their output's workspace focus target, close
their popups, get `suspended` in a configure (Mutter does this for minimized and
other-workspace windows; the configure goes out at the end of the operation, carrying the
new `activated` too), and their frame callbacks fall to the existing 995 ms throttle. A
hidden window's commits damage only the capture views (`scene_graph::note_damage_views`), so
they repaint no output. A capture view draws its window's tree even while the window is
hidden (`build_draw_list_into` skips only the root's own visibility), so a screencast of a
window on another workspace keeps updating. Minimize is `xdg_toplevel.set_minimized`
(`wm_capabilities` now offers it), the shell's `request_minimize`, `window.minimize`
(Super+H); an activation (the shell's request_activate, a granted xdg-activation)
unminimizes. Sticky (`window.sticky-toggle`) shows a window on every workspace of its
output.

*Rejected:* moving hidden windows off screen (X11 WMs): it changes their geometry, output
membership and enter/leave events, and captures would see nothing.

### W3. ext-workspace-v1 is the interface; zlm adds membership

The compositor serves `ext_workspace_manager_v1` v1 (staging): a group per output
(`capabilities` 0; `output_enter` for each `wl_output` of the client, also one bound later),
a handle per workspace (`id` only for a static workspace the configuration names —
`<output>/<name>`, unique and stable across sessions; `name`; 1-D `coordinates`, its
position; `state` active and urgent; capabilities activate, plus assign in dynamic mode).
Changes are diffed per bound manager and closed by one `done` per operation; requests run on
`commit`, assigns before activates (niri), the whole commit one change; deactivate, remove
and create_workspace are not offered and are ignored; nothing changes while the session is
locked. Hidden from sandboxed clients. lumen-shell's indicator is a client of it like any
bar. zlm v7 adds only what ext-workspace lacks: `toplevel_workspace(toplevel,
ext_workspace_handle_v1?)` naming the shell's own handle (null for a sticky window), sent
after the ext-workspace `done` that introduced the workspace, again for every window when
the shell binds ext-workspace after its bridge; the `sticky` toplevel flag; `minimized` now
sent as a state. The never-implemented `zlm_workspace_manager_v1` / `zlm_workspace_v1` /
`snapshot_workspace` are deprecated. ext-workspace is therefore a P0 protocol (zlm's glue
links its interface tables).

*Rejected:* zlm workspace objects (a second, private model a bar cannot use); the cosmic
workspace protocol (superseded by ext-workspace; labwc moved too).

### W4. Hotplug keeps what is visible visible

An output that goes: its active workspace's windows join the fallback output's active
workspace (they stay in view, placed by the existing evacuation); its other non-empty
workspaces are appended to the fallback's list (static: merged into the workspace at the same
position); each remembers the gone output's identifier and its position there, and so does
each window (`toplevel_record::home_workspace`). When that output returns, its workspaces go
back in their order (ext-workspace: leave, enter), windows that were merged go to a workspace
at their old position (made if needed), and the workspace that was active there is active
again. With no output left, workspaces wait (stranded, still showing what was shown) for the
next output. ext-workspace `assign` (dynamic) moves a workspace and its windows to another
output.

### Consequences

- Default bindings: Super+1…9, Super+Shift+1…9, Super+Page_Up/Down and Super+Ctrl+Left/Right,
  with Shift to move the focused window (it follows with `move-follows`), Super+H minimizes.
- `lumen-ctl workspaces`, workspace fields in `windows` and `outputs`, `workspace` events in
  `monitor`.
- No per-frame cost: the model changes on actions only; a switch is O(windows) visibility
  toggles and damage; the frame path sees fewer visible nodes.
- Trade-offs: a window straddling two outputs belongs to the one its frame overlaps most and
  hides on both when that workspace is left; a workspace switch is not animated (cost on
  this class of GPU); the shell's task list shows the active workspace's windows and sticky
  ones (GNOME's and KDE's default), not a global list.

---

## Part 4 — switching and snapping

Prior art: GNOME's switcher popup (a delayed show, the primary modifier ends it), KWin's
TabBox (an in-compositor keyboard grab), niri 25.11 `recent-windows` (`open-delay-ms`,
`debounce-ms`), labwc; Windows 11 Snap (Super+arrows), GNOME edge tiling and its tiled
states, KWin quick tiling and its outline.

### K1. The window switcher is the shell's, most recently used first

Alt+Tab (and Super+Tab) is `switcher.next`, Alt+Shift+Tab `switcher.previous`, Alt+grave and
Alt+Shift+grave the same within the focused window's application. lumen-shell keeps the
windows' **MRU order** in its task model: seeded from the stacking order when a snapshot
lands (a restarted shell knows no history; click to focus raises, so stacking is the best
record), every `focus_changed` moving that window to the front, a new window entering at the
back until focused. With focus-follows-mouse (`focus.mode = "sloppy"`) a focus that lasted
less than 750 ms before it moved on does not count (niri's `debounce-ms`): the window goes
back where it was, unless the switcher or the user chose it. Scope `workspace` (default) lists
the windows the panels list (each output's active workspace and the sticky ones), `all` every
window; minimized ones per `include-minimized`; modal dialogs never (the parent stands for
them). The selection starts on the previous window (the second entry), or the last one
backwards. Nothing is drawn for a quick Alt+Tab: the view — an overlay layer surface
centred on the focused window's output, no keyboard interactivity, exclusive zone -1, at the
output's fractional scale — appears only after `open-delay` (150 ms). It shows each window's
icon (the panel's resolver: theme name, its own pixels, its desktop entry, a letter tile) in
96 px cells and the selected window's title; hovering selects, a click commits. The commit
is `request_activate`: the compositor unminimizes the window and switches to its workspace.

*Rejected:* thumbnails now — a readback per window costs ~12 ms each on SVGA3D; the cheap
path for later is a compositor-drawn mirror (`zlm_shell_v1.get_window_mirror` drawing the
window's textures into a shell subsurface, Clutter clone / KWin's WindowThumbnailItem); a
switcher in the compositor (KWin): the shell owns every UI decision.

### K2. A held action carries the modifier across the process boundary

A binding of a held action (`lx.config` marks the `switcher.*` actions held; they must have a
modifier) gives the shell a **hold**: `zlm_shell_v1.held_action` (v7) creates a
`zlm_held_action_v1`. While the binding's **primary modifier** — its highest: Super, Alt,
Ctrl, Shift, GNOME's `primaryModifier` — stays down, the compositor diverts every key press
to the hold (`key`: the first-level keysym, the modifier bits, and the action the user's
binding of that key names, so Alt+Shift+Tab steps back whatever it is bound to) instead of
the focused client; modifier presses, and releases of keys pressed earlier, still reach the
client, which keeps the keyboard focus and a correct modifier state throughout. Diverted
keys repeat at the keyboard's rate. Letting go of the modifier sends `released` (the
switcher commits); the shell ends a hold itself by destroying it (Escape, Return, a click);
the compositor sends `cancelled` when the session locks, at a VT switch, when a move,
resize or drag begins and when a button is pressed on a window (GNOME closes its switcher on
an outside click); the hold ends with the object's destruction (also the shell's
connection going). With no v7 shell, `switcher.next` is the compositor's **quick switch**:
the topmost other shown window is activated, so Alt+Tab works while the shell restarts.

*Rejected:* the shell taking the keyboard with an exclusive layer surface for the switch
(the focused application sees leave/enter even for a cancelled switch, and the first Tab can
race the surface mapping); "every modifier of the binding up" as the end (Alt+Shift+Tab would
stay open while Shift is held after Alt was let go).

### K3. Snapping: Windows 11's Super+arrows

`window.snap-left/right/up/down` (Super+arrows) follow Windows 11's table, made total:
normal → Left/Right a half, Up maximizes, Down minimizes; a half → Up/Down the quarters,
the other direction restores, the same direction goes to the far half of the neighbouring
output (none: nothing); a quarter → the opposite horizontal quarter, Down/Up back to the
half; maximized → Left/Right a half, Down restores. The snap is window-manager state
(`toplevel_record::snap`, never with maximized), applied like maximize: one configure with
the size and the place, the commit answering it places the window (the far edge anchored, so
a right half keeps touching the right edge whatever size the client draws), `restore` holding
the normal geometry the first time the window leaves the normal state. Halves split the
usable area with the odd pixel to the left (top) part, minus a server-side frame; a minimum
larger than the half is kept, anchored at the snap's outer corner (KWin). The configure
carries xdg `tiled_left/right/top/bottom` (xdg_wm_base v2+) — every edge of a half or a
quarter lies against the usable area's edge or the other part, so all four — and GTK draws
no shadow or rounded corner, foot fills its cell grid. Snapped windows refit with the usable
area (a panel appears), follow evacuation to the same half of the fallback output and return
snapped with their home output (`home_snap`); fullscreen remembers the snap. The shell sees
the window as `tiled` (`toplevel_state`), `lumen-ctl windows` too.

### K4. Edge snapping, drag-to-restore, keyboard move and resize

During any interactive move (client, shell, frame, Super+drag, keyboard), on the tick that
applies the coalesced motion, the pointer within `edge-snap-distance` (8) of an **outer**
edge of the layout — one no other output continues at that point — selects that half, within
`edge-snap-corner` (64) of a corner along it the quarter, the top edge maximize (its ends the
top quarters); a release there snaps, with the window's pre-drag geometry as its normal one.
The zone shows a **preview** drawn by the compositor: one fill and a 1 px border from two 1×1
premultiplied textures (the accent at 25 % and 80 %), in a layer between the windows and the
top layer-shell layer — five draws while shown, damage only when it appears, moves or goes;
in output captures, never in a window's. A move started on a maximized or snapped window
does not move it until the pointer has travelled 8 px (KWin, GNOME); then it is restored to
its normal size under the pointer (the pointer keeps its fraction of the width and its
distance from the top; without a normal geometry, two thirds of the usable area), one
configure, and the motion until that configure's commit moves where it will land; Escape
puts it back maximized or snapped. `window.move-keyboard` / `window.resize-keyboard`
(Alt+F7 / Alt+F8) start the same grab with no button: the pointer is warped to the window's
centre or its frame's bottom-right corner and drives it; arrows move the pointer 10 logical
px (Ctrl: 1), repeating; for a resize the first arrow of an axis picks that edge (GNOME);
Return, Space or a button applies, Escape cancels, a focus change ends it.

*Rejected:* a shell-drawn preview (an IPC round trip per zone change on the drag path, a
frame late); a preview under the dragged window only (it would need the window's tree split
from the window layer; the 25 % tint over it is legible).

### Consequences

- Default bindings: Alt+Tab, Super+Tab, Alt+Shift+Tab, Super+Shift+Tab, Alt+grave,
  Alt+Shift+grave; Super+Left/Right/Up/Down; Alt+F7, Alt+F8.
- zlm v7 gains `held_action` and `zlm_held_action_v1`; the shell's bridge gives a hold to its
  switcher and ends it as described.
- No idle cost: the switcher's timer slot is armed only during a hold, the view exists only
  while shown; snapping is configure work on an action; edge zones are one rect test per
  coalesced move tick, the preview two textures staged once.
- Trade-offs: the preview tints the dragged window too (it is above the window layer); a
  window maximized before it ever mapped is restored by a drag to two thirds of the usable
  area (it has no normal size); thumbnails wait for the compositor mirror.

## Part 5 — notifications and the tray

Prior art: the freedesktop Notifications specification 1.2 (methods, `ActivationToken`
before `ActionInvoked`, close reasons 1–4, the markup subset); mako (one layer surface for
the stack, `top`/`overlay`), GNOME Shell (banners suppressed over fullscreen, critical ones
shown, ids from a counter, the message tray), KDE Plasma (do-not-disturb while the screen is
shared, critical shown in do-not-disturb, a middle click on the indicator toggles it, the
history); KDE's StatusNotifierItem specification (`org.kde.StatusNotifierWatcher`, KDE's
`ProvideXdgActivationToken`), `com.canonical.dbusmenu`, waybar's tray (watcher and host in
one process), libappindicator's path registrations.

### N1. One user-bus connection, opened after the panel shows

lumen-shell opens the user bus once its first panel frame is on screen (the start-up path
stays as fast as before) and keeps one `sd_bus` for everything: the notification server,
the tray's watcher and host, and the application launcher's `StartTransientUnit` calls
(which no longer open a connection of their own). It is one more fd in the shell's `ppoll`
set; the bus's own reply deadline lowers the poll timeout; nothing polls or wakes
periodically, so an idle desktop costs nothing (measured: no wakeup of the shell in 10 s
with a history and a tray item). Well-known names are requested with `SD_BUS_NAME_QUEUE`
and no replacement: when mako, dunst or a KDE watcher owns one, the shell logs it, waits in
the queue and takes the name when the other exits. Every call is answered inline on the
shell's single thread.

*Rejected:* a notification daemon of its own process (another process to supervise, and the
popups need the shell's outputs, lock state and capture state anyway); a second connection
for the launcher (one more fd and Hello for nothing).

### N2. The Notifications server

`org.freedesktop.Notifications` at `/org/freedesktop/Notifications`: capabilities
`actions body body-markup body-hyperlinks icon-static persistence` (no `body-images`: an
`<img>` shows its alt text; no `sound`); server information `Lumen, Lumen, <version>, 1.2`.
Ids are never 0 and never handed out twice in a session: the counter is persisted in blocks
of 64 (`$XDG_RUNTIME_DIR/lumen/shell-state`, also do-not-disturb; one write per 64
notifications), so a restarted shell continues after anything a client may still hold.
`replaces_id` of an open notification replaces it in place (same id, same card, its timer
restarted); an unknown one makes a new notification. `expire_timeout` -1 takes
`notifications.timeout`, `timeout-low` or `critical-timeout` by urgency (0: never), 0 never,
> 0 that many ms; timers run only while a card is shown and pause while the pointer is over
the stack and while the session is locked. Hints read: `urgency` (byte; int and uint
accepted), `category`, `desktop-entry` (its icon), `image-data`/`image_data`/`icon_data`
(validated: 8 bits, RGB or RGBA, the rowstride against the length, ≤ 4096²; box-filtered into
at most 96² at once), `image-path`/`image_path` (a `file://` URI, a path in any format the
background decodes, or an icon name), `resident`, `transient`; image precedence per 1.2.
The body markup subset — `<b> <i> <u> <a href> <img alt> <br>`, the five entities and numeric
ones — is parsed into styled spans; anything else shows as the text it is; links open only
`http`, `https` and `mailto`, through `xdg-open` launched as its own unit, only on a click.
Close reasons: expired 1, dismissed by the user 2 (close button, a click on a card without a
default action, a right or middle click, an action invoked, `notifications.dismiss[-all]`),
`CloseNotification` 3 (an unknown or closed id is an error, as the spec says); none at the
shell's exit (the spec allows it). An action click requests an xdg-activation token with the
click's serial, then emits `ActivationToken(id, token)`, `ActionInvoked(id, key)` and
`NotificationClosed(id, 2)` in that order (a `resident` one stays open, in the history).
Senders with open notifications that have actions are watched (`NameOwnerChanged`, at most
32): when one leaves the bus its actions are no longer offered. A sender sending more than 20
a second has the excess merged into its newest open notification (warned once).

### N3. One layer surface for the stack

The popups are one wlr-layer-shell surface on the output of the focused window (else the
one the pointer was on), kept there while any shows: the newest card at the anchor edge
(`position`), at most `max-visible`, a "+n more" pill beyond; 360 logical px wide, exclusive
zone 0 (it keeps clear of the panel), no keyboard, an input region of the cards only. It is
on the `top` layer — below fullscreen windows, as GNOME and KDE suppress banners over
fullscreen — and moves to `overlay` (`set_layer`) while a critical one is shown. A card: a
48 px icon, the summary in bold on one line (ellipsized), the body wrapped to four lines
(`lx.text::wrap_text`: greedy breaks at spaces, after hyphens and slashes, around CJK
characters, a cluster break for over-long words, "…" on the last line; each piece shaped in
its span's font), up to three action buttons (more go into a menu), a close button. The
whole buffer is repainted on a change (a few cards; cheaper than tracking them) and
committed once; nothing is drawn while nothing changes. `lx.text` keeps up to eight faces
loaded (bold summary, regular and italic body, the panel's fonts) instead of one, so mixing
styles no longer reloads a face per switch, and its glyph cache, keyed per face, survives it.

*Rejected:* a surface per notification (a commit and a configure round trip each, and
stacking them is the compositor's guesswork); animation (a frame's cost per change).

### N4. Do-not-disturb, the lock, capture, the history

Do-not-disturb sends low and normal notifications straight to the history, open and unread;
critical ones still pop up (KDE). It is set by `notifications.toggle-dnd`, a middle click on
the panel's bell (KDE's gesture) or the history menu's first entry, and automatically while
any client captures the screen (`dnd-while-sharing`, zlm `capture_state`); the bell is drawn
crossed out then. While locked nothing of the shell shows (the compositor hides it) and new
notifications queue with their timers stopped; at the unlock the newest `max-visible` pop up
and older ones go to the history. The history (`history` entries, in memory) is the bell's
menu: each entry's time, application and summary; its actions while it is open and its
sender is there; Clear all; a click removes an entry. The bell shows the unread count.

### N5. The tray: watcher and host in the shell

The shell owns `org.kde.StatusNotifierWatcher` (queued behind another, e.g. KDE's
kded: then it is only a host, registered with that watcher and following its signals) and
`org.kde.StatusNotifierHost-<pid>`, and emits `StatusNotifierHostRegistered` when it gets the
watcher. Items register by bus name or by object path (the caller's unique name then);
both are kept as `bus/path`, deduplicated, at most 32, dropped when their name loses its owner.
Each item's properties are read once (`GetAll`) and again only when it signals a change,
coalesced per loop iteration. Panel cells, left of the bell: the icon by name (the item's
`IconThemePath` first, then the theme), else the pixmap nearest the size (ARGB32 in network
order, premultiplied), the attention icon for `NeedsAttention`, the overlay composited,
`Passive` hidden; a tooltip after 500 ms. Left click: `ProvideXdgActivationToken(token)` (a
token for the click's serial) then `Activate(x, y)`, or the menu for `ItemIsMenu`; middle:
`SecondaryActivate`; right: the dbusmenu menu, else `ContextMenu(x, y)`; the wheel `Scroll`
(120 per notch, KDE's units). When the shell restarts, items see the watcher's owner change
and register again.

### N6. Menus are xdg_popups of the panel

A `com.canonical.dbusmenu` menu is read with `AboutToShow(0)` and one `GetLayout(0, -1,
…)`, shown as an `xdg_popup` of the panel (`zwlr_layer_surface_v1.get_popup`, an explicit
grab on the click's serial; submenus are popups of their parent menu), and followed while
open (`LayoutUpdated`, `ItemsPropertiesUpdated` → the layout read again). Separators,
disabled items, check and radio states, icons, shortcuts and mnemonics (underlined; the
letter activates) are drawn; arrows, Return, Space, Escape and Left/Right drive it. A click
sends `Event(id, "clicked")`, opening `"opened"`, closing `"closed"`. The panel's layer
surface takes no keyboard; while one of its menus is open it is `on_demand`, which lets the
popup grab take the keyboard (the compositor gives a layer surface's popups its
interactivity), and goes back to `none` when the menu closes. The same menu machinery shows
the notification history and a notification's further actions.

### Consequences

- New `lx.shell` partitions `:user_bus`, `:notifications` (model, markup, image data, the
  D-Bus server), `:notify_view`, `:menu_popup` (menus and tooltips), `:tray` (watcher, host,
  dbusmenu); their bodies are implementation units (`*.cpp`), the partitions declarations only.
  `lx.text` gains wrapped paragraphs and several loaded faces.
- `notifications.*` actions are available (no default bindings); `[notifications]` applies
  live; configuration problems are a notification (until dismissed, replaced by the next
  report) once the server runs, the panel's notice before.
- The shell binds `xdg_wm_base` (popups only) and answers its pings.
- No idle cost: the bus fd sleeps; the notification timer is armed only while a card's timer
  runs, the tooltip's only while hovering an item.
- Trade-offs: notifications on screen when the shell dies are lost (no `NotificationClosed`
  can be sent; ids stay unique); the D-Bus activation environment is not involved (the shell is
  not bus-activated: a `Notify` before the shell runs fails with ServiceUnknown, as with mako
  without activation); a notification's "More…" menu has no keyboard (the stack takes none);
  item icons in `icon-data` of dbusmenu items are not drawn (names only); a right-to-left body
  is shaped per piece in visual order but its lines are laid left to right.
