---
name: agent-screenshoter
description: Take screenshots (and pick colors) on Lumen's live seat — per-host tool inventories with the re-verify one-liner, grim/slurp over wlr-screencopy v3 (Lumen, sway, Hyprland), the xdg-desktop-portal Screenshot wire contract (exact D-Bus invocation, lumen-portal-probe, target semantics, locked-session refusal), evidence-path conventions, and why xdg-desktop-portal-wlr is never the screenshot path and must never be installed. Use whenever pixels from the running desktop are needed as task evidence.
---

# Screenshots on the Lumen seat (agent-screenshoter)

> **Scope:** current-project — hosts, tool inventories and wire contracts are this project's.

Two hosts share the hostname `iritur`. Detect which one you are on before anything else:
`ls /sys/class/drm/card*/device/driver` (vmwgfx → VM; i915+nouveau → notebook) — same
rule as `lumen-build-run`. Inventories below do **not** transfer between them.

## 1. Tool inventory (re-verify every session)

```bash
for t in grim slurp import convert magick wl-copy wl-paste busctl gdbus wlr-randr wayland-info wf-recorder; do command -v $t || echo $t ABSENT; done
```

- **Notebook `iritur`** (i915+nouveau), verified 2026-11-17 by running that line: present
  `grim`, `slurp`, `import`, `convert`, `magick`, `wl-copy`, `wl-paste`, `busctl`,
  `gdbus`, `wlr-randr`, `wf-recorder`; absent `wayshot`, `scrot`, `maim`, `spectacle`,
  `flameshot`, `xwd`, `wayland-info`.
- **Dev VM** (vmwgfx), from task records: present `grim`, `slurp`, `import`/`convert`,
  `wl-copy`; absent `wayshot`, `scrot`, `maim`, `spectacle`, `flameshot`, `xwd`. Whether
  the VM also has `magick`/`wlr-randr`/`wf-recorder` is **unverified** — run the
  one-liner there before relying on them; never assert unverified extras.

An absent tool is reported as absent — **never installed** (see §6).

## 2. The live session

A Lumen desktop session is brought up by `scripts/run-desktop.sh`; the portal stack rides
on `scripts/portal-session.sh`, sourced from it (bring-up `run-desktop.sh:585`, teardown
via `portal_session_stop` in `cleanup()` `run-desktop.sh:518`). `portal-session.sh`
exports `WAYLAND_DISPLAY`/`XDG_CURRENT_DESKTOP=Lumen`/`XDG_SESSION_TYPE=wayland` to the
systemd user manager (`portal-session.sh:113-128`), starts the backends including
xdg-desktop-portal-wlr for ScreenCast (`:144-160`) and xdg-desktop-portal-lumen, restarts
the frontend (`:174`), and undoes all of it (`portal_session_stop`, `:193-224`).
From SSH, compositor work joins the seat with `--join-seat-session` (see `lumen-build-run`).
grim & co. are direct Wayland clients: they need the session's `WAYLAND_DISPLAY`.

## 3. Choosing the path

Lumen implements **wlr-screencopy v3**: the `zwlr_screencopy_manager_v1` global is
advertised at version 3 (`modules/lx.compositor/wire_install.cpp:779-784`), frames come
from the shared capture core (`modules/lx.compositor/wire_capture.cpp`; ADR-009 Part 4
status `docs/adr/009-app-compatibility-and-capture.md:8-11`, Decision 24 `:184-185`);
cursor inclusion rides ext-image-copy-capture's PAINT_CURSORS option
(`wire_capture.cpp:1022`). ADR-009's consequences (`:238-241`): grim (output, window,
with pointer), wf-recorder and lumen-probe capture work at scale 1 and fractional scale,
pixel-identical to the compositor's own read-back.

| Seat runs | Path |
| --- | --- |
| Lumen | grim direct (wlr-screencopy v3 above), or the portal path (§5) for portal-shaped evidence |
| sway / Hyprland (wlr-screencopy compositors) | the same grim commands work — verified claim for Lumen only; for the others rely on their wlr-screencopy support, do not overclaim |
| weston (no wlr-screencopy) | grim fails; the portal path applies only where a portal frontend + backend actually run |

## 4. Direct capture: grim + slurp

`grim -h` and `slurp -h` (verified on the notebook): grim takes `-g <geometry>` region,
`-o <output>` single output, `-T <identifier>` foreign-toplevel (window) capture, `-c`
include cursors, `-t png|ppm|jpeg` plus `-s` scale, `-q` jpeg quality, `-l` png level;
slurp takes `-o` (select an output), `-f` (geometry output format), `-d` (print
dimensions). Output names come from `wlr-randr` (present on the notebook; works against
Lumen's `zwlr_output_manager_v1`).

```bash
export WAYLAND_DISPLAY=<session socket>          # e.g. from the session's env
wlr-randr                                         # list outputs, pick names
grim all-outputs.png                              # combined layout (no -o)
grim -o DP-1 single-output.png                    # one output
grim -g "$(slurp)" region-interactive.png         # drag a region
grim -g '800x600+100+200' region-explicit.png     # explicit WxH+X+Y
grim -T <foreign-toplevel identifier> window.png  # one window (ext-foreign-toplevel handle)
grim -c -g "$(slurp)" region-with-cursor.png      # include the cursor
grim -t jpeg -q 90 shot.jpg                       # format/quality
wl-copy -t image/png < shot.png                   # clipboard hand-off (wl-copy on both hosts)
```

`slurp -o` alone prints the chosen output's name for `grim -o`. (The drag/Enter/click/
Escape selection semantics at `scripts/app-smoke.sh:336-348` are the **portal's**
interactive area picker, §5 — slurp has its own drag selection.)

