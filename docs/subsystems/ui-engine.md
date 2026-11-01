# The UI engine as built (`lx.ui`, `lx.ui.host`, and its consumers)

ADR-010 designed the engine; this is it as Phase 5 left it, with the measurements. Read
ADR-010 for the why and ui-architecture.md for the model (§1–§8) and the migration ledger
(§9); this doc is what to believe in 2026-11.

## 1. The stack in one paragraph

`lx.ui` (Desktop layer) is the declarative engine: describers build descriptor trees into a
frame arena each tick, a reconciler folds them onto retained `ui_node`s, flex layout solves
in place, and nodes emit a `draw_list` of ADR-010's primitives (rounded bordered gradient
quads, shadows, atlas sprites, glyphs, underlines) with damage-scoped emission. `lx.ui.host`
(Wayland-facing, "Lumen's GDK") puts that engine on Wayland surfaces: one `context` per UI
thread (a single `client_gpu` in the automatic tier — hardware Vulkan → hardware GL → wl_shm
CPU — one renderer, one atlas with glyph and icon caches, the theme, a worker, one timerfd,
the seat router) and `surface`s of any role (`toplevel_role`, `layer_role`, `popup_role`,
`subsurface_role`), each an `ui_root` with its own swapchain and frame loop. Consumers:
`lx.app` (windows), every `lumen-shell` view (panel, switcher, notification stack, launcher,
menus, tooltip — `lx.shell:view_ui`'s literal boxes keep their smoke-pinned palettes), and
the portal's overlay. The compositor never runs the engine; the shell never draws outside
it; `shm_canvas` is gone.

## 2. The frame loop (§4.9 as measured)

Input, a reducer, a timer or a configure leaves work in the root's damage ledger; the
surface's frame source sees it in the same iteration. From idle a frame is drawn at once
(input lands on screen at the next refresh); otherwise it waits for the frame callback —
a surface never draws faster than the compositor presents. A frame: acquire a free image,
take its age as the repaint region, tick the root (describe → reconcile → layout → emit,
damage-scoped), render inside the repaint, present, ack the configure it answers, attach,
`damage_buffer` × ≤ 16 rects, request the next callback. Idle rule (G1): empty ledger, no
animation, no timer due, no key held → no frame callback requested, nothing submitted.

## 3. Measured (this host: VMware SVGA3D, GL tier, 2 vCPUs)

| Gate | Value | Measured | Source |
|------|-------|----------|--------|
| G1 idle | no frames, no wakeups | lumen-shell 0 context switches in 10 s (panel, history, tray) | app-smoke N8, 2026-11-01 |
| G5 allocations | none per steady frame | `test_app_window` G5 checks: 0 operator new over hover frames | ctest |
| G6 input→photon | ≤ 1 refresh + 2 ms | window-move median 9.6–13.1 ms, p95 17.6–21.7 ms (SVGA3D present stalls inflate max) | drag-smoke g, 2026-11-01 |
| G8 shell memory | ≤ 25 MB (set for the cpu tier; the shell rides the automatic tier, GL here — accepted) | RssAnon 23.8 MB fresh, 28.4 MB idle with three window entries and a history | /proc/smaps_rollup, 2026-11-01 (ADR-010 G8 row) |
| first panel | ≤ 14.1 ms of shell work | 3.7–8.0 ms geometry + swapchain + first tick; the submit→present leg is the host's 25–89 ms flip stall | the shell's first-frame line, 2026-11-01 |
| panel redraw | ≤ 314 µs | 213–309 µs of shell CPU per hover-switched frame | schedstat over 240 alternated hovers, 2026-11-01 |

`lumen-ui-bench` rows (S1–S4) live in `tests/bench/results/<machine>-<driver>/` and compare
against `tests/bench/baseline/` per machine — never across hosts.

## 4. The engine comparison (ADR-010 §8.6)

The same scenes in other engines on this host, observed from outside (§8.6's protocol:
process CPU from `/proc/PID/stat`, RssAnon, context switches at idle, the compositor's own
`compositor.latency` lines). Host: VMware SVGA3D (no hardware Vulkan), GL the automatic
client tier, 2 vCPUs, Wayland-only.

Measured 2026-11-01, one engine window at a time on the same compositor build, 10 s
idle after mapping (the §8.6 protocol; scripts: `/proc/PID/stat`, `status`, `smaps_rollup`):

| Engine | Scenes run | CPU in 10 s | RssAnon | Ctx switches in 10 s | Presents in 10 s |
|--------|------------|-------------|---------|----------------------|------------------|
| Lumen `lumen-hello` (GL tier) | S1 (multi-window demo, as shipped) | ~0 ms | 28.9 MB | 1 | — |
| Lumen `lumen-shell` (GL tier) | panel + views + tray | ~0 ms | 23.8–28.4 MB | 0 | — |
| GTK 4 `gtk4-widget-factory` | the gallery (S1-like) | ~2050 ms | 125.1 MB | 2969 | — |
| egui 0.33 (`eframe`, glow), S1 build | S1 | ~20 ms | 18.8 MB | 71 | — |
| egui 0.33, text wall | S4-lite (200 paragraphs ×3 scripts) | ~30 ms | 27.5 MB | 67 | — |
| GPUI | not comparable here | — | — | — | — |
| Slint / Qt Quick / Flutter | not installed | — | — | — | — |

On the "Presents" column, checked at the wire (§8.6's protocol as written was wrong twice
over):

- `compositor.latency: present` is **not** client feedback at all — it is the compositor's
  own input→present for frames *it* composited in response to input (`note_present_latency`
  reads the input timestamp its pipeline packed at commit). The idle measurement windows
  contained no input, so every row read 0, Lumen's included: the column measured nothing.
- GTK4 **does bind** `wp_presentation` (v1, from both GDK and its EGL layer,
  `clock_id(CLOCK_MONOTONIC)`) — but never created a per-surface feedback object in 8 s of
  running, so no per-frame correlation happens from its side either.
- The commit-rate row is not currently logged per client at all (`surface geometry` fires
  on size change only — one line for GTK's whole run). A small compositor addition — a
  per-client commit counter, logged or served like the latency summary — would make §8.6's
  commit-rate row complete for every engine, and its input→present rows would need the
  same anchoring the compositor already does for itself.

So the comparable rows across engines today are CPU, memory and idle behaviour; the
pacing rows await that compositor addition.

What is not comparable, per engine: GPUI needs a hardware Vulkan device this host lacks
(ADR-010 §8.6 keeps it for the hardware-Vulkan host); GTK4's gallery animates by design,
so its switches and CPU say "the factory", not "GTK idle" — the RssAnon gap (125 vs
19–29 MB) is the comparable row; egui ran a purpose-built S1/S4-lite window (the full
`egui_demo_app` pulls an audio stack the comparison does not need), and its ~500 ms
repaint timer is its own idle story — 7 switches/s against Lumen's 0 (the idle rule:
nothing requests a frame). The raw runs: `/tmp/engine-86/` (compositor logs with
`compositor.latency`), the sources `/tmp/egui-cmp` (one small `eframe` window, glow on
GL). Slint, Qt Quick and Flutter are not installed on this host.

## 5. Where things live

| Piece | Module | Notes |
|-------|--------|-------|
| engine core | `lx.ui` | `:element`, `:node`, `:reconcile`, `:flex`, `:style`, `:theme`, `:text_node`, `:input`, `:overlay`, `:scroll`, `:list`, `:text_input`, `:popups`, `:clipboard` |
| Wayland host | `lx.ui.host` | `:context` (per-UI-thread shared state), `:surface` (any role), `:roles`, `:input` (seat router + repeat + IME + cursor shape) |
| shell views | `lx.shell` | `panel`, `switcher`, `notify_view`, `launcher`, `menu_popup` on the host; `:view_ui` — the literal-colour boxes the smoke suites pin |
| portal overlay | `lx.portal:overlay` | select / pick / choose on its own connection; `desktop` keeps the raw capture machinery |
| app windows | `lx.app` | toplevels on the host, multiple per app |
| bench | `lumen-ui-bench`, `scripts/ui-bench.sh` | S1–S4 per backend, JSON under `tests/bench/results/` |
