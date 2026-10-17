> **Status:** Accepted. Part 1 (multi-output: every connected monitor composited and
> flipped on its own), Part 2 (hotplug: monitors connected, disconnected and resized while
> Lumen runs) and Part 3 (output configuration: `zwlr_output_manager_v1` — wlr-randr,
> kanshi — with a real TEST_ONLY, output transforms, adaptive sync, and `outputs.toml`
> restored at connect) implemented.

# ADR-008: Real monitors

**Date:** 2026-10-17
**Scope:** Part 1: `lx.drm` (`connector_scan` probe, `:edid`, `:crtc_alloc`, `kms_pipeline`
per CRTC, `commit_modeset`, device-level flip dispatch), `lx.scene` (output views),
`lx.scheduler` (`frame_clocks`), `lx.wayland.server` (`add_global_ex`, `remove_global`,
`collect_globals`), `lx.compositor` (`:output`, per-output pipelines and clocks in
`compositor_impl`, `wire_output.cpp`, presentation and frame callbacks per pace output,
the window manager's per-output areas, placement and fullscreen, per-output layer shell,
display power and lock), `lx.shell` (one panel per output, launcher output),
`lumen-compositor` (`--output-scale`, `--virtual-outputs`).
Part 2: `lx.drm` (`:uevent` — the udev monitor; `probe_connectors` without recording, a
TEST_ONLY of another mode, disabling a removed pipeline, leaving the event registry),
`lx.compositor` (`:hotplug` — `hotplug_controller`; `:output_policy` — the scan diff, mode
choice and fallback order; output plans UI → render → UI in `compositor_impl`; window
migration and return in `:wm`; lock surfaces dropped with their output),
`lumen-probe` (lock surfaces for outputs added while locked), `tests/tools/lumen-vmw-layout`,
`scripts/monitor-smoke.sh`.
Part 3: `lx.drm` (`:modes` — CVT custom modes; VRR_ENABLED in modesets), `lx.gfx`
(`compose_output_transform`, `transform_rect`), `lx.scene` (output transform),
`lx.scheduler` (VRR frame clock), `lx.wayland.server` (the sandbox global filter),
`lx.compositor` (`:output_management` — the wire-free model; `wire_output_management.cpp`;
`:toml_lite`; `:output_config_store`; the apply/test jobs, the whole-configuration
TEST_ONLY and persistence in `compositor_impl`), `lumen-probe output-mgmt`,
`scripts/monitor-smoke.sh` (randr, mgmt-errors, kanshi, persist)
**Deciders:** project owner

---

## Context

Lumen drove exactly one output: the first connected connector, one CRTC, one snapshot
buffer, one frame clock, one `wl_output` global. Everything else about a monitor — a
second one, its scale, where windows and panels go, which display paces a client — had no
answer. The design (`design-real-monitors`, Parts A–G) covers multi-output, hotplug and
output configuration; this ADR records the decisions of Part 1 and the alternatives
rejected.

## Decisions (Part 1)

