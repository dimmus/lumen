---
name: lumen-decisions
description: Lumen's settled technical decisions and measured outcomes that must not be re-litigated or re-measured — malloc replacement ruled out, umbriel/scenefx techniques kept/rejected, raise-the-bar items kept/reverted/abandoned, Vulkan 1.3 floor, kernel-features scheduling reverted, tiling model, render-scale filter, clipboard/DnD scope, Phase-5 shell-adoption decisions. Load before proposing optimizations, re-introducing reverted features, or designing anything these records already decided.
---

# Settled decisions & measured outcomes (do not re-litigate without new evidence)

Full-fidelity records: `.zcode/memories/lumen-*.md` (one file per item below, with numbers).
The user's standing rules behind all of these: **best-in-class bar** (compare wlroots/sway,
KWin, Mutter, Smithay/niri, Hyprland; measure, don't assert) and **no degradation**
(every change earns its place by measurement; the Vulkan backend is the gate).

## Ruled out by measurement

- **Malloc replacement** (2026-10-27): glibc vs tcmalloc/jemalloc/mimalloc under drag-smoke —
  identical latency within noise, +10–31 MiB anon RSS for the alternatives. The frame path is
  arena-based; ~100k allocations are startup (Mesa/xkbcommon). Don't propose one without a
  workload that allocates on the frame path.
- **GL program build-ahead on a helper thread** (G7 cold): measured and LOST (+12–20 ms) —
  the compositor is busy on the other core. G7's cold cost is the driver (~103 ms of EGL,
  Mesa compiles, fontconfig), Lumen's own code ≈5 ms. Don't retry.
- **SDR through an _SRGB view of scanout** (umbriel item 3): 8-bit storage between blends is
  off by up to 4 levels vs FP16. Do not re-propose.
- **Thread scheduling tiers** (EEVDF/RT, kernel-features): REVERTED — 500 µs EEVDF slices cut
  up the GL composite (cursor p95 25.5→35.2 ms under load). Kept reverted until measurable on
  hardware Vulkan.

## Kept / shipped (don't undo, don't re-derive)

- **umbriel/scenefx techniques** (PR #55): per-target-size persistent FP16 Vulkan
  intermediates; damage-limited composite+encode (cursor frame 123→1.7 ms on lavapipe);
  partial opaque regions + no-blend pipeline for opaque; clipping partly-covered draws to
  ≤4 rects (GL 25–36 % faster on overlap scenes). Neither project is usable as a dependency
  (wlroots/GLES2-only) — techniques only.
- **Raise-the-bar** (PR #63): KEPT RLIMIT_NOFILE raise, Vulkan GPU timestamps (debug-gated),
  `scripts/vkmark-bench.sh`, spring animations (`[animations] enabled=false` default),
  ext-background-effect-v1 blur (`[panel] blur=false` default), wp_color_manager_v1 (SDR
  outputs only). REVERTED after review: GL reset recovery (S2 — cursor lost after reset;
  was one fix round from good), rounded corners/shadows (F1 — radius 30–32 masked client top
  corners; GL zeroed arrays per draw; also one fix round from good). ABANDONED: shader highp
  (S4, nothing at risk), frozen test clock (S5, no user). Known accepted cost:
  gamma22/PQ-tagged surfaces ~2× on SVGA3D GL, 20–30× on CPU.
- **Vulkan 1.3 floor** (PR #57): 1.3 minimum, devices below filtered (→ automatic falls to
  GL), dynamic rendering + sync2 everywhere, no VkRenderPass/VkFramebuffer, shared
  `pipeline_cache` compositor→ui_vk, host_image_copy uploads, memory_budget-fed GPU pool,
  memory priority 1.0 on scanout targets. Hardware numbers are the user's (RADV/ANV); lavapipe
  caches nothing in VkPipelineCache. Push descriptors not done.
- **Kernel features** (b4550d3): `lx.runtime:psi` + psi_monitor, fence deadlines (vmwgfx
  accepts+ignores), SO_PEERPIDFD fail-closed, DRM fdinfo (vmwgfx prints none), CLOSEFB
  console restore done by Lumen itself. Merging main needed `drm_timeline_ops::query` callers
  updated — expect that class of semantic conflict.
- **Tiling** (ADR-012, PR #58): dwm-style master-stack, compositor-side, per workspace; all
  11 default layout bindings kept by user decision (they take their Super keys on floating
  workspaces too). Not built: pointer-drag split resize, orientations, monocle/dwindle.
  Never reintroduce a shell-side tiling plugin.
- **render_scale** (PR #70): HiDPI supersampling with **area filter kept on every downscaled
  draw by user decision** — its cost at non-integer ratios (GL ~4→10 ms fullscreen) is an
  accepted quality trade; do not re-raise as a regression.
- **PRIME offload** (PR #71): `PrefersNonDefaultGPU`/`X-KDE-RunOnDiscreteGpu` via
  switcheroo-control (fallback `DRI_PRIME=1`) into app systemd units; discrete/integrated
  launcher menu item. Notebook verification still pending.
- **Clipboard / DnD** (PRs #61, #62, aa25900): `lx.wayland.client:data` + `lx.app:clipboard`
  behind `lx::ui::clipboard`; primary selection, file (uri-list) drags, drag icon, ask action
  all shipped. Eager read of others' selections; per-process marker mime.
- **ADR-010 UI engine** (PRs #31–#48, #74–#77): GPUI-style primitive stream, six phases, one
  paradigm (Phase 5 deleted the retained-widget API; `shm_canvas` deleted; shell, portal,
  multi-window lx.app all on `lx.ui.host`). Decisions that were asked and answered: `lx.ui.host`
  layer-5 module; shell memory accepted up to G8 (25 MB anon); decorations on the lowering
  path extend `lower_to_blits` with coverage-texture blits (no deco atlas); §8.6 comparison
  may build egui from crates.io and pacman-install waybar/ironbar/gtk4-layer-shell/layer-shell-qt.
  Theme fallback table was rebuilt from MIT one.json because the first version took values
  from Zed's GPL crates — **keep Zed GPL sources out**.
- **Whole-path repaint lead** (2026-11-04, notebook): the frame-clock lead carries the
  measured thread cost AND the flip-fed whole-path lateness (`observe_commit_late`,
  aim ring) — a callback client holds 59.9 fps with a 12 ms median present. Don't widen
  `k_client_margin_ns` for handoff costs; the feedback absorbs them.
- **Font-match persistence** (PR #44): `face_cache::persist_matches`, first decorated tick_ui
  24.6 ms → 1.7–2.8 ms. Don't port the WIP's font part again.

## Open leads (details in `.zcode/TODO.md`)

Shell background render cache still only in unmerged WIP c8d4499; explicit sync on the wire
untested (no timeline syncobjs on vmwgfx); notebook checks pending; G2 p95 / G7 GL cold /
G11 frame rule open as host-limited.

## Gotchas

- `lx.shell.cppm` uses **CRLF** line endings — restore CRLF after any scripted/Python edit.
- Pushing needs SSH (`git push git@github.com:dimmus/lumen.git <branch>`); the https origin
  has no credential helper.
- `app-smoke` Z-groups fail if xdg-desktop-portal's frontend is broken — environmental,
  check a baseline before blaming a change.
