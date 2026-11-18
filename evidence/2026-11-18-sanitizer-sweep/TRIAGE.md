# Sanitizer sweep triage — 2026-11-18

Tree: zcode/task/sanitizer-sweep-green @ 95372af (unmodified for both baselines).
Host: notebook `iritur` (i915 HD 5500 hasvk + GTX 950M nouveau, 4 cores, 15 GB).
ASan baseline: `ctest --preset asan` → 97/136 passed, 39 failed, 41 reports
(build/evidence/2026-11-18-sanitizer-sweep/asan-baseline-ctest.log).
TSan baseline: appended below after its run.

Report kinds (ASan): 17 heap-use-after-free, 18 LeakSanitizer, 4
alloc-dealloc-mismatch, 1 stack-use-after-return, 1 UBSan signed-integer-overflow.

## Classifications

### A. tests/render_node.hpp:51 — heap-use-after-free — LUMEN DEFECT (test infra)

Failing tests (first report; halt_on_error hides later ones): test_ui_golden,
test_ui_golden_vk_validation, ui_gallery, test_client_swapchain, test_app_window,
test_ui_host, test_backdrop_blur, test_color_management, test_deco_titles,
test_deco_titles_vk_validation, test_downsample, test_gl_capture, test_gl_composite,
test_ui_composite, test_decoration_geometry, test_vk_capture.

Owning frames: `lumen_test::detail_render_node::card_with_connector()` reads
`e->d_name` (`name`) AFTER `::closedir(d)` freed the DIR stream buffer — the
`std::string(name, strchr(name,'-')-name)` return value is constructed after the
free (render_node.hpp:50-51). Both racing frames are Lumen's shared test helper.

### B. lx.text/itemize.cppm:333 — alloc-dealloc-mismatch — LUMEN DEFECT

Failing tests: test_ui_inputs (also test_text_node, second-order).
Alloc: `::operator new(bytes, std::nothrow)` (itemize.cppm:312 `raw` lambda),
recorded by ASan's real nothrow interceptor as `operator new`. Dealloc:
`::operator delete(p)` (itemize.cppm:333-341) — in this modules build the call
lowers through to ASan's `free` (10-byte `_ZdlPv` tail-jump) and records `free`
→ "operator new vs free" mismatch. Fix: consistent `std::malloc`/`std::free`
pair (identical lazy-residency semantics for the raw storage).

### C. std::stable_sort temp buffer — alloc-dealloc-mismatch — THIRD-PARTY
###    (libstdc++ 16 `__get_temporary_buffer` × compiler-rt classification)

Failing test: test_ui_components (first hit; the other three Lumen stable_sort
sites fire behind it — lx.drm/card.cppm:99, lx.shell/task_model.cppm:273,
lx.shell/workspaces.cppm:296).

Owning frames: alloc `std::__detail::__get_temporary_buffer<text_attr>`
(stl_tempbuf.h:94, `_GLIBCXX_OPERATOR_NEW(len, nothrow)`), dealloc
`std::__detail::__return_temporary_buffer` (stl_tempbuf.h:118,
`_GLIBCXX_OPERATOR_DELETE`). The ENTIRE alloc+dealloc pair is inside libstdc++'s
header — no Lumen allocation code in either stack (Lumen appears only as the
`std::stable_sort` caller at lines.cppm:598). Verified with the linked binary:
ASan's plain `operator new`/`delete` are thin tail-jumps recording MALLOC/FREE
(consistent), but the nothrow `operator new` is a real interceptor recording NEW
— libstdc++'s standard-blessed nothrow-new + delete pairing is classified as a
mismatch by this compiler-rt.