| # | Question | Decision |
|---|----------|----------|
| M1 | Identity | `output_id` is a per-session counter never reused; the **slot** (0–15) is the bit index for masks; the **name** is the connector; the persistent **identifier** is "make model serial" from the EDID (libdisplay-info, base-block fallback), `connector:NAME` without one, duplicates get ` connector:NAME` appended. |
| M2 | Layout | Logical coordinates, integer origins. Auto placement left to right by logical width (`wlr_output_layout_add_auto`); explicit positions stay; `suggested X/Y` on `hotplug_mode_update` connectors (VMs), as Mutter, unless it overlaps. |
| M3 | Snapshots | One snapshot buffer **per output view**. Damage is noted once in layout space and fanned out (projected, clipped) to every view it touches; only due outputs with damage build and publish. |
| M4 | Render | One render thread; per output a pipeline (CRTC, primary and cursor plane, three scanout slots, buffer-age damage, fence, flip state). Each published output is composited and committed in turn. |
| M5 | Commits | Frames: one nonblocking atomic commit per CRTC with its own flip event. Configuration (start, resume): TEST_ONLY, then **one** combined `ALLOW_MODESET` of every pipeline needing it, carrying each output's first frame. Events are routed by CRTC; a commit's cookie owes each pipeline it covers one event (a per-pipeline bit armed by the commit, consumed by that event, with the pipeline's flip generation), so separate modesets in flight at once on a shared cookie never take each other's events. Every modeset turns off the CRTCs no pipeline of ours drives that another master lit — read at open and again at each resume (fbcon lights every connected connector, a disabled monitor's included) — and unroutes the connectors the kernel has on them now. |
| M6 | Frame clocks | A `repaint_scheduler` per output over **one** shared timer armed at the earliest start; outputs due within 1 ms share a tick; each clock stops after its own quiet ticks; a clock never ticks twice per refresh period, even when the repaint estimate exceeds one (flat out). The 1 s housekeeping timer runs only while every clock is stopped. |
| M7 | CRTC/plane allocation | Pure backtracking search (`assign_crtcs`): current routing first, then the CRTC a previous master routed, then by index; a primary and an optional cursor plane per CRTC (planes bound to a CRTC stay with it); accepted only when a TEST_ONLY of the whole state passes. Bounded (16 connectors, 256 tests, 4096 steps). |
| M8 | Pacing | A surface's **pace output** is the output it overlaps most among those it was shown on (outputs with ≤ 10 % of its area count only alone; wlroots' `primary_output`). Frame callbacks and presentation feedback follow that output's flips; `sync_output` names it. Callbacks go out **at the flip event**, so a client has the whole period before that output's next repaint. |
| M9 | Input | The pointer is clamped to the union of the output rects (`closest_point`); absolute devices span the layout bounding box. |
| M10 | Placement | A toplevel maps on its parent's output, else the pointer's. Maximize and fullscreen use the window's output; a move is constrained by the pointer's output's usable area. |
| M11 | Fullscreen | `xdg_toplevel.set_fullscreen(output)` / zlm `request_fullscreen`: the window's tree moves to a fullscreen layer between top and overlay, sized to the output, over a black backdrop; unfullscreen restores the rect and maximized state. |
| M12 | Layer shell | Every layer surface belongs to one output (the requested one, else the pointer's); arrangement, exclusive zones and usable areas are per output. |
| M13 | Shell | lumen-shell: one panel per `wl_output`, each listing the windows on its output; the launcher opens on the focused window's output, else the last pointed panel's, else the first. |
| M14 | Lock | Lock state stays global; `locked` is sent once every live, lit output has flipped a locked frame (`lock_frame_tracker`). |
| M15 | Display power | One power machine and one gate bit per output; the paused tick takes over only when every output is dark; input wakes every output. |

## Decisions (Part 2)

| # | Question | Decision |
|---|----------|----------|
| H1 | Hotplug monitor | libudev `udev_monitor` on the "udev" netlink group, filtered in the kernel to subsystem `drm`, devtype `drm_minor`; an fd source on the UI loop. Only `change` with HOTPLUG=1 of our card's devnum acts (CONNECTOR= narrows the probe); `remove` of the card ends every output. |
| H2 | Probe | The forced connector probe (`drmModeGetConnector`, EDID over DDC on real monitors) runs on the **worker**, on the master fd, into the worker's own scan; one in flight plus one merged rerun; UI diffs the result. Nothing changed: nothing happens. Never while the VT is away (only the master's probe is forced); the resume probes everything first and waits for it ≤ 250 ms. |
| H3 | Mode follow | An output with no explicit mode follows its connector's preferred mode: a new preferred mode (the VMware window resize) is TEST_ONLY'd with a real framebuffer of that size, then switched to and modeset; refused, the other modes in sway's order. An explicit mode is kept while offered. Bad link-status: a modeset of the same mode. |
| H4 | `wl_output` lifecycle | The global appears once the output's pipeline is up (a monitor whose every mode fails its test never appears); removal: membership first (`leave`), then `wl_global_remove`, objects inert, `wl_global_destroy` 5 s later; a bind racing it gets an inert object. A slot is reused only after its pipeline was retired. The last output going is replaced by HEADLESS-1 before its global goes. |
| H5 | Migration | Windows of a removed output move to the pointer's output (else the first in layout order) keeping their relative position in its usable area; maximized and fullscreen ones are re-configured. Each remembers its home (identifier and rect) and returns when that output comes back, unless the user moved or resized it since. When the output going is the last live one and another is on its way (a configuration enabling one output and disabling the only other, a dock's saved setup), no stand-in flashes up: its windows are **stranded** and the next output that comes up takes them in the same way (HEADLESS-1 does, if that add fails). Layer surfaces are `closed`, lock surfaces dropped. |

## Decisions (Part 3)

