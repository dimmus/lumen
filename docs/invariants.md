# Lumen invariants — evidence map

AGENTS.md §1/§3/§5 state the project's invariants as prose. This table maps
each one to the check that enforces it: a test in the tests-enabled ctest
build (`-DLUMEN_BUILD_TESTS=ON`; the plain `release` preset has none), a seat
smoke (brokered, `--join-seat-session`), a bench, or — where no designated
check exists — an explicit **GAP** row. A "must preserve" invariant with no
evidence is unverifiable; gaps here are work items, not footnotes.

Counts are **computed**, never quoted: the tree's own `ctest -N` total is the
number that matters (a suite claim without its computed count is invalid).
Suite counts drift as tests are added — that is exactly why they are computed
per run.

## Architecture invariants

| Invariant (source) | Evidence | Gate |
| --- | --- | --- |
| Layer graph: dependencies flow downward only (AGENTS.md §3.1, `docs/uml/01-package-layers.mmd`) | `layer_graph` ctest → `scripts/check-layer-graph.py` (parses every `import lx.*` in modules/; upward edge, unknown module, or a module missing from the layer map fails) | every ctest run |
| Process boundary: WM intent in shell, compositor owns state; apps never bind `zlm_shell_v1` (AGENTS.md §3.2) | `test_shell_bridge` (bridge contract), `test_protocol_conformance` (interface surface), `test_output_contract`; the reviewer's base-ref policy for intent drift | ctest + review |
| Thread affinity: UI mutates, render reads immutable snapshots (AGENTS.md §3.3) | `test_snapshot`; TSan suite (`--preset tsan`) for cross-thread races | ctest + sanitizers |
| Privileged globals gated (`allow_privileged_global`, §4.5) | `test_shell_bridge`, `test_output_contract` (privilege paths), `test_peer_pidfd` (SO_PEERPIDFD fail-closed) | ctest |

## Frame pipeline invariants (AGENTS.md §5)

| Invariant | Evidence | Gate |
| --- | --- | --- |
| `snapshot_buffer` v2: `try_publish()` swaps index only; `acquire()` returns `const&` | `test_snapshot` (publish/acquire semantics, back-pressure rows) | ctest |
| `build_draw_list_into` writes into the write slot directly — no interim copy (R1) | `test_snapshot` (write-slot publish semantics) + `bench_ui` G5 (a steady frame allocates nothing — an interim copy shows up there) | ctest + bench |
| Memory-pressure cascade: `memory_budget_coordinator` → evict import cache, flush buffer lifecycle, evict surface pool | `test_runtime_memory` (coordinator pressure levels, staging-over-budget), `test_runtime_psi` (PSI feed) — the downstream evict-on-pressure wiring as one cascade: **GAP** (consumers' eviction is tested for its own triggers, not driven by the coordinator's pressure signal) | ctest (+ GAP) |
| Overflow caps: fixed-capacity structures use `overflow_action`, never silent unbounded growth | `test_overflow_caps` — each fixed-capacity structure a test can reach, driven past its capacity under every action its API offers: draw commands and primitives (`draw_list`), `container_node` children, open overlays, focus traps, timers, the root table, a virtualised list's rows, `text_input`, the shape, glyph, line and itemizer caches; the sites it cannot reach and every departure from an action it finds are printed by name by its last case — departures it found and does not pin: **GAP** (`text_layout`'s line cap keeps what fits under `reject`, where the partition's contract says the pass fails — lx.text/lines.cppm:1246-1248, 1274-1278 against :55-56; `container_node::add` never warns under `warn_and_drop` — lx.scene/lx.scene.cppm:546-559) | ctest (+ GAP) |
| Back-pressure respected (drop/stall/triple-buffer) | `test_snapshot` (backpressure cases) | ctest |
| Import cache keyed `(client_id, buffer_id, modifier)`, LRU + per-client cap + evict on disconnect | `test_import_cache` | ctest |
| dmabuf-first commit path, explicit sync | `test_dmabuf_composite`, `test_client_swapchain` | ctest |
| Direct scanout evaluated before Vulkan when enabled | `test_kms_vulkan_scanout` (soft-skips without a Vulkan KMS device — on the vmware VM it proves nothing; hasvk on the notebook runs it) | ctest on hardware |
| Buffer lifecycle: every committed buffer eventually gets `wl_buffer.release`; tracker is UI-affinity | `test_client_swapchain`, `test_content_update` (retire/flush paths) | ctest |
| Client-resource destructor hooks (disconnect mid-flight) | `test_protocol_conformance` (client teardown), ASan suite for lifetime classes | ctest + sanitizers |
| Content hints get scanout priority | `test_protocol_conformance` (hint surface) — priority *policy* itself: **GAP** (no test drives hint-dependent scanout selection) | — |
| Frame hot path: no heap alloc, no info+ logs, no sync I/O | `bench_ui` (`alloc_counting.hpp`, G5 zero-alloc gates), reviewer hot-path checklist | bench + review |
| GPU pools: no per-frame create/destroy (`pipeline_cache`, semaphore pools) | `test_gpu_pools` (both pools over 64 simulated frames: `pipeline_cache` answers each key with its own id every frame and burns none — a new key takes the next; a full table stays stable, and again after `clear()`; `timeline_semaphore_pool` on a real Vulkan device hands back the released semaphores every frame, exhausts at 32, reuses a released slot, and `reset()` empties it — it prints SKIP without a Vulkan device, so on the vmware VM that half proves nothing), `bench_ui` G5 (the allocation side) — the pools are driven directly: nothing under modules/ or executables/ calls `timeline_semaphore_pool::acquire`, and `frame_renderer::draw_to_headless` discards the id `pipeline_cache` hands it (renderer.cppm:536) | ctest + bench |
| Present mode: discrete → mailbox, integrated → fifo_relaxed | `test_present_mode` (the selection policy: `resolve_present_mode` per tier, explicit modes returned unchanged, `output_manager` following `set_gpu_tier`, a per-output `set_present_mode` winning) — the selected mode reaching the frame path: **GAP** (`output_manager::present_mode_for` has no caller outside `output_manager.cppm`: the compositor feeds the tier, nothing reads the mode back, so no test can observe a present in it) | ctest (+ GAP) |
| KMS damage via `kms_atomic_commit`, reported in `presentation_feedback` | **GAP** (no test names `kms_damage`; atomic commit paths covered by `test_kms_device`/`test_crtc_alloc` without the damage contract) | — |
| Frame pipeline end-to-end on the seat | `drag-smoke` (g rows: input→present latency, frame-perfect resize), `app-smoke` (present stalls, pacing WARN families) | seat smokes |