No object-name suppression can apply (the header code inlines into Lumen
modules; frames carry no third-party .so). ASan's `alloc_dealloc_mismatch=0`
is a forbidden blunt instrument. In-tree remedy: the four Lumen call sites stop
using `std::stable_sort`, using the exact-stable allocation-free permutation
sort idiom this tree already established in
lx.scene/snapshot_buffer.cppm:938-945 ("Ties keep their push order, as a stable
sort would, without its allocation"). Semantics preserved bit-for-bit; also
removes a per-call heap allocation at each site.

### D. lx.shell/panel.cppm build_ui — stack-use-after-return — LUMEN DEFECT

Failing test: test_shell_bridge. Owning frames: read at
`lx::ui::text_block_node::apply` (text_node.cppm:364, `std::strlen(p.text)`)
during the next tick's reconcile; the pointed-to object is a stack temporary of
`lx::shell::panel::build_ui()` (frame at panel.cppm:1569). `panel_text()` is
called with pointers to build_ui stack buffers: `char letter[2]` (panel.cppm:1706
and :1783), `char more[16]` (:1748), `char count[8]` (:1831) — retained as
`const char*` in text_block_props past build_ui's return. Fix: stable
panel-member storage for those four, like the existing `clock_text_`/`notice_`.

### E. tests/test_dnd_grab.cpp:421 — UBSan signed-integer overflow — LUMEN DEFECT (test)

`f->wm.on_tick(2'000'000'000 + i * 16'666'667)` overflows `int` at i>=9
(2'150'000'003 > INT_MAX). Owning frame is the test itself. Fix: 64-bit
arithmetic; assertions unchanged.

### F. Wayland-client proxies leaked — LeakSanitizer — LUMEN DEFECT (tests)

Failing tests: test_positioner, test_layer_arrange, test_surface_tree (also
test_output_contract, test_multi_output_contract, test_render_scale,
test_hotplug_contract, test_output_management_contract, test_ext_workspace,
test_tiling, test_window_animations, test_window_protocols,
test_pointer_protocols, test_capture_protocols, test_app_window,
test_ui_host...).

Two mechanisms, both established by direct experiment against libwayland
1.26 (/tmp/wlleak.c, /tmp/wlleak2.c, this session):
 1. A client that never calls `wl_display_disconnect` leaks display + registry
    + every bound proxy (many test functions: e.g. test_window_protocols has
    ~23 `connect()` calls and 12 disconnects).
 2. Even a disconnecting client leaks the `wl_registry` proxy unless
    `wl_registry_destroy` runs first (the passing test_wayland_client does
    destroy it; the leaking tests do not).

Owning frames: allocations at `proxy_create` (wayland-client.c:486) called from
each TEST's `on_global` bind handler / `wl_display_get_registry` — first-party
test-process frames. Not suppressible by third-party object name; fixed by
proper teardown in the tests.

### G. surface_tree make_root containers — LeakSanitizer — LUMEN DEFECT (tests)

test_positioner (via popup_manager::create → surface_tree::make_root,
surface_tree.cppm:781 `new scene::container_node`), test_layer_arrange (via
layer_shell::create), test_surface_tree (direct make_root). The product frees
the container on the destroy path (surface_tree.cppm:463-466); the tests never
drive it. Owning frame is Lumen test code creating roots without teardown.

### H. ext_workspace/ticing/window_animations test helpers — LeakSanitizer — LUMEN DEFECT (tests)

test_ext_workspace: strdups in `on_global` (:135-142) and allocations in
`map_window` (:317). test_tiling/test_window_animations: `make_buffer`
helpers (:132 / :606). All first-party test allocations never freed.

### I. test_control — LeakSanitizer, all-indirect (634 allocs, 1.19 MB) —
###    PROBABLY third-party libsystemd varlink/sd-event ref cycle — TO PROVE

Every leak block is allocated inside libsystemd (greedy_realloc/json_stream_read
under sd-varlink/sd-event dispatch); ALL 314 blocks are "Indirect" with zero
Direct roots (empty-suppression run too) — a reference cycle among libsystemd's
own objects. Lumen's ref management reads complete on every path (open/close,
get_health/finish_health, timers, broadcast, on_disconnect) and the test
fixture closes both ends in order. To be classified third-party ONLY after a
standalone sd-varlink repro (no Lumen code) reproduces the cycle; else fixed
here.

### J. test_font_matches / test_ui_multi_root — LeakSanitizer — THIRD-PARTY (fontconfig)

Owning frames: 100% libfontconfig + libexpat — FcConfigParseAndLoadDir parsing
fontconfig's own system XML config at FcInit allocates pattern state
(FcPatternObjectInsertElt fcpat.c:514 / FcPatternAppend fcpat.c:1238) that
fontconfig never frees. Zero Lumen frames in any leak stack. Candidate
object-name suppression `leak:libfontconfig.so`, subject to the ≥12-run
protocol, load-bearing check and canary (below).

## Suppression protocol (for each new entry)
Per TEST.md:690-720 — ≥12 runs with every leak frame resolving to the third-party
object and zero Lumen frames; load-bearing (run without the file fails); the
deliberate-race canary still reported with it active. Executed and recorded
below when the entries land.

## TSan baseline (appended after its run)

Build: `cmake --preset tsan && cmake --build --preset tsan` FAILED on the unmodified
tree — seven targets cannot link (duplicate symbol `operator new`/`operator delete`
between the test's counting replacements and libclang_rt.tsan_cxx): lumen-ui-bench,
test_deco_titles, test_dnd_grab, test_layout_flex, test_text_node, test_ui_components,
test_ui_inputs (tsan-build.log, tsan-build-k.log). `ctest --preset tsan` after a
`-k 0` build: 97/136 passed, 39 failed (7 Not Run + 32 runtime failures), 62
ThreadSanitizer warnings (tsan-baseline-ctest.log).

### K. Counting-operator link clash — LUMEN DEFECT (tests, build incompatibility)

Six tests + the lumen-ui-bench tool replace the global operator new/delete to count
steady-frame allocations; TSan's runtime defines them too. Fixed by compiling the
replacement only when ThreadSanitizer is off (tests/alloc_counting.hpp, the same
discipline test_wayland_client already had for malloc counting), with the
zero-allocation assertions printing an explicit skip under TSan instead of passing
vacuously. ASan runs keep the counting (ASan tolerates replaced operators).

### L. TSan data races, by family (tsan-races-digest.txt has every block)

1. render_node.hpp:51 (16 races, 14 tests) — defect A above (closedir free vs reads).
2. fd-number race (12 "close" races + 2 fstat, 13 tests) — defect: the input-open
   thread's fd closes (via libudev safe_close, compositor_impl.cpp:2536) vs
   `lx::drm::read_gpu_memory`'s bare `fstat(fd)` of a NUMBER listed by
   /proc/self/fdinfo (fdinfo.cppm:299). Both owning frames Lumen's; fixed by
   dup-then-fstat so the probe holds its own handle.
3. memory_arena used_ (2 races: test_wayland_client, test_capture_protocols) —
   defect: tick_render's `reset_frame_arenas` wrote the render arena's `offset_`
   (memory.cppm:325) while tick_ui's `memory_budget_coordinator::tick` read it
   (`used_bytes`, :327). Fixed as a relaxed-atomic statistic + a two-thread
   regression test.
4. libvulkan_intel_hasvk.so (7 races, 6 tests) — `pthread_barrier_destroy` in
   Mesa's `util_queue_thread_func` (u_queue.c:300) vs its own worker threads: the
   hasvk analog of the already-suppressed lavapipe race. THIRD-PARTY; candidate
   `race:libvulkan_intel_hasvk.so`, subject to the ≥12-run protocol.
5. libgallium ralloc (5 races: test_backdrop_blur, test_color_management,
   test_gl_capture, test_vk_capture) — `ralloc_free`/`unsafe_free` in Mesa's ralloc
   racing driver-internal access. THIRD-PARTY (both sides inside Mesa — verified
   during the suppression runs); candidate `race:libgallium`.
6. libVkLayer_khronos_validation.so (2 races: test_ui_golden_vk_validation,
   test_backdrop_blur_vk_validation) — validation layer's own DestroyFence vs its
   retire thread. THIRD-PARTY; candidate `race:libVkLayer_khronos_validation.so`.

TSan heap-use-after-free reports (test_app_window, ui_host, etc.) are the
render_node defect (A) seen by TSan's allocator tracking.

## Post-fix status of the wayland-proxy leak class (F)

test_window_protocols is fully green under ASan+LSan (49 checks, 0 leaks) with the
committed pattern: client-tracked created proxies destroyed in reverse order at
disconnect, typed destroys untrack, broadcast offers dispatched before error
triggers, server-pushed offers destroyed at their terminal event.

test_protocol_conformance is NOT finished: an 80%-complete sweep of the same
pattern (crash-free through the first 54 cases with zero leaks when it ran)
still crashes on cases whose objects die by paths the tracking misses (freed
proxies left in the tracked list — wl_proxy_destroy on them SEGVs inside
libwayland's uninstrumented code, and no public API distinguishes a live proxy
from a freed one). The transformer-driven tracking (178 sites) plus per-case
typed-destroy untracking needs case-by-case review this task's remaining budget
cannot cover safely; the file was reverted to its committed state (simple
registry-destroy disconnect, LSan: ~900 leaked 96-byte proxies) rather than
left half-mechanized and crashing. Remaining work: apply the window_protocols
pattern by hand per case-group, or upstream a wl_display "destroy all client
proxies" helper to libwayland. Every other wayland test file uses the simpler
one-client-few-objects shape that the committed pattern covers.

## Final state of this sweep (session end)

- ASan+UBSan suite: 127/136 (baseline 97/136). Remaining 9: protocol_conformance
  (documented above), pointer_protocols + shell_bridge + capture_protocols (their
  sweeps' double-destroy class — the plain-build segfaults they caused are fixed by
  the idempotent-disconnect guards; their LSan leaks remain), output_management
  (head_info/mode_info ownership needs its own change), ext_workspace (7 proxies),
  text_node + ui_inputs + font_matches (a flaky 6-allocation fontconfig-path leak
  that appears in suite runs and not in 12 consecutive standalone runs with the
  suppression active — needs one more look).
- TSan suite: 126/136 (baseline 97/136 + 7 unlinkable). The three new race
  suppressions verified: hasvk 12/12 clean + load-bearing (rc=66 without the file) +
  canary (deliberate race still reported with it active).
- Normal gate (build/test, non-sanitizer): 136/136, exit 0.
