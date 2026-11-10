# Test selection and executable recipes

Read current TEST.md in full first. It includes detailed per-row assertions and host exceptions that this index does not duplicate. Read scripts' help/source before invocation: defaults, durations and group lists evolve. Paths in commands below assume repository root in Linux. Use a unique absolute artifact directory (`ART`) you created for this run.

## Build and automated gates

```bash
./scripts/install-deps.sh dev --check
./scripts/fetch-protocols.sh
cmake --list-presets
cmake --preset release
cmake --build --preset release
ctest --test-dir build/release --show-only=json-v1 > "$ART/tests.json"
ctest --test-dir build/release --print-labels
ctest --preset release --output-junit "$ART/release.xml" --no-tests=error
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-junit "$ART/asan.xml" --no-tests=error
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-junit "$ART/tsan.xml" --no-tests=error
```

Check each exit before the next dependent step. `asan` is **address+undefined**, not address alone. Release/asan/tsan test presets exclude the `bench` label. `bench` is a **test** preset using release, not a configure preset. Debug has configure/build presets but no test preset: use `ctest --test-dir build/debug`. No `ubsan`, `msan` or `coverage` preset exists. C++26 module builds need compatible Clang, clang-scan-deps and standard-library modules; the dependency installer checks this. CMake 4.4 test-preparation targets may build one closure: discover `test_prep/<name>` support before using it; otherwise build the named test or normal build. CTest does not generally build a missing binary.

For a targeted rerun use anchored names, an explicit timeout and nonempty selection:

```bash
cmake --build build/asan --target test_protocol_conformance
ctest --test-dir build/asan -R '^test_protocol_conformance$' --output-on-failure --no-tests=error --timeout 180 --output-junit "$ART/protocol.xml"
```

CTest applies sanitizer environments; launching binaries directly can omit suppressions and `LUMEN_CONFIG=0`. Inspect `ctest --show-only=json-v1` and CMake properties before reproducing directly. Check `Testing/Temporary/LastTest.log`, JUnit and test output for runtime soft-skips, missing validation variants, unsupported GPU paths and insufficient samples. Enumerating registered tests is not proof every backend ran.

## Target by boundary

These are selection candidates, not promises they exist in every configured build. Select exact discovered names. Follow TEST.md for required assertions and negative protocol cases.

| Area | Candidate tests and follow-up |
|---|---|
| Foundation/runtime/threading | test_result, test_snapshot, test_runtime_memory, test_runtime_psi, test_runtime_task, test_frame_clocks, test_repaint_schedule; ASan/UBSan and TSan; timeline/scheduler evidence |
| Buffers/surfaces/client lifetime | test_import_cache, test_surface_tree, test_shm_staging, test_dmabuf_composite, test_content_update, test_fence_deadline; disconnect mid-flight, fd/memory trend, Vulkan validation |
| Protocols and input | test_protocol_bind, test_protocol_conformance, test_wayland_client, test_pointer_protocols, test_pointer_constraints, test_input_serials, test_data_transfer, test_dnd_grab; seeded fuzz, selected app-smoke groups |
| WM/shell | test_positioner, test_layer_arrange, test_wm_geometry, test_keybindings, test_workspaces, test_ext_workspace, test_task_model, test_shell_bridge, test_activation_policy, test_liveness, test_tiling, test_window_animations |
| Config/session/IPC | test_config, test_config_edit, test_input_config, test_input_keymap, test_control, test_health, test_session_discovery, test_session_pause, test_lumen_session; R/config and session smoke |
| Outputs/KMS | test_output_contract, test_multi_output_contract, test_hotplug_contract, test_output_management_contract, test_output_membership, test_output_layout, test_render_scale, test_kms_device, test_kms_vkms, test_kms_vulkan_scanout; monitor/scale smoke |
| Pixels/UI/text | test_ui_golden, test_ui_golden_vk_validation, ui_gallery, test_headless_golden, test_ui_composite, test_gl_composite, test_cpu_composite, test_backdrop_blur*, test_color_management*, test_deco_titles*, test_decoration_geometry, test_text_*, test_layout_*, test_ui_*, test_settings_ui; exact pixels/defined tolerance, matching fonts/output/driver |
| Capture/portal/desktop | test_capture, test_gl_capture, test_vk_capture, test_capture_protocols, test_portal_compose, test_session_lock, test_idle_notify, test_notifications, test_tray; real PipeWire/D-Bus clients and applicable smoke |

Do not count tests by summing this table. Confirm actual test registrations and build options from current CMake. Do not run goldens' update modes during verification.

## Smoke scripts and seat limits

Run `bash scripts/<name>.sh --help` where supported, otherwise read its argument parser. Full-display scripts take DRM/input/VT and are reserved to the coordinator's assigned Linux seat. Container-only checks cannot substitute for them. Never hardcode archived event nodes, card numbers, cgroup session paths, UID or output geometry.

