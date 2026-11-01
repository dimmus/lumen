# ADR 013 — The Settings application

**Status:** Proposed — analysis and design complete (this document); nothing implemented yet.
**Date:** 2026-11-01
**Scope:** new executable `lumen-settings` (+ its UI module library, `executables/lumen-settings/`),
`lx.ui` (the controls the settings surface needs and the engine lacks: slider, segmented
control, select, stepper, swatch row), `lx.a11y` (roles for them), `lx.config` (a new `:edit`
partition that edits the user layer in place; a new `[ui]` section), `lx.compositor`
(additive `io.lumen.Compositor` methods: input-device listing, window rules, output identify;
export of the `[ui]` keys into the session environment), `deploy/` (a desktop entry), docs.
**Deciders:** project owner
**Related:** [ADR-008](008-real-monitors.md) · [ADR-010](010-gpu-ui-engine.md) ·
[ADR-011](011-session-and-window-management.md) · [ADR-012](012-master-stack-tiling.md) ·
[ui-architecture.md](../subsystems/ui-architecture.md) · [ui-engine.md](../subsystems/ui-engine.md) ·
[health.md](../subsystems/health.md) · [shell-state-sync.md](../subsystems/shell-state-sync.md)

---

## 1. Decision

Lumen gets **`lumen-settings`**: a client application on `lx.app` / `lx.ui` — the same GPU UI
engine, the same Zed-derived visual language as the panel and the launcher — that renders the
configuration surface **from `lx.config`'s own schema metadata**, edits **the user layer of
`config.toml` in place**, drives **outputs through `zwlr_output_manager_v1`**, and observes
the session through the **existing Varlink control interface**.

The compositor keeps every source of truth. The app is a viewer and an editor of files and
protocol state; it owns no state of its own and needs no new privileged protocol. One rule
carries the whole design:

> A key that `lx.config` can describe, `lumen-settings` can show; a file the compositor
> watches, `lumen-settings` can edit; a method `io.lumen.Compositor` serves,
> `lumen-settings` can call. Nothing is duplicated; nothing is invented.

The visual mood is Zed's, as it is for the whole engine (ADR-010 §5): flat grouped rows
rather than card grids, search before navigation, values edited in place, restraint in
hover, focus and motion, zero hardcoded color.

## 2. What can be set and observed today (the analysis)

This section is the inventory the app is built against. Every claim has its source in the
tree; nothing here is new.

### 2.1 `config.toml` — the primary surface

Nineteen sections, roughly seventy scalar keys, per-device and per-output override tables
and a 128-entry binding table, all described in one place:
`lx.config:schema`'s `k_keys` / `k_pointer_keys`
(`modules/lx.config/schema.cppm:348-484`), exported as `schema_keys()` and
`pointer_keys()` (`schema.cppm:300-301, 622-623`).

Each `key_desc` (`schema.cppm:269-284`) carries exactly what a settings row needs:
`section`, `key`, `value_kind` (boolean | integer | number | string | enumeration |
string_list), `key_owner` (compositor | shell), `reload_class`
(**live** | **start** | **next_open**, `schema.cppm:260-267`), a numeric range or an enum
word list, a string capacity, and a field pointer into `config_snapshot`.

| Section | Keys (defaults) | Owner / reload |
|---|---|---|
| `[input.keyboard]` | layout, variant, options, model, rules (""), repeat-rate (25), repeat-delay (600), numlock (false) | C / live; **numlock is the only `start` key** |
| `[input.touchpad]` `[input.mouse]` `[input.trackpoint]` | tap, tap-button-map, drag, drag-lock, three-finger-drag, natural-scroll, scroll-method, click-method, accel-profile, accel-speed, left-handed, disable-while-typing, middle-emulation, scroll-factor — per-class field masks (`k_mouse_fields`, `k_trackpoint_fields`, `schema.cppm:44-50`) | C / live |
| `[[input.device]]` | name + any pointer key, ≤ 16 overrides | C / live |
| `[[output]]` | name (connector / "make model serial" / `*`), render-scale (0.25..4) | C / live |
| `[cursor]` | theme (""), size (0 → 24) | C / live |
| `[focus]` | mode (click\|sloppy), raise-on-click, raise-on-sloppy | C / live |
| `[windows]` | edge-snap, edge-snap-distance (8), edge-snap-corner (64), snap-preview, drag-restore | C / live |
| `[tiling]` | layout (floating\|master-stack), master-ratio (0.55), master-count (1), gaps (8) | C / live |
| `[animations]` | enabled (false), stiffness (800), damping-ratio (1.0) | C / live |
| `[workspaces]` | mode (dynamic\|static), count (4), names, switch-on-activate, move-follows | C / live |
| `[switcher]` | scope, include-minimized, open-delay (150 ms) | S / **next_open** |
| `[idle]` | policy (shell\|none), blank-after (600 s), blank-locked-after (60), lock-command, lock-on-blank | S / live |
| `[background]` | spec (`lumen` \| `#RRGGBB` \| `gradient:…` \| `image:PATH:fill\|fit\|center\|stretch\|tile` \| `none`) | S / live |
| `[panel]` | blur (false; ext-background-effect-v1) | S / live |
| `[notifications]` | position, timeout (5), timeout-low (3), critical-timeout (0), max-visible (3), dnd-while-sharing, history (50) | S / live |
| `[general]` | default-bindings (true) | C / live |
| `[bindings]` | `"Super+T" = "shell.terminal"`, string or inline table with `argument`, `allow-when-inhibited`, `allow-when-locked`, `repeat`; `"none"` removes a default | C / live |