| # | Question | Decision |
|---|----------|----------|
| C1 | Output management | `zwlr_output_manager_v1` v4: a head per connected monitor (enabled or not) and per virtual output; modes by tuple (a changed or gone tuple is finished, a new object introduced — the protocol cannot take "preferred" back); a custom mode in use is one more mode object (wlroots' virtual mode). **Every** protocol error is raised, `already_set` and `unconfigured_head` included (wlroots and niri skip them; wlr-randr 0.5 and kanshi 1.9 never trip them). At apply/test: `already_used`, then the serial (stale → `cancelled`, not an error), then `unconfigured_head`. Every head disabled → `failed`. Restating the current state (kanshi at login) → `succeeded`, nothing done. |
| C2 | Test and apply | One job at a time, four queued. A whole-configuration TEST_ONLY on render: changed pipelines with real framebuffers of their new mode (kept for the switch), disabled ones off, newly enabled ones through the CRTC allocator with the same test (`test` ends there — wlr-randr `--dryrun` is real). Apply: enables first, then modes (switched at the first frame of the new size; adaptive sync changed with a mode goes into that switch's modeset, which answers for both), adaptive sync, transform, scale, position, disables last — in UI's bookkeeping. On render the disables run first: retires move ahead of the batch's adds, and a KMS add waits (≤ 1 s) while a retiring CRTC's last flip drains, so the enable's CRTC allocation and TEST_ONLY see the outputs being disabled already off, as the configuration's test did. `succeeded` once each changed output's modeset went through (its ticket), with the heads and `done` sent before it. A job's handle on its configuration object is a number never reused, so a destroyed object's answer cannot reach a new one at the same address. A modeset refused after its test passed reverts to the previous state and answers `failed`. Position and scale changes need no modeset. |
| C3 | Persistence | `$XDG_CONFIG_HOME/lumen/outputs.toml` (`LUMEN_OUTPUT_CONFIG`: another file, or `0` for none), a strict TOML subset. Per monitor (identifier): mode or nothing (preferred), scale, transform, adaptive sync — only what differs from the defaults. Per setup (the sorted identifiers connected together): enabled and position. A VM monitor (`hotplug_mode_update`) keeps neither mode nor position (the host owns them, Mutter). Written 2 s after a client's apply, on the worker (write, fsync, rename); applied at start before the first modeset and whenever the set of connected monitors changes; a file that does not parse is renamed `.bad` at the next save. |
| C4 | kanshi | A layer on top: Lumen restores the last applied state of the setup, usually kanshi's own, so kanshi's apply at login changes nothing (no modeset, no save). |
| C5 | Privilege | Not privileged: any same-UID client (wlr-randr from a terminal, kanshi as a user service). Sandboxed (Flatpak: `/proc/<pid>/root/.flatpak-info`) clients do not see it — a global filter in `lx.wayland.server` (`deny_sandboxed`), also on output power, idle notification, session lock, layer shell, input method and virtual keyboard. |
| C6 | Transform | Emitters draw in the untransformed content space; each view turns every draw once (rect, clip, buffer transform composed with the output's: `R_buffer ∘ R_output⁻¹`) and turns damage; occlusion still runs on axis-aligned output pixels. The cursor plane's image and hotspot are turned per output when staged. 90° is counter-clockwise, as `wl_output.transform` says (the transform a buffer has when it can be scanned out as it is). |
| C7 | Custom modes | CVT 2.0: reduced blanking v2 on digital connectors, standard blanking on analog ones; libdisplay-info's `di_cvt_compute` when present, built-in formulas otherwise (equal for 320 size/refresh pairs, `test_cvt`). The kernel's TEST_ONLY judges. |
| C8 | Adaptive sync | `VRR_ENABLED` is stated by every modeset (on only when asked and `vrr_capable`; another master's left on is reset); a VRR output's frame clock repaints on demand, no sooner than one period after the last flip. A hotplugged output whose VRR is refused comes up without it. |

Ownership across threads: render owns the pipelines; UI holds a handle set by the add's
answer and cleared before it asks for the retire. UI never hands render that handle in a
task: render's copy of an output's projection is updated by slot, looked up on render, so a
task drained after the output's retire finds nothing. A retired pipeline leaves the KMS event
registry on render and is destroyed on UI, the thread that drains events. A mode switch is
tagged with the first frame of the new size; render applies it right before compositing
that frame and never composites an older one into the new targets (at a VT resume this
puts the new mode in the resume's one combined modeset).

## Alternatives rejected

- **One snapshot for all outputs**, cropped per output by render: couples back-pressure —
  a flip-pending output pins the slot every other output needs — and makes outputs with
  different refresh rates or phases pay for each other's frames.
- **One combined commit for every frame** (all CRTCs in one atomic commit): ties every
  output to the slowest and fails with EBUSY whenever any CRTC has a flip pending. wlroots,
  KWin, Mutter and niri all flip per CRTC; sway and KWin modeset everything together, as
  Lumen does.
- **A timer per output**: the same behaviour as one timer armed at the earliest start,
  with N timerfds and N wakeups when outputs are in phase.
- **Frame callbacks at the next tick** (as with one output): the tick comes at the repaint
  start, late in the period, and the client's reply misses that repaint — a frame every two
  periods (measured: 33 ms instead of 17 ms on this host).
- **First-fit CRTC assignment without TEST_ONLY**: shared resources (bandwidth, shared
  planes) are only judged reliably by the kernel.
- **Polling connectors per tick** (the old `hotplug_monitor`): a wakeup per frame for an
  event that comes a few times a day, and it never saw anything — the cached state it
  re-read is only refreshed by a forced probe.
- **sd-device** (`sd_device_monitor`): its external-event-loop API
  (`sd_device_monitor_receive`) is too new to depend on, and libudev is linked anyway.
  **The kernel's netlink group**: events before udevd ran its rules, parsed by hand, for no
  gain.
- **Probing on the UI thread**: EDID over DDC takes tens of milliseconds per connector on
  real monitors; Mutter probes in its KMS thread too.
- **Switching the logical state before the test**: clients would be told of a mode the
  kernel may refuse; the test with a real buffer comes first (sway's swapchain prepare).
- **sysfs `status` forcing for tests**: it raises no uevent, so a compositor never notices
  (§3 of the design); the vmwgfx layout ioctl does what the host does.
- **Answering `test` without testing** (niri) and **wlroots' leniency** on `already_set` /
  `unconfigured_head`: a `--dryrun` that cannot fail is no test, and the spec calls both
  protocol errors. If an important client trips them, the fallback is to log and ignore.
- **`succeeded` at the switch**: a modeset can still be refused after its test passed;
  the answer waits for the commit (bounded at 3 s) and a refusal reverts.
- **A shell-owned layout** (the shell storing and pushing positions): the compositor knows
  when monitors come and go and must restore before the first modeset; a client can only
  act after it, a second modeset.
- **`$XDG_STATE_HOME`** for `outputs.toml`: users edit and version it like sway's or
  KWin's configuration; it is configuration.
- **A transform in every emitter** (`project_node` turning rects): five emitters (surfaces,
  decorations, the cursor, the fullscreen backdrop, layer surfaces) would each compose
  buffer transforms; one pass over the draw list does it once.

## Consequences

- A second monitor costs only its own frames: an idle output publishes nothing and its
  clock stays stopped while another animates (measured: 7 context switches in 5 s with
  three idle outputs; a client on one output does not wake the others).
- `wl_output`, `zxdg_output_v1`, enter/leave, preferred buffer scale and fractional scale
  are per output; a window dragged across a boundary enters the new output, leaves the old
  one and gets the new scale.
- `lx.drm` no longer has a device-wide CRTC: callers ask `pipeline_desc_for(connector)`
  and own a `kms_pipeline`.
- Virtual outputs (`--virtual-outputs WxH[@S],…`) make multi-output protocol tests possible
  without hardware; no card is opened then.

- Hotplug (measured on vmwgfx): a uevent's probe takes 0.6–3 ms on the worker; an identical
  layout costs that probe and a diff, nothing else; an idle session is unchanged (≈ 1
  wakeup/s with two idle outputs). A VM window resize re-modesets its output once, at the
  new size, with the other outputs flipping on; a resize while on another VT is lit at the
  new size on return in the resume's one modeset.

- Output configuration (measured on vmwgfx): wlr-randr's mode change is one TEST_ONLY with
  a real buffer and one modeset of that output only; position, scale and transform changes
  repaint without a modeset; `--dryrun` of a mode the kernel refuses is `failed` and nothing
  on screen changes; kanshi's profile applies on hotplug and finds nothing to change after
  a restart; the restart itself is one combined modeset at the persisted state.
- An apply is not one atomic commit of the whole configuration (wlroots 0.18's
  `wlr_backend_commit`, KWin and Mutter commit every CRTC of a configuration together).
  Its steps run in an order whose intermediate states need no more than the tested final
  one for disables (they go first); a staying output's new mode is still switched at its
  first frame, after an enable, so hardware where the enable fits only next to that
  output's *new* mode can see an apply fail that its test passed (it is then reverted and
  answered `failed`, as the protocol asks). A single commit needs every enabled output's
  first frame composited before the commit — the start-up path's combined modeset does
  that; extending it to client applies is the next step.

## Host test method

vmwgfx exposes eight `Virtual-N` connectors with `hotplug_mode_update` and `suggested X/Y`.
A layout set through `DRM_IOCTL_VMW_UPDATE_LAYOUT` before the compositor starts gives two
or three real CRTCs with real flips. `scripts/inject-input.py --layout <compositor log>`
drives the VMware absolute mouse in layout coordinates over the outputs' bounding box.
Hotplug is driven the same way while Lumen runs: `lumen-vmw-layout` (a test tool, root:
the ioctl is allowed to DRM master or CAP_SYS_ADMIN, and works while Lumen holds master)
raises one HOTPLUG=1 uevent per layout, as open-vm-tools does when the host window or
topology changes. `scripts/monitor-smoke.sh` adds, resizes and removes monitors, switches
VTs, powers outputs off and locks, judges each step from the logs and screenshots, and
reports INVALID when vmtoolsd pushed the host's layout under it. Headless,
`compositor::simulate_connector_scan` feeds fake scans through the same path
(`test_hotplug_contract`).
