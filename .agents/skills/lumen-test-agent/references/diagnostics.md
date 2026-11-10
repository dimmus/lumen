# Targeted diagnostics catalog

This is a broad selection guide for Lumen's Linux/C++26/Wayland/Vulkan stack, not a claim that every tool is installed or every conceivable tool is enumerated. Prefer repository tests and low-overhead observations, then one tool that answers the question. Confirm installed-version CLI with `--help`. Preserve raw output, symbols/build IDs and overhead metadata. Installing tools does not grant kernel, GPU or seat access.

## Select by question

| Question | Tools | Evidence / limits |
|---|---|---|
| UAF, overflow, UB, leaks | Clang ASan+UBSan preset; LSan; llvm-symbolizer | Reproducing stack with allocation/free sites; repository suppressions only; external drivers may be uninstrumented |
| Data race / lock lifetime | TSan preset; optionally Helgrind/DRD | TSan cannot combine with ASan; third-party reports need access-stack attribution; custom sync needs understanding |
| Uninitialized reads | MemorySanitizer (MSan); Valgrind Memcheck `--track-origins=yes` | MSan is experimental here: entire dependency chain including standard library must be instrumented; stock Mesa/system libs prevent trustworthy blanket claims |
| Allocation count / steady-frame churn | heaptrack + heaptrack_print/GUI; existing counting-new and mallinfo2 benchmark gates; optional memray is Python-only | Separate Mesa/LLVM/xkb startup from Lumen steady frames; heaptrack sees CPU allocations, not GPU heap residency |
| Heap growth / retained memory | heaptrack, Massif, DHAT, `/proc/PID/smaps_rollup`, pmap, cgroup memory.stat/events | RSS is not a leak verdict; distinguish mappings/cache/driver/GPU memory; collect repeated workload/idle cycles |
| CPU hotspots / cache / branches | perf stat/record/report/annotate, hotspot, FlameGraph, Samply (supported platforms), LLVM XRay (custom instrumentation) | Kernel/PMU permissions, frame pointers or DWARF unwinding; sample losses and multiplexing; no profiler timing acceptance |
| Wall time / startup | `/usr/bin/time -v`, hyperfine with explicit warmups/preparation; repository ui-bench | Startup benchmark differs from idle or frame latency; hyperfine shell/synchronization overhead matters |
| Scheduler stalls / wakeups / locks | Perfetto kernel scheduling, perf sched/timechart/lock (kernel support), trace-cmd/KernelShark, ftrace, bpftrace/BCC offcputime/runqlat | tracefs/debugfs/BTF/events and privileges vary; scope PID/cgroup, cap duration/buffer; host-wide data may contain unrelated activity |
| Syscalls / fd lifecycle / IPC blocking | strace, lsof, `/proc/PID/fd`, pidstat, ss; optional rr for deterministic replay | ptrace permission; rr CPU/perf restrictions, Vulkan/GPU/device replay limits; tracing perturbs timing |
| Crash / hang | gdb, coredumpctl, eu-stack/eu-addr2line, llvm-symbolizer; gdb thread backtraces | Exact binary, shared objects, debug symbols and build IDs; core files may contain private client data; save owned target only |
| GPU API correctness / sync | Vulkan validation + sync validation; GPU-assisted validation (opt-in supported devices), spirv-val, spirv-dis | Validation variants must actually exist/run; validation changes overhead and does not prove race freedom in CPU code |
| GPU frame content / pipeline | RenderDoc, gfxreconstruct, Vulkan API dump layer, apitrace for EGL/GLES | Injection/capture layers can conflict with each other and sanitizer runs; dmabuf/external sync/WSI replay may fail; no GPU timing claim from replay alone |
| GPU utilization / memory / driver | vulkaninfo, drm_info, modetest, eglinfo, `/proc/PID/fdinfo`, intel_gpu_top, amdgpu_top/radeontop, nvidia-smi, nvtop; Nsight Graphics/Systems, Radeon GPU Profiler, Intel GPA (vendor-specific) | Match actual vendor/driver and device; nvidia tools don't establish NVK support; modetest modesetting can seize display, use listing only unless assigned |
| Present / damage / input latency | Lumen timeline, ui-bench e2e, vkmark-bench, presentation-feedback and smoke logs, GPU timestamp queries | Join CLOCK_MONOTONIC injection→frame→flip; refresh rate/output scaling, queue/fence/display split; CPU submit time is not GPU execution time |
| Input / protocol contract | wev, wayland-info, WAYLAND_DEBUG=client, libinput debug-events/record/replay, evtest, evemu, uinput tools, repository probes | Input tools may expose keystrokes or inject live events; assigned seat only; keep both client and compositor evidence |
| Desktop services | busctl, gdbus, dbus-monitor, systemctl --user, journalctl, loginctl, wpctl/pw-cli/pw-top, portal probes | Scope D-Bus names/units and scratch state; user bus/session setup absent in ordinary Docker; preserve environment restoration |
| Resource pressure / I/O / power | pidstat/iostat/vmstat/mpstat, PSI `/proc/pressure`, systemd-cgtop, cgroup v2, iotop, turbostat, powertop, sensors | No host-wide stress/kill or permanent governor changes by default; `stress-ng`/fio only bounded scratch resources in isolated assigned environment |
| Fuzz and coverage | libFuzzer, llvm-profdata/llvm-cov, sancov, optional AFL++ (separate integration), honggfuzz | Seed real protocol handshake; coverage must reach modules; crashes reproducible; foreign fuzzers are not existing repository harnesses |
| Static/build diagnostics | clang-tidy/Clang Static Analyzer/scan-build, clang-format check, include module dependency tooling, ShellCheck, Python compile checks, CMake --graphviz, Ninja -t commands/compdb, clang -ftime-trace | C++26 module tooling support varies; compile_commands.json uses exact generated module options; cppcheck/IWYU may not understand this modules build |
| Security hardening audit | readelf/llvm-readobj, checksec, sanitizers' CFI/Scudo/HWASan where supported | CFI needs compatible LTO/visibility; HWASan/MTE architecture and runtime restrictions; not existing Lumen presets |