| Script | Purpose / example after seat assignment |
|---|---|
| tty-smoke.sh | startup, actual KMS/console restoration: `./scripts/tty-smoke.sh lumen-smoke` |
| input-smoke.sh | hardware/libinput/xkb probe first, then keystroke routing through wev: `./scripts/input-smoke.sh 30`; interactive human input required |
| drag-smoke.sh | unattended move/resize/focus/grab destruction, configure pacing and injector-to-present latency: `./scripts/drag-smoke.sh --join-seat-session` |
| app-smoke.sh | real clients, wire/client and compositor logs, pixel geometry: `./scripts/app-smoke.sh --join-seat-session --only R`; choose groups from **current source**, not just TEST.md's old bracket list |
| session-smoke.sh | scratch-prefix installation, systemd user session/env restore, READY/reload, shell restart, logout; optional `--watchdog` intentionally SIGSTOPs compositor |
| monitor-smoke.sh | multi-output hotplug, layout, workspace/snap/notification movement; `--only add,workspaces` from TEST.md, verify current options and hardware helper prerequisites |
| scale-smoke.sh | scale/resolution and rendering checks; discover current arguments and physical-output requirements |
| ui-bench.sh | stage/allocation gates; headless by default, `--client --short --backend cpu` covers client pipeline; `--e2e --join-seat-session` takes the seat; `--e2e --dry-run` validates recorded sample parsing only |
| vkmark-bench.sh | real client commit/release and fifo pacing; `--join-seat-session --modes bare,panel,shell --size 800x600` |

Supporting scripts: `inject-input.py` writes Linux input events, `uinput-mouse.py` creates an absolute device, `run-desktop.sh` launches sessions, `portal-session.sh` manages portal environment; these are stateful infrastructure, not standalone passing tests. Inspect cleanup and privileges first. `fetch-protocols.sh`, `protocol_manifest.py`, flex/theme fixture tools support builds/validation; don't regenerate tracked inputs as an incidental test.

App group index documented by TEST.md: A subsurfaces, B popups, C decorations, D layer-shell, E shell bridge, F clipboard/primary, G DnD, H output contracts, I liveness, J activation, X destruction ordering, R configuration, Q workspaces, O switching/snapping (also fractional `--scale 1.25`), N notifications/tray, T7 live blur. Additional current groups must be derived from source. Use targeted groups for diagnosis, full suite for required acceptance. Negative tests intentionally provoke protocol errors; whitelist only the exact cases/PIDs the suite expects.

For drag/app smoke: 0 PASS (may warn), 1 FAIL, 2 COULD NOT RUN, 3 INVALID from foreign input. ui-bench e2e also uses 3 INVALID; vkmark uses 0 completed, 1 failed, 2 could not run. Read each other script's own verdict semantics. A host pointer mixed with injection invalidates the run. Preserve logs in script-designated directories (drag-smoke-logs, app-smoke-logs, vkmark-bench-logs), then copy/link the exact run directory into the evidence report.

TEST.md notebook constraints: discover render node of connected connector (`tests/render_node.hpp`, existing `LUMEN_TEST_RENDER_NODE`; `none` forces no GPU); never force GL on an incompatible secondary NVIDIA card. Discover suitable absolute pointer/keyboard; multitouch-only touchpad is insufficient. Assigned notebook smoke uses `LUMEN_SMOKE_SCALE=1`; O's own `--scale` remains independent. Pixel rows calibrated to VM geometry/fonts need same-host baseline comparison, not dismissal or automatic recalibration. VKMS scanout requires available module, DRM master and `LUMEN_TEST_VKMS=1`; missing VKMS is uncovered. Archived SVGA3D fences, PSI, portal and geometry noise require fresh baseline evidence, not blanket exceptions.

## Fuzzing

```bash
cmake -B build/fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DLUMEN_BUILD_FUZZERS=ON
cmake --build build/fuzz
ctest --test-dir build/fuzz -R '^fuzz_wayland_dispatch_smoke$' --output-on-failure --no-tests=error
mkdir -p "$ART/corpus" "$ART/crashes"
# Binary seed names from the current corpus README; re-discover if changed.
cp tests/fuzz/corpus/{sync,get_registry,bind_compositor,create_surface} "$ART/corpus/"
./build/fuzz/fuzz_wayland_dispatch "$ART/corpus" -max_total_time=600 -timeout=10 -rss_limit_mb=4096 -artifact_prefix="$ART/crashes/"
```

Use a writable corpus initialized from repository binary seeds; the explicit copy excludes README.md (verify seed names against the current tree before a real campaign). Preserve original seeds. libFuzzer instruments the whole build through `LUMEN_BUILD_FUZZERS`; harness-only coverage is inadequate. Record exec/s, coverage growth, corpus size, seed, budget and crash artifacts. The 10-second CTest smoke is not a campaign. Reproduce saved crash with the same build, symbolize, minimize with libFuzzer's supported minimization flags, and report a confirmed compositor crash as release-blocking. Keep CPU/memory bounded; do not combine separate TSan and ASan builds.

## Performance

Run `ctest --preset bench --no-tests=error` separately and serialized. Current registrations include bench_frame, bench_composite, bench_ui_composite, bench_ui and bench_ui_client; discovery determines availability. `bench_composite 1920 1080 --blur --vk` and `--color --vk` target blur/colour Vulkan cost (omit --vk for the documented other path).

`ui-bench.sh --backend vulkan --no-build` and equivalent GL/CPU/none/client runs retain backend-specific metadata and results. Software Vulkan results do not establish a hardware baseline. Do not use `--update-baseline` without an explicitly reviewed baseline update. The script's FAIL is p50 >25%, WARN p50 >10% or p95 >25%, with absolute floor and allocation/batch/sample gates; these thresholds tolerate noise and never authorize a measured smaller regression. Fine changes require same-host/backend interleaved A/B, ≥3 rounds, fixed pin, warmup and ASLR policy, raw distributions. Do not compare sanitized/traced/profiler runs to uninstrumented release timing.