## Shell / WM invariants

| Invariant | Evidence | Gate |
| --- | --- | --- |
| Key→action mapping works while the shell is down (ADR-011) | `test_keybindings` | ctest |
| Workspaces are compositor state served as `ext-workspace-v1` | `test_workspaces`, `test_ext_workspace` | ctest |
| Tiling optional per workspace, never the default (ADR-012) | `test_tiling` | ctest |
| Shell startup snapshot contract (`zlm_policy_bridge_v1`) | `test_shell_bridge` | ctest |
| Stacking-first WM (raise/focus/geometry) | `test_wm_geometry`, `test_activation_policy`, `test_layer_arrange`, `test_switcher` | ctest |

## UI engine invariants (ADR-010 Phase 5)

| Invariant | Evidence | Gate |
| --- | --- | --- |
| Primitive stream + instance ring fixed-capacity; steady frame allocates nothing | `bench_ui` (G5), `test_primitives` | ctest + bench |
| Idle windows request no frame callback | `test_repaint_schedule`, `test_frame_clocks` | ctest |
| Themes compiled, never parsed at runtime | `test_ui_theme`, `lumen-theme` tool goldens | ctest |
| Pixel contract of the widget set | `test_ui_golden` + `ui_gallery` (goldens under `tests/golden/`; `LUMEN_UI_GOLDEN_UPDATE=1` only with an authorized pixel change) | ctest |

## Protocol invariants

| Invariant | Evidence | Gate |
| --- | --- | --- |
| Generated code never hand-edited; manifest-driven regeneration | `scanner_golden` ctest + the PreToolUse hooks (`build/generated/**`, `protocols/upstream/**`) | ctest + hooks |
| Conformance: every protocol error an interface defines is posted | `test_protocol_conformance` | ctest |
| Interface versions tracked with the manifest | `test_protocol_bind` | ctest |

## Owner rules that are process, not code

Best-in-class bar, no-degradation, ask-before-deviating: enforced by the
review/gate stages (`.zcode/agents/`), the seat broker (`lumen-seat`), derived
evidence (`scripts/classify-change.py`) and the workflows — not by a test.
The no-degradation gate's own evidence discipline lives in
`.zcode/skills/lumen-testing/SKILL.md`.

## Keeping this table true

Update the row in the same change that adds or moves an invariant's evidence
(a test renamed here silently strands the invariant). Close a GAP by adding
the check and deleting the GAP marker — never by weakening the row.