## Sanitizers and suppressions

Use `cmake --preset asan` (address,undefined) and `tsan`; detailed commands are in test-matrix.md. Set `LLVM_SYMBOLIZER_PATH` to the matching discovered llvm-symbolizer when needed. Keep frame pointers and symbols; reproduce at the same optimization level before concluding a debug-only result generalizes. UBSan defaults can recover and print reports with exit 0: inspect stderr; for a targeted strict run merge `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` into existing environment without dropping repository suppression settings. Do not invent `LUMEN_SANITIZER=ubsan`.

TEST.md's rule: suppressions only by **third-party shared-object name**, never broad symbol names or Lumen frames. Inspect current `tests/sanitizers/*.supp` and `LUMEN_SANITIZER_ENV*` in CMake. TSan's Mesa lavapipe access-stack evidence and LSan's Mesa EGL/gallium suppression are narrowly scoped. Vulkan ICD leak-check disable applies only to tests currently opted in via CMake properties; TEST.md's historical two-test list is not a complete current list. Never export `detect_leaks=0` globally. CTest is preferable to direct launches because it applies these properties. Investigating a suppression means rerunning the exact case with/without it and collecting stacks, not introducing a deliberate race into production code.

MSan, HWASan, CFI, Scudo and standalone UBSan require isolated build configurations, compiler/runtime/dependency compatibility validation, and explicit task justification. Do not silently replace required ASan/TSan gates with them. No ASan/TSan, Memcheck or heaptrack combination in the same process by default.

## Perfetto and Lumen's native timeline

Lumen already exports **Chrome trace-event JSON**, readable by Perfetto. This is not the Perfetto SDK's TrackEvent protobuf producer. Read `modules/lx.trace/lx.trace.cppm` and callers of `timeline_start_from_env` / `timeline_flush` to confirm the chosen executable starts and flushes recording; setting an env var on an arbitrary test may do nothing. The native timeline uses CLOCK_MONOTONIC, preallocated event storage, existing `LUMEN_TRACE_FILE` and `LUMEN_TRACE_EVENTS` (default 2,000,000). It flushes after recording threads stop; crash/SIGKILL can lose output. Multiple processes must have distinct output paths. Check overflow/drop counters and trace integrity; a full buffer is incomplete coverage.

For a traced headless run, first discover the executable's supported private-socket/virtual-output options from source/help. Assign `LUMEN_TRACE_FILE="$ART/compositor.json"` only to the compositor, start the bounded workload, and use its graceful shutdown so `timeline_flush` runs. A simpler verified timeline-enabled executable can be used if the requested boundary does not need a compositor. Validate JSON, nonempty events, thread IDs/names, timestamp range and required scopes before claiming capture success.