The action namespace has 48 actions with owner, argument kind, held/lock-safe/pointer flags
and summaries (`lx.config:actions`, `actions.cppm:27-222`); 52 default bindings
(`schema.cppm:500-567`); the combo grammar and the reserved, never-configurable keys live in
`lx.config:keys` (`keys.cppm:101-111, 336-348`).

**The schema is already a UI model.** `value_kind` picks the control, the range or word
list constrains it, `reload_class` picks the badge, `key_owner` picks nothing the user sees
(the write path is the same) but explains where the value takes effect. Adding a config key
in `lx.config` will make it appear in the app with no pane code written. That is the
"best in class" move: no hand-written pane per key, no drift between file and UI.

### 2.2 Everything that is not in `config.toml`

| Surface | Where | Runtime change |
|---|---|---|
| Output mode/scale/position/transform/VRR | `zwlr_output_manager_v1` (non-privileged, same-UID; sandboxed clients don't see it, `architecture.md:2168-2176`) → persisted by the compositor as `$XDG_CONFIG_HOME/lumen/outputs.toml` (ADR-008 C3, `output_config_store.cppm:3-30`) | live, per head; test/apply jobs |
| Themes | compiled `.lxtheme` from Zed-schema JSON (`lx.ui:theme_compile`, `tools/lumen-theme`); One Dark/Light built in; `LUMEN_THEME` picks one | per process at start; hot reload via `theme_watcher` for file themes |
| UI density / UI font size | engine calls `set_ui_density` / `font_settings`; **no persisted Lumen-wide setting exists** | per process |
| Environment overrides | `LUMEN_CONFIG`, `LUMEN_LOG`, `LUMEN_THEME`, `LUMEN_UI_BACKEND`, `LUMEN_OUTPUT_CONFIG`, `XCURSOR_*`… (§4.7) | at process start |
| Compositor constructor config | `compositor::config` (`lx.compositor.cppm:78-143`): socket, shell path, target fps, present backend, decorations, ping timeout, scales, virtual outputs, worker threads, budgets | CLI only — start |
| Health / live state | Varlink `io.lumen.Compositor` (`control.cpp:45-184`): GetVersion, GetHealth, ListOutputs / ListWorkspaces / ListWindows, Reload, RunAction, Logout, Subscribe | pull + 2 Hz push while subscribed |
| Window rules | `window-rules.toml` + `zlm_window_rules_v1` — **wire handlers inert today** (`wire_shell.cpp:214-223`) and the interface is shell-privileged | n/a |

### 2.3 The control surfaces a plain client may use

- **Varlink `io.lumen.Compositor`** on `$XDG_RUNTIME_DIR/<display>.varlink`: socket mode
  0600 and `SD_VARLINK_SERVER_MYSELF_ONLY` — the session's own UID only
  (`control.cpp:674-675, 726-727`). `lumen-ctl` is the reference client
  (`sd_varlink_connect_address`, `executables/lumen-ctl/main.cpp:123-136`). Limits: 16
  connections, 4 subscribers (`control.cppm:130-137`) — a settings app is one of each.
  `Subscribe` streams `focus, window-*, workspace*, health` (re-evaluated every 2 s while
  subscribed, `compositor_impl.cpp:1394`) and **`config`** events.
- **`zwlr_output_manager_v1`**: heads, modes, custom modes, test/apply; every protocol
  error raised (ADR-008 C1-C8). Client-side generated bindings already exist —
  `tests/clients/lumen-probe-output-mgmt.cpp` drives them.
- **The config files themselves**: read with `lx.config` locate/parse — the same layered
  semantics the compositor applies (`locate.cppm:63-105`); write per §4.3.

Not available to it, by design: `zlm_shell_v1` and everything behind it
(`get_policy_bridge`, `grab_key`, the config memfd push, `zlm_window_rules_v1`) — the
privilege gate is session UID **plus** the shell binary
(`lx.wayland.server.cppm:223-234`). Shortcuts must therefore be captured in-window
(wl_keyboard keysyms), not with `grab_key`.

### 2.4 The reload machinery the write path rides on

The compositor inotify-watches every layer directory; events debounce 100 ms
(`compositor_impl.cpp:1406, 4265-4278`), then re-parse: a **syntax error in any layer
rejects the whole generation** (the previous config stays, `parse.cpp:919-923`); a **bad
value rejects only that key**; unknown keys warn with "did you mean". Diagnostics reach four
places today (log, panel notification, `lumen-ctl reload`, `config check`) — the app becomes
the fifth, via the `Reload` reply (`[]Diagnostic`) and the `Subscribe` `config` event
(generation, errors, warnings). `finish_config_generation`
(`compositor_impl.cpp:4546-4609`) queues one more reload if busy and re-sends the accepted
bytes to the shell over `zlm_shell_v1.config` (`compositor_impl.cpp:4611-4653`).

### 2.5 What the UI engine gives — and what a settings app lacks

Present (ADR-010 Phase 5, `lx.ui:components` / `:popups` / `:text_input` / `:list` /
`:scroll`): button (six styles, sizes, icons), icon_button, toggle_button, checkbox
(tri-state), switch, label/headline, divider, progress_bar, avatar, image, key_binding and
key_binding_hint, tab_bar, panel, status_bar, popover_menu / context_menu / modal / toast,
text_input (IME, bidi, drag), virtualised list, scroll_area; full style/token/theme stack
(~230 tokens, three densities, dark/light, hot reload), focus rings and traps, keyboard
navigation everywhere, AT-SPI roles, 53 Lucide icons (`icon.cppm:56-111`) that already
include every pane icon this design needs (monitor, keyboard, bell, sun, moon, settings,
power, refresh-cw, user…).

Missing for a settings surface (verified absent in `lx.ui`; also
`ui-architecture.md:550-558`): **slider**, **radio group / segmented control**,
**dropdown / select**, **number stepper**, **color swatch row**, and the
**design-system scaffold** above raw `div`/`flex` (rows, groups, labeled controls) that
ui-architecture.md itself lists as "still to build".

Capacities a settings pane must respect (`k_*` constants): ≤ 64 menu entries
(`popups.cppm:374`), 16 overlays per root (`overlay.cppm:171`), 32 timers per root
(`interact.cppm:147`), list pool 32 chunks = 512 rows (`list.cppm:67-72`), text input
1024 B (`text_input.cppm:81`), ≤ 16 damage rects per frame, 64 children per node, frame
arena 512 KiB per window (`window.cppm:92`).

### 2.6 The gaps this ADR closes

| # | Gap | Closed by |
|---|-----|-----------|
| G1 | No slider / segmented / select / stepper / swatch controls | Phase A in `lx.ui:components` + roles in `lx.a11y` |
| G2 | TOML `writer` builds fresh documents (`toml.cppm:71-92`); nothing edits a hand-written layer in place | `lx.config:edit` (§4.3) |
| G3 | Theme, density and UI font size are per-process env/struct — no Lumen-wide persisted setting | `[ui]` section + session env export (§4.7) |
| G4 | No client-visible enumeration of input devices (libinput names live compositor-side; `[[input.device]]` matches by exact name) | `ListInputDevices` on the Varlink interface (§4.8) |
| G5 | Window rules defined (`window-rules.toml`, `zlm_window_rules_v1`) but handlers inert and shell-privileged | Varlink window-rule methods + compositor-side store (§4.8), the app's UI |
| G6 | No desktop entry: the launcher cannot show a Settings app | `deploy/applications/org.lumen.Settings.desktop.in` |
| G7 | No audio stack anywhere in Lumen (PipeWire appears only as ScreenCast plumbing, `architecture.md:1784`) — out of scope, stated here once | future ADR, not this one |

## 3. Prior art read

| Source | What it teaches | Taken / rejected |
|---|---|---|
| **Zed's settings UI** | Search-driven navigation over a flat schema; the raw file is one click away; inline pickers; no modal "apply" | Taken whole — it is the mood of `lx.ui` already. "Reveal in file" becomes "show the TOML lines" |
| **GNOME Settings** | Per-panel daemons behind a thin viewer; continuous apply; 15 s revert timer for display changes | Viewer-over-authority taken (Varlink/files are our daemons); revert timer taken for Displays; panel-daemon *processes* rejected — the authority already exists |
| **KDE System Settings** | KCM plugins, huge surface, explicit apply | Plugin indirection rejected: `schema_keys()` *is* the plugin interface |
| **sway / wlr ecosystem** | Config file is the truth; GUIs (wdisplays, wlr-randr) are optional lenses | Taken: the file stays authoritative; a GUI never becomes a second store |
| **niri** | One readable config file + `niri msg` IPC; no settings GUI | IPC symmetry taken (the app is `lumen-ctl` with a face); "no GUI" rejected — that is this ADR |
| **macOS System Settings** | Sidebar + content, preview-first display pane, humanized units ("10 minutes") | Layout skeleton and humanized labels taken |

## 4. Architecture

### 4.1 Process and trust model

```
lumen-settings (unprivileged client, app_id org.lumen.Settings)
  │  lx.app window(s) on the automatic tier (Vulkan → GL → CPU)
  │
  ├── reads  config layers          (lx.config locate/parse — identical semantics)
  ├── edits  $XDG_CONFIG_HOME/lumen/config.toml   (user layer only; §4.3)
  ├── calls  io.lumen.Compositor    (Varlink, UID-authenticated; §4.8)
  ├── drives zwlr_output_manager_v1 (same-UID, non-sandboxed; test/apply)
  └── never binds zlm_shell_v1, never writes outputs.toml, never holds WM state
```

The app is a client among clients. Everything it can do, `lumen-ctl` plus a text editor can
already do — it does it with better UX. No new privileged global, no protocol exemption, no
daemon of its own. If the compositor is unreachable (control socket off), panes degrade to
file-editing with a "restart Lumen to see some changes" notice — the same fallback a text
editor has.

### 4.2 The read path

- **Config**: `locate()` + `parse()` over the same layers the compositor reads; the UI shows
  *effective* values with a "set in `<layer>`" marker when a lower layer owns the value, and
  a modified marker when the effective value differs from `default_config()`.
- **Live state**: `GetVersion`, `GetHealth`, `ListOutputs`, `ListWorkspaces`, `ListWindows`
  fill the panes that describe the running session (Displays, Session & Health).
- **Events**: one `Subscribe` connection drives everything dynamic — health at 2 Hz, config
  generations and diagnostics, workspace/output changes. The Varlink reply loop lives on the
  worker executor; results are posted to the UI thread with `executor.post(ui, …)` — the
  engine's standard handoff (AGENTS.md §3.3).
- **Outputs**: heads/modes from `zwlr_output_manager_v1` through the client bindings
  (promoted from `tests/clients/lumen-probe-output-mgmt.cpp` into a small
  `lx.wayland.client` helper).

### 4.3 The write path — `lx.config:edit`

Immediate apply, no OK button (Zed's model; the `live` reload class makes it honest). One
key at a time:

1. Re-read the user layer (guards against an external editor's concurrent change).
2. Splice one key: replace the value on its existing line; or add the key under its existing
   section header (with a `# set by lumen-settings` comment); or append the section at the
   end; or delete the line for *reset to default*. Every other byte — comments, blank lines,
   unknown keys, the user's own ordering — is preserved. Bindings are spliced the same way
   (a line per combo; `"none"` lines for removals of defaults).
3. Re-parse the edited text and assert the snapshot changed **only** in the intended key;
   any mismatch aborts the edit (never write a file whose meaning was not verified).
4. Atomic write: temp file in the same directory, `fsync`, `rename` — the pattern
   `output_config_store.cpp:26-30` already uses.
5. The compositor's watch reloads it (≤ ~100 ms debounce + parse). The row shows a pending
   marker; the `Reload`/`config` event closes it. A rejected generation (syntax error made
   by someone else in another layer, or a value the splice could not have produced) reverts
   the row to the still-effective value and shows the diagnostic inline plus a toast.

Why splice instead of regenerating the file: users hand-write layers; regeneration would
destroy their comments and ordering and produce spurious diffs against dotfile repos. The
fresh-document `toml::writer` stays what it is (used by `config defaults` output), and the
splice editor is a new partition (`lx.config:edit`) with its own tests — it never touches
parse semantics.

### 4.4 Live-apply semantics per key

`reload_class` drives the row badge: `live` — none; `next_open` (the three `[switcher]`
keys) — "next time the switcher opens"; `start` (`input.keyboard.numlock`) — "next login".
CLI-only `compositor::config` values (present backend, target fps, …) are shown read-only in
Session & Health with the flag that sets them — honest display, no fake writability.

### 4.5 Concurrent edits and external changes

The app keeps its own `lx.config:watch` on the user layer: an external change re-reads and
re-renders (last writer wins per key; the generation counter from `config` events orders
compositor-visible history). Editing a key whose value another layer shadows shows the
marker and offers "edit anyway" (writes the user layer, as anything would).

### 4.6 Displays

All output state flows through `zwlr_output_manager_v1` `create_configuration` /
`test_or_apply` — the compositor validates, applies and persists `outputs.toml` (ADR-008
C1-C5); the app never writes that file. A failed test reports the protocol error inline and
nothing is applied. After an apply, a 15 s revert modal (GNOME's timer, macOS's "Confirm")
re-tests the previous configuration if not dismissed — misapplied mode + blank screen is the
one mistake this pane must make unhittable. Per-output `render-scale` overrides are config
keys (`[[output]]`) and ride §4.3 like any other key.

### 4.7 The `[ui]` section (new schema)

Four keys, owner compositor, reload `live`: `theme` (string: built-in name or `.lxtheme`
path), `appearance` (`dark` | `light`), `density` (`compact` | `default` | `comfortable`),
`ui-font-size` (number, 10..24 px, the rem base). The compositor already exports
`XCURSOR_THEME`/`XCURSOR_SIZE` from `[cursor]` into the session environment
(`compositor_impl.cpp:3734-3738`); it exports `LUMEN_THEME`, `LUMEN_UI_DENSITY` and
`LUMEN_FONT_SIZE` the same way, and pushes `[ui]` to the shell with the rest of the config
memfd. Engine apps already honor `LUMEN_THEME` at start (`lx.app/window.cppm:34-54`), so:
shell = live (config push), settings app = live (`set_active_builtin_theme`,
`set_ui_density`, `theme_watcher` for file themes), other apps = next start. That boundary
is documented in the pane, not hidden.

### 4.8 Varlink interface additions (additive, same trust)

| Method | Serves |
|---|---|
| `ListInputDevices() → []InputDevice` (name, class, active settings source) | Input pane's `[[input.device]]` picker — names must match libinput's exactly, so the compositor is the only one who knows them (G4) |
| `GetWindowRules() / SetWindowRule(…) / ClearWindowRule(…)` | Window-rules UI; the compositor applies and persists `window-rules.toml` (G5). The inert `zlm_window_rules_v1` handlers stay a shell-side concern; the store and rule application get implemented once, behind both |
| `IdentifyOutputs()` | Compositor draws its own brief overlay per output (the app cannot: layer-shell for overlays is not granted to plain clients) |

Schema additions only; `varlinkctl introspect` reflects them, `lumen-ctl` grows matching
commands in the same change. No method can do more than the user's own hands could
(UID-gated socket, `architecture.md:2213-2221`).

### 4.9 What the app does not do (v1)

No audio (G7 — nothing to drive), no networking, no users, no locale/time (systemd/host
settings, not Lumen's), no theme *editing* (import + compile only), no autostart editor, no
default-app chooser. Each is a future pane against a real subsystem when that subsystem
exists; none is stubbed.

## 5. The UI design — Zed's language on the Lumen engine

### 5.1 Principles (the mood, made rules)

1. **Search before navigation.** One field filters every row in the app; keyboard first,
   mouse sufficient.
2. **Flat rows, not cards.** Grouped lists with headers and dividers; no boxed tiles, no
   nesting beyond two levels.
3. **Values edited in place.** Click a value → the control opens where it is; no per-pane
   Apply, ever.
4. **Restraint.** Hover changes background only (`ghost_element.hover`), never label color;
   focus is the 1 px `border.focused` ring, distinct from selection; motion is 100-150 ms
   fades, `reduced_motion` honored; nothing bounces.
5. **Zero hardcoded color.** Every pane draws from `tok::*`; dark/light and any compiled
   Zed theme restyle the app unchanged; densities and font size scale it for free.
6. **The file is nearby.** Every row can show the TOML it writes; the config's raw text is
   one toggle away.
7. **Silence is success.** Applying a setting produces no toast; only failures and external
   changes speak.

### 5.2 Window and navigation

```
┌──────────────────────────────────────────────────────────────────────┐
│  🔍 Search settings…                                        /        │  search row (text_input, "/" hint)
├──────────────┬─────────────────────────────────────────────────────┤
│ Appearance   │  WINDOWS                                     ▸ group │  left rail: 220 px, surface.background,
│ Displays   ● │  ───────────────────────────────────────────────────│  icon+label rows, ghost hover,
│ Input        │  Focus                    follow mouse  [Click ▾]   │  active row ghost_element.selected;
│ Shortcuts    │  Raise on click               on        ( ●)        │  "●" = pane has a modified value
│ Windows      │  Raise on hover              off        ( ○)        │
│ Notifications│  ── Snapping ─────────────────────────────────────── │
│ Idle & Lock  │  Edge snap                    on        ( ●)        │
│ Session      │  Snap distance      [────●────] 8 px                │
│              │  Corner size        [───●─────] 64 px               │
│              │  …                                             ▲    │
├──────────────┴─────────────────────────────────────────────────────┤
│ gen 12 · all changes applied · ● 1 restarts at next login          │  status_bar
└──────────────────────────────────────────────────────────────────────┘
```

- Single window, default 960×680, min 780×520, opaque (`window_options.opaque = true` — no
  per-pixel alpha needed until panel blur exists for apps), server-side decorations on.
- Left rail: the pane list; the modified-value dot is the `warning` token at 6 px. Rail
  rows are `list_item`-shaped (icon 14 px + label); roving-tabindex navigation, arrows +
  typeahead, one tab stop.
- Search: `Ctrl+F`/`/` focuses; subsequence match (Zed palette rules) over pane, group,
  label, key and enum words; results render as rows grouped by pane; Enter opens the pane
  scrolled and focused on the row; Esc clears. The search field is the only control that
  takes `/` globally — it is a settings app.
- Status bar (existing `status_bar` widget): config generation + apply state, health level
  dot (from `Subscribe`), count of badges ("1 restarts at next login"), and the pending
  spinner equivalent — a 6 px pulsing dot, `reduced_motion` makes it static.

### 5.3 The settings row (the atom)

One row = one `key_desc`. Structure, straight from the engine's vocabulary:

```
row:  div().flex_row().h(row_h).px(sp::b12)
        label block  — label (text, 14) + description (text.muted, 12, optional)   [grow 1]
        value block  — the control, right-aligned, fixed 220 px column             [no grow]
      hover: ghost_element.hover; modified: 4 px dot (warning token) before label;
      pending: control at 60 % opacity + dot (info); error: description line swaps to
               the diagnostic (error token) and the row keeps focus.
group: header (label, text.muted, small, uppercase) + rows + divider().inset()
       between groups — no group boxes, no cards.
pane:  scroll_area(vertical) of groups; ⇧? no — Page/arrow keys scroll; Home/End.
```

`row_h` is 32 px standard, 40 px when the row carries a description — both from the
existing spacing scale. The 220 px value column keeps controls vertically aligned down a
whole pane, which is most of what makes a settings app read as designed rather than
assembled.

### 5.4 Control mapping — `value_kind` to widget

| `value_kind` / shape | Control | Notes |
|---|---|---|
| boolean | `switch` | existing |
| integer/number **with range** | **slider** + numeric readout | click readout → stepper for exact entry; arrow keys ± step; Home/End to bounds |
| integer/number without range (or range > 64 steps) | **stepper** | `text_input` (numeric) −/+ buttons, validated |
| enumeration ≤ 3 words | **segmented** | e.g. focus `Click / Sloppy`, tiling `Floating / Master-stack` |
| enumeration > 3 words | **select** | popover list with the checkmark on the active word; e.g. scroll-method, notification position |
| string (validated) | `text_input` | xkb fields validate live via `check_keymap()` (`parse.cpp:976-1006`); paths validate for existence |
| string_list | **token list editor** | workspace names: rows of removable chips + add field; cap 16 × 32 B enforced |
| color/`background` spec | **swatch row** + `text_input` | presets (lumen, #hex set, gradient pairs, image path) + live preview strip |
| binding | `key_binding` + capture modal | §5.5 Shortcuts |
| theme | `select` + live re-theme | the whole app is the preview — changing `theme` calls `set_active_theme` immediately |

The five new controls (slider, segmented, select, stepper, swatch row, plus the token list
as a composition) are built once in `lx.ui:components` with `control_node` primitives —
quads, one border, existing hover/focus mechanics — styled exactly like switch/checkbox so
the set reads as one family (§5.7 of ADR-010 is their spec source). Selects use the
scrollable `list` inside the popover, not `menu`, so mode lists of any length work under the
64-entry menu cap. One overlay is open at a time (16-overlay cap is never a factor).

### 5.5 The panes

| Pane | Groups and rows (schema keys in brackets) | Special mechanics |
|---|---|---|
| **Appearance** | Theme & density `[ui] theme/appearance/density/ui-font-size` · Background `[background] spec` + preview strip · Panel `[panel] blur` · Cursor `[cursor] theme/size` (theme select from installed themes, size stepper 0..256; empty values fall back to `XCURSOR_*`, shown as placeholder) | Live: the app and the shell re-theme on apply; Zed-theme JSON import: pick file → `theme_compiler::compile` → `~/.config/lumen/themes/<name>.lxtheme` → `theme_watcher` makes it live; compile report shown (fallbacks listed, `theme_compile.cppm:83-102`) |
| **Displays** | One group per head (from `ListOutputs` + wlr heads): enable · mode (select, grouped W×H@Hz) · scale (fractional slider 0.25..4) · transform (segmented) · adaptive sync (switch) · position (steppers + minimap) · per-head advanced `[output] render-scale` | test → apply → **15 s revert modal**; minimap = quads with head rectangles, drag to rearrange (v2, steppers first); "Identify" button → `IdentifyOutputs()` |
| **Input** | Keyboard `[input.keyboard]` layout/variant/model/options/rules (live `check_keymap` validation) · repeat-rate/delay sliders + **test field** (a real `text_input`, the best preview there is) · Touchpad / Mouse / Trackpoint groups (field masks respected: a mouse group shows exactly `k_mouse_fields`) · Per-device overrides `[[input.device]]`: device select from `ListInputDevices` + "add override" | Unsupported-per-class keys never render (masks, `schema.cppm:44-50`) |
| **Shortcuts** | Search + groups by action owner (Windows, Tiling, Workspaces, Switcher, Shell, Session) — rows from `actions()`; each: summary, keycaps, options gear | Edit → modal with **in-window capture** (wl_keyboard keysyms; Esc cancels, Backspace clears — `grab_key` is shell-only, §2.3); conflict check against reserved keys (`keys.cppm:336-348`) and live bindings, inline error with "override anyway"; binding options toggles (`allow-when-inhibited`, `allow-when-locked`, `repeat`); per-row and global reset (`[general] default-bindings`); unbound actions listed with "add" |
| **Windows & Workspaces** | Focus `[focus]` · Snapping `[windows]` (distance/corner sliders with a live mini-preview drawn in quads) · Tiling `[tiling]` (layout segmented, ratio slider, gaps stepper) · Workspaces `[workspaces]` (mode segmented; count stepper and names editor enabled only in static mode — disabled rows render muted, not hidden) · Switcher `[switcher]` scope (segmented workspace/all) · include-minimized · open-delay slider | `[animations]` rides here too: enabled switch, stiffness/damping sliders, "preview" row animates a sample window only when `enabled` and `!reduced_motion`; the three `[switcher]` rows are the only `next_open` keys — badge "next time the switcher opens" (§4.4) |
| **Notifications** | Position (select + minimap of the 5 anchors) · timeouts (normal/low steppers, critical "until dismissed") · max-visible stepper · dnd-while-sharing switch · history size · **Do not disturb** switch → `RunAction(notifications.toggle-dnd)` | DND is shell state (shell-state file); v1 toggles without readback, labeled honestly |
| **Idle & Lock** | `[idle]` policy segmented · blank-after / blank-locked-after sliders with humanized labels ("10 minutes", "never") · lock-on-blank switch · lock-command path input (existence-validated) | CLI flags that override these (`shell_cli_setting`) shown as a notice when present |
| **Session & Health** | Version card (`GetVersion`: version, zlm version, backend) · Health parts (rows with `status_kind` icon + level, live 2 Hz) · Memory (progress bars: RSS, staging vs budget, GPU regions from DRM fdinfo) · Per-output speed (presented / missed_recent) · **Diagnostics log** (ring of config generations with their errors/warnings, from `config` events) · Actions: Reload, Logout (confirm modal) · Read-only display of `compositor::config` start flags (`LUMEN_LOG` level, present backend, target fps) | This pane is `lumen-ctl status/health/monitor` with a face; the equivalent CLI is shown as copyable text |

Pane order is the rail order above: appearance-first (most-visited), session-last.

The section→pane matrix is **total** over the schema: every section in `k_sections`
(`schema.cppm:489-494`) and every non-config surface of §2.2 maps to exactly one pane above
(the top-level `version` integer is a file-format marker, not a setting, and renders
nowhere). Phase B's gate keeps it that way mechanically — its test walks `schema_keys()` and
fails on any descriptor no pane claims.

### 5.6 Feedback, errors, and the pending state

- Pending: row value dims to 60 % + info dot, cleared by the `config` event for the
  generation the edit produced.
- Rejected edit: the diagnostic appears in the row (error token), a toast summarizes,
  the row re-renders the effective value. Never a dead end: the diagnostic names
  file/line/key exactly as `lumen-ctl reload` does.
- External change: toast(info) "Configuration reloaded (generation N)"; rows re-render.
- Destructive actions (Logout, revert-timer expiry, clearing all window rules) use the
  existing `modal` with explicit buttons — the only modals in the app besides shortcut
  capture and the display revert timer.

### 5.7 Accessibility and i18n

Every new control ships with role, name, value and states (`lx.a11y`: add `slider`,
`radio_group` for segmented/select, `spin_button` for stepper; the bridge's publish path
already walks the tree). Keyboard paths per pane are the design, not an afterthought:
rail → rows → control is three tab stops per pane; lists arrow-navigate. Strings are raw
UTF-8 through the engine's full pipeline (bidi, fallback faces); no message catalog exists
in the engine yet and none is invented here — pane strings are compile-time literals, and a
catalog is an engine-wide decision, not this app's.

### 5.8 GPU/UI-engine mechanics (why this is a GPU UI design)

The app inherits the engine's renderer wholesale — the primitive stream, instanced quads,
atlas glyphs, damage — and the design is shaped to its invariants:

- **Idle at zero**: a settings window nobody touches requests no frame callback (G1).
  The 2 Hz health subscribe updates state, but only *changes* invalidate paint — a steady
  health stream with equal levels repaints nothing.
- **Damage discipline**: dragging a slider damages one row (the ledger merges to one rect);
  scrolling damages the viewport; the theme switch is the only whole-window repaint.
- **Zero steady-frame allocation** (G5): rows describe from `key_desc` spans (no per-frame
  string building beyond `frame_format` value text in the arena); panes are reducer trees —
  one reducer per pane, `bind(host, action)` handlers, only the dirty pane re-describes.
- **Capacity budget**: the widest pane (Shortcuts, ~48 actions × rows ≈ 120 rows) fits the
  virtualised list pool (512 rows) with the whole pane in one `scroll_area`; mode selects
  use scrollable popover lists (menu's 64-entry cap never met); overlays: one at a time;
  timers: tooltips + revert modal + capture, well under 32.
- **Preview surfaces are primitives, not images**: the snap-distance diagram, the display
  minimap, the notification anchors, the background gradient strip — all quads/gradients/
  borders from `draw_scope`; no raster assets, no icon sprawl. Lucide set covers the rail
  (monitor, keyboard, bell, sun/moon, settings, power, refresh-cw…).
- **Tier freedom**: automatic backend selection means the app runs on the same host range
  as the shell (Vulkan → GL → CPU); nothing in the design assumes a specific tier.

## 6. Realization plan

Phases compile and test green independently, in order; each is shippable behind the state
of the previous. Sizes are one focused developer, an ordering aid (ADR-010's convention).

### Phase A — Engine controls (≈1.5 weeks)
`lx.ui:components`: `slider`, `segmented`, `select` (scrollable popover list), `stepper`,
`swatch_row`; `lx.a11y` roles for them; golden images in `lumen-ui-gallery`
(`tests/ui_gallery.cpp`) for dark/light; unit tests beside `test_ui_components.cpp`.
**Gate:** goldens pass on cpu/gl tiers; a11y tree assertions; a bench row confirms a
slider-drag frame allocates nothing.

### Phase B — Settings foundation, read-only (≈2 weeks)
`executables/lumen-settings/` (`main.cpp` + `settings-ui` module library — the
`lumen-hello`/`lumen-hello-ui` pattern, `CMakeLists.txt:310-320`); rail + search + status
bar; the row/group/pane framework rendering **every** `schema_keys()` + `pointer_keys()`
entry; Varlink client glue on the worker executor (the `lumen-ctl` pattern, async);
`Subscribe` plumbing. **Gate:** a test drives the pane tree (the `test_app_window` method)
and asserts every schema key is reachable through search; app opens on the host's automatic
tier.

### Phase C — The write path (≈1.5 weeks)
`lx.config:edit` splice editor + round-trip property tests (over `deploy/lumen/config.toml`
as the adversarial corpus: comments, odd ordering, unknown keys); pending/diagnostic/revert
flow; per-key and "reset all in section"; `config` event integration. **Gate:** property
tests green; live host round trip — edit to generation bump ≤ 1 s; injected bad-layer
rejection reverts the row with its diagnostic.

### Phase D — Core panes (≈2 weeks)
Appearance (incl. `[ui]` schema, compositor env export, shell push, theme import via
`theme_compiler`), Windows & Workspaces, Notifications, Idle & Lock; humanized value
formatting. **Gate:** all §5.5 rows wired; on-host theme change re-themes app + shell
without restart; schema addition of `[ui]` documented in `deploy/lumen/config.toml`.

### Phase E — Shortcuts (≈1.5 weeks)
Capture modal, conflict/override flow, binding options, resets, unbound-action discovery.
**Gate:** every `actions()` entry bindable; reserved-key enforcement mirrors `parse.cpp`
(the same `lx.config:keys` tables, not a copy); conflict test matrix.

### Phase F — Displays (≈2 weeks)
wlr output-manager client helper promoted from the probe test; per-head panes; test/apply;
15 s revert modal; render-scale rows; `IdentifyOutputs` compositor overlay. **Gate:** apply
round trip persists `outputs.toml` (observed on disk); a refused test never applies
(ADR-008 C5 error surfaced); lid-closed/headless smoke.

### Phase G — Input, window rules, polish (≈2.5 weeks)
`ListInputDevices` + Input pane; window-rules Varlink methods + compositor store + pane
(implementing the rules engine the inert protocol promised); `org.lumen.Settings.desktop`
entry; user-guide section; full a11y pass. **Gate:** fresh-session smoke list (launch from
the launcher, change one key per pane, sign out via the app once); docs table §7 complete.

**Sequencing note:** Phases B and A can interleave; C only needs A's controls where rows
become writable; D–G are independent of each other after C, ordered by user value.

### Not in v1 (restated): audio, networking, users/locale/time, theme editor, autostart,
default apps, per-app permissions — each needs a subsystem that does not exist; none gets a
stub.

## 7. Documentation and manifest updates (with the implementing change)

| Change | Update |
|---|---|
| New executable + module library | `CMakeLists.txt` (the `lumen-hello` pattern + `lumen_check_executable_layers`), `README.md` build list, `AGENTS.md` §6 map |
| `lx.config:edit`, `[ui]` section | `deploy/lumen/config.toml` example, `docs/user-guide.md` §Configuration, `lumen-ctl config defaults` regenerates via the same `default_config.inc.in` flow |
| Varlink additions | `control.cpp` IDL, this ADR's §4.8 table mirrored in `docs/architecture.md` §Control socket, `lumen-ctl` commands |
| New controls | `docs/api/README.md`, `docs/subsystems/ui-architecture.md` component list, gallery goldens |
| Desktop entry | `deploy/applications/`, install rules, launcher docs |
| Status flip | This ADR: Proposed → Accepted with per-phase "as built" notes (the ADR-010 convention) |

## 8. Risks

| Risk | Answer |
|---|---|
| Schema/UI drift | Impossible by construction: the UI *is* `schema_keys()`. A key without a descriptor doesn't render; that is a lint, not a failure |
| Splice editor corrupts a user file | Byte-preserving outside the edit + mandatory re-parse assertion + atomic rename; property tests over a hostile corpus |
| Varlink absent (older libsystemd, `--no-control`) | Panes degrade to file editing with a notice; nothing crashes, nothing lies |
| Two writers (app + text editor) | Atomic rename means last whole-file write wins; generation events reconcile; per-key conflict is the user's visible diff, not silent loss |
| Capacity ceilings on exotic panes | Budgeted in §5.8; virtualised lists and scrollable popovers keep every pane under every `k_*` cap |
| Scope creep toward a "control center" | §4.9 says no; every new pane needs a subsystem that exists and a schema that describes it |
