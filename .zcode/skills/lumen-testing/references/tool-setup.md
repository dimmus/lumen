<!-- Ported 2026-11-19 from the removed DSH archive (.agents/skills/lumen-test-agent/references/) — the deep catalog still applies; environment text (hosts, Docker mentions, script paths) is superseded by .zcode/skills/lumen-testing/SKILL.md and lumen-build-run, and the DSH helper scripts it names (test_tools.py, capture_run.py, validate_helpers.py) were removed with the archive — their command lines are historical. -->
# Development tool setup

Use the existing `scripts/install-deps.sh` for Lumen's toolchain, libraries, sanitizer runtimes, smoke clients, Vulkan layers and common development tools. Do not duplicate its distribution/toolchain logic. It supports Arch, Debian/Ubuntu, Fedora and openSUSE; older releases may lack the required C++26 standard library. Inspect its current plan and checks:

```bash
bash scripts/install-deps.sh dev --dry-run --no-fetch
bash scripts/install-deps.sh dev --yes --no-fetch
bash scripts/install-deps.sh dev --check
```

Installation mutates the selected development system, may need sudo/root and network access, and must finish successfully. The user requesting development setup authorizes needed packages; follow runtime restrictions and record the transaction. Plan mode does not prove availability. Check warnings even when exit is zero: missing optional smoke clients/layers still reduce coverage. Fetch protocols separately when needed. Never select `--nouveau-atomic` as an incidental setup step: it writes boot configuration and rebuilds initramfs.

## Diagnostic helper

The DSH `test_tools.py` helper was removed with the archive and was not ported —
the command lines that used to live here are gone. What survives is its group
inventory (useful as a checklist when a task needs a diagnostic that is not yet
verified present — check with `command -v`, and an absent tool is reported as
absent, never installed):

Groups: core (gdb/strace/heaptrack/sysstat/time/lsof), memory (heaptrack/Valgrind), cpu (perf/sysstat/time/hyperfine), tracing (Perfetto/trace processor/trace-cmd/bpftrace), gpu (vulkaninfo/RenderDoc/drm_info), input (wayland-info/wev/evtest), scripts (ShellCheck). Many distributions do not package Perfetto or its processor in the same package; repository availability is checked at install time, never assumed. Ubuntu perf packages must match the running kernel. Report an absent tool and use another available diagnostic, not a mismatched wrapper as evidence of support.

(The DSH-era "Docker on this Windows host" section that followed here was dropped
with the port — containers are ruled out for this project, see
`.zcode/skills/lumen-build-run/SKILL.md`.)

For a chosen container, use `docker exec --workdir <actual-source-path> <name> python3 ...` and capture its real exit code. A container based on old Debian can validate Python/shell helpers while being unable to build Lumen. Separate those claims. Start no replacement graphical session.

## Tools outside the helper

Use configured distribution repositories first. Discover package providers (`apt-cache search/policy`, `pacman -Ss/-Si`, `dnf provides`, `zypper search`) and installed tool versions. Match LLVM tools/compiler-rt to Clang; runtime objects ship with compiler packages, not an `asan` CLI. Vendor GPU diagnostics and specialized tools (Nsight, Radeon GPU Profiler, Intel GPA, gfxreconstruct, hotspot, KernelShark, BCC, rr, AFL++, SPIR-V tools) have platform, license or dependency limits; consult official current docs for the selected target rather than bulk installing them.

For Perfetto/trace processor not packaged in configured repos, use the official [Perfetto installation/build instructions](https://perfetto.dev/docs/quickstart/trace-analysis) or [source build](https://perfetto.dev/docs/contributing/build-instructions). Choose an explicit release/architecture, verify published checksum/signature when available, retain URL/version/hash, and install into a task/user tool directory with explicit PATH. If no published checksum exists, record that provenance limitation; do not invent verification. Do not pipe a network response directly into a shell or silently execute a self-downloading launcher. An offline Perfetto UI requires its own verified distribution. Native Lumen JSON can still be collected without installing a recorder; analyzing system scheduling needs the separate supported capture path.

Runtime diagnosis before escalation: `perf_event_paranoid`, Yama ptrace_scope, tracefs/BTF availability, kernel version, capabilities/seccomp, render-node permissions, VK ICD/layer manifests, XDG runtime directory, D-Bus/user systemd/logind, input nodes and VKMS. Read settings; do not disable seccomp, add all capabilities, make device nodes world-writable, change host sysctls or reboot to pass a test. Any separately authorized temporary capability change needs precise scope and restoration evidence. Missing hardware/permissions/services remain unsupported or could-not-run, never PASS.