**grim is not side-effect-free on a Lumen session.** A wlr-screencopy frame counts as
active capture for 2 s after it (`k_indicator_ns`,
`modules/lx.compositor/wire_capture.cpp:97,1495-1508`), which pushes
`zlm_shell_v1.capture_state` (`wire_capture.cpp:1514`) → the shell raises do-not-disturb
while capturing (`dnd_while_sharing` defaults true;
`modules/lx.shell/shell_desktop.cpp:318-319`, `notifications.cppm:132,224`) and the panel
bell swaps to its DND layout. Back-to-back shots catch each other's flip: rapid grim
probing flipped ~3.4 k px at the bell bbox (notebook, `x2775-3121 y17-46`) — probe-induced,
settled shots byte-identical (`.zcode/memories/lumen-animations-close-ghost.md:64-66,90`).
Space panel shots >2 s apart, exclude the bell bbox from cross-shot pixel diffs, and never
read DND state out of a shot.

## 5. The portal path (xdg-desktop-portal Screenshot)

Wire contract, read off `tests/clients/lumen-portal-probe.cpp:60-62,253-307`:

- Frontend bus name `org.freedesktop.portal.Desktop`, object path
  `/org/freedesktop/portal/desktop`, interface `org.freedesktop.portal.Screenshot`,
  method `Screenshot(s parent_window, a{sv} options)` with options `handle_token` (s),
  `interactive` (b), `target` (u).
- The method reply is only a request handle; the result arrives as the
  `org.freedesktop.Request.Response(u response, a{sv} results)` signal on
  `/org/freedesktop/portal/desktop/request/<sender>/<token>` (probe subscription at
  `lumen-portal-probe.cpp:166-200`); `results["uri"]` is the `file://…` PNG path.
  Response 0 = success, 1 = cancelled, 2 = refused (e.g. session locked — ADR-009
  Decision 30, `009-…md:214-215`).
- Backend-direct: `org.freedesktop.impl.portal.desktop.lumen`, interface
  `org.freedesktop.impl.portal.Screenshot`, `Screenshot(o handle, s app_id,
  s parent_window, a{sv})` returning `(u, a{sv})` in the method reply.

One-liners (wire-exact per the source above; the URI arrives on the **signal**, so pair
the call with `gdbus monitor --session --dest org.freedesktop.portal.Desktop` or
`busctl --user monitor` unless using the probe):

```bash
gdbus call --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.portal.Screenshot.Screenshot \
  "" "{'handle_token':<'shot1'>,'interactive':<false>,'target':<uint32 1>}"
```

The ready-made client (built by the tests build, e.g. `build/test/lumen-portal-probe`)
does the call **and** the Response wait, and prints `response <n>` + `uri <path>`:

```bash
build/test/lumen-portal-probe screenshot [--interactive] [--target 1|2|4] [--backend]
```

Target semantics (`modules/lx.portal/service.cpp:453,511`): `0`/`1` = whole output(s),
non-interactive → a **private runtime file** under `/run/user/<uid>/lumen-portal/
Screenshot-…` (checked by `scripts/app-smoke.sh:7516`); `2` = window, `4` = area, or
`interactive: true` → `$XDG_PICTURES_DIR/Screenshots/Screenshot from YYYY-MM-DD HH-MM-SS…`
(`modules/lx.portal/files.cpp:100-113`); interactive area selection happens in the
compositor (drag/Enter/click/Escape, `scripts/app-smoke.sh:336-348`). A locked session
refuses with response 2 — never the lock screen.

## 6. xdg-desktop-portal-wlr: not Lumen's screenshot path; never install it

`scripts/portal-session.sh:144-155` **does** start xdg-desktop-portal-wlr in a
run-desktop Lumen session — but only as the **ScreenCast** backend (ADR-009 Decision 25,
`009-…md:192-197`). The **Screenshot** interface is served by Lumen's own
`xdg-desktop-portal-lumen` (Decision 26, `:198-204`); `lumen-portals.conf` says
Screenshot lumen, ScreenCast wlr (Decision 28, `:207`; `deploy/portal/lumen-portals.conf`).
xdpw's own Screenshot implementation (grim into `/tmp/out.png`, no choice, no PickColor)
is explicitly **not used**. So "Lumen does not use or need the wlr one" is exactly true
for screenshots; the wlr backend is present only for ScreenCast when already installed.
The agent consumes the session's portal or the direct screencopy tools and **must never
install xdg-desktop-portal-wlr (or any package)** to get a screenshot.

## 7. Evidence conventions

Shots land under the task's evidence run dir, `evidence/<date>-<run>/…` (e.g.
`evidence/2026-11-17-seat-redo/d-supplements/shot-pre-flip-banner-zoom.png` cited in
`.zcode/TODO.md:83-89`); `evidence/` is gitignored (`.gitignore:14`) and never merged.
Portal-written files (Pictures/Screenshots or the private runtime dir) get **copied**
there — the private runtime file is also expired by the backend, so copy before judging.
Report each shot as: artifact path + the exact command + host + session socket/compositor.
Judge panel pixels with §4's side effect in mind: a bell in DND layout is the shot's own
probe artifact, not session state.