Open locally in [Perfetto UI](https://ui.perfetto.dev) or use a separately verified offline UI when data must stay offline; do not publish traces automatically. With a discovered trace-processor executable (`trace_processor` or `trace_processor_shell`), check its version's query CLI. For versions supporting `-q` as a SQL file:

```sql
SELECT name, COUNT(*) AS samples, ROUND(AVG(dur) / 1000.0, 2) AS mean_us,
       ROUND(MAX(dur) / 1000.0, 2) AS max_us
FROM slice WHERE dur >= 0 GROUP BY name ORDER BY max_us DESC LIMIT 30;
```

Save SQL and CSV output beside trace; Perfetto SQL stores durations in nanoseconds even though Chrome JSON timestamps use microseconds. Inspect `stats` for parse/data loss and actual schema before version-specific queries. Mean/max do not substitute for p50/p95 frame gates.

For kernel scheduling add a **separate** Perfetto system capture using a checked-in-to-artifacts text config and the installed version's supported ftrace datasource/events (`sched_switch`, `sched_waking`, process/thread metadata). Bound buffers/duration, check tracefs access and producer readiness first; use `perfetto --txt -c "$ART/capture.pbtxt" -o "$ART/system.pftrace"` only after validating options. Installing `perfetto` does not guarantee traced/traced_probes services or kernel datasource availability. Correlate timestamps using known clock domains/clock snapshots; two separately opened files are not automatically merged/aligned, and JSON cannot just be concatenated with protobuf. Keep application-only trace evidence explicit when system capture is unavailable.

Official reference: [Perfetto recording](https://perfetto.dev/docs/quickstart/linux-tracing), [trace processor](https://perfetto.dev/docs/analysis/trace-processor), [Chrome JSON format](https://perfetto.dev/docs/getting-started/other-formats).

## CPU, allocation and crash recipes

Examples use `BIN` as the discovered absolute test/bench executable; supply its verified workload arguments. `ART` is unique and absolute. These are **diagnostic** runs on an assigned target, separate from performance acceptance.

```bash
# Read-only capability metadata; do not change sysctls to make it pass.
cat /proc/sys/kernel/perf_event_paranoid /proc/sys/kernel/yama/ptrace_scope
perf stat -r 5 -e task-clock,context-switches,page-faults -- "$BIN"
perf record -k mono -g --call-graph dwarf -o "$ART/perf.data" -- "$BIN"
perf report --stdio -i "$ART/perf.data" > "$ART/perf-report.txt"
heaptrack -o "$ART/heaptrack" "$BIN"
# Discover the actual generated compressed filename, then:
heaptrack_print <generated-file> > "$ART/heaptrack.txt"
valgrind --tool=memcheck --leak-check=full --track-origins=yes --error-exitcode=99 --log-file="$ART/memcheck.log" "$BIN"
valgrind --tool=massif --massif-out-file="$ART/massif.out" "$BIN"
ms_print "$ART/massif.out" > "$ART/massif.txt"
strace -ff -ttt -T -o "$ART/strace" -- "$BIN"
gdb -batch -ex run -ex 'thread apply all bt full' --args "$BIN" > "$ART/gdb.txt" 2>&1
```

`perf -k mono` aligns native timeline's clock; verify installed perf support, call-graph quality and lost samples. A denied event means unavailable evidence, not zero cost. heaptrack should launch the owned target (ptrace_scope may prevent attachment); normal exit must finish the report. Heaptrack records startup as well as workload: use stacks/phases or repeat bounded workloads to isolate steady-state allocations. Follow allocation gates for authoritative frame-path counts. Valgrind may reject new instructions, Vulkan or module-toolchain artifacts; report unsupported execution, not a clean memory result. GDB can exit 0 after the inferior crashes: inspect inferior termination. Apply `timeout` and ownership-aware cleanup to potentially hanging tools. Attaching gdb stops threads and alters deadlines.

For crash dumps prefer an owned program's known core via coredumpctl after recording PID/time/build ID; never harvest all users' cores. `thread apply all bt full`, shared-library mappings and faulting instruction often suffice for handoff. Do not enable global core dumps or relax Yama without explicit separate authorization.

References: [Clang sanitizers](https://clang.llvm.org/docs/UsersManual.html#controlling-code-generation), [heaptrack](https://github.com/KDE/heaptrack), [Valgrind manuals](https://valgrind.org/docs/manual/manual.html), [perf wiki](https://perfwiki.github.io/main/), [RenderDoc](https://renderdoc.org/docs/), [Vulkan validation](https://vulkan.lunarg.com/doc/sdk/latest/linux/khronos_validation_layer.html).

## Coverage and GPU checks

For coverage, use an isolated `build/coverage`, `-fprofile-instr-generate -fcoverage-mapping` on C/C++ and profile runtime link flags. Confirm propagation to C++ module objects and executable links in actual commands; rebuild modules rather than reuse incompatible BMI/cache artifacts. Set `LLVM_PROFILE_FILE="$ART/%m-%p.profraw"`, run selected tests, merge with matching `llvm-profdata merge -sparse`, and use `llvm-cov report/show` with the test binaries plus needed `-object` inputs. Handle source path mapping, missing/duplicate profiles, branch coverage and failed tests. There is no existing Lumen coverage switch; do not invent one or claim coverage from an empty profile. [LLVM coverage reference](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html).

For Vulkan identify driver/device with vulkaninfo and render-node discovery first. Prefer existing `*_vk_validation` CTest variants (their CMake properties supply Lumen's validation environment). Explicit process-only sync-validation settings from build/run are `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`; verify layer availability, emitted validation messages and current SDK support. Classify driver validation failures using an exact reproducer. RenderDoc/gfxreconstruct capture only the owned workload and preserve replay limitations for external memory, multi-process clients and real KMS. Never equate software-renderer pixel correctness with hardware Vulkan or physical scanout correctness.
