# Development tool setup

Use the existing `scripts/install-deps.sh` for Lumen's toolchain, libraries, sanitizer runtimes, smoke clients, Vulkan layers and common development tools. Do not duplicate its distribution/toolchain logic. It supports Arch, Debian/Ubuntu, Fedora and openSUSE; older releases may lack the required C++26 standard library. Inspect its current plan and checks:

```bash
bash scripts/install-deps.sh dev --dry-run --no-fetch
bash scripts/install-deps.sh dev --yes --no-fetch
bash scripts/install-deps.sh dev --check
```

Installation mutates the selected development system, may need sudo/root and network access, and must finish successfully. The user requesting development setup authorizes needed packages; follow runtime restrictions and record the transaction. Plan mode does not prove availability. Check warnings even when exit is zero: missing optional smoke clients/layers still reduce coverage. Fetch protocols separately when needed. Never select `--nouveau-atomic` as an incidental setup step: it writes boot configuration and rebuilds initramfs.

## Diagnostic helper

`../scripts/test_tools.py` relative to this reference uses Python 3.10+ standard library. From repository root:

```bash
python3 .agents/skills/lumen-test-agent/scripts/test_tools.py --list
python3 .agents/skills/lumen-test-agent/scripts/test_tools.py --check --groups core memory
python3 .agents/skills/lumen-test-agent/scripts/test_tools.py --plan --groups cpu
python3 .agents/skills/lumen-test-agent/scripts/test_tools.py --install --groups cpu --yes
```

`--check` reports discovered executable paths as JSON, exits 1 if any is absent. It does not run tools or verify kernel capabilities. `--plan` previews missing tools and candidate repository packages without querying or installing packages. Non-Linux plan requires `--manager apt|pacman|dnf|zypper` and is only a packaging preview. `--install` is Linux-only, preflights every candidate against configured repositories, aborts **before installation** if any is unavailable, inherits package-manager output, preserves failures, then checks executable discovery. Exit 2 means setup/availability error; missing post-install commands return 1. Nothing automatically enables services or changes repos/kernel/security policy. `--yes` only skips the package manager's confirmation, not harness policy.

Groups: core (gdb/strace/heaptrack/sysstat/time/lsof), memory (heaptrack/Valgrind), cpu (perf/sysstat/time/hyperfine), tracing (Perfetto/trace processor/trace-cmd/bpftrace), gpu (vulkaninfo/RenderDoc/drm_info), input (wayland-info/wev/evtest), scripts (ShellCheck). Run only needed groups; many distributions do not package Perfetto or its processor in the same package. Repository availability is checked at install time, not assumed. Ubuntu perf packages must match the running kernel; Docker Desktop's kernel often has no matching distribution package. Report it and use another available diagnostic, not a mismatched perf wrapper as evidence of support.

## Docker on this Windows host

Discover rather than assume:

```powershell
docker ps -a --format '{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Status}}'
docker inspect <chosen-container> --format '{{json .Mounts}}'
docker image ls
```

Do not use BuildKit's infrastructure container as the development shell. A stopped project container may be started when needed, after inspecting its mounts/entrypoint and confirming ownership. Mount paths must cover this exact worktree. Prefer an existing compatible Linux development image. For a task-local validation container, bind only this worktree, use a unique name, cap resources and use `--rm`; ordinary script syntax/helper checks need no GPU or privileged mode. Inspect installed Python/Bash and `/etc/os-release` before executing. Do not replace a current build image with an unpinned host upgrade just to obtain one optional tool. Reproducible image/package versions and volume paths belong in results.

For a chosen container, use `docker exec --workdir <actual-source-path> <name> python3 ...` and capture its real exit code. A container based on old Debian can validate Python/shell helpers while being unable to build Lumen. Separate those claims. Start no replacement graphical session.

## Tools outside the helper

Use configured distribution repositories first. Discover package providers (`apt-cache search/policy`, `pacman -Ss/-Si`, `dnf provides`, `zypper search`) and installed tool versions. Match LLVM tools/compiler-rt to Clang; runtime objects ship with compiler packages, not an `asan` CLI. Vendor GPU diagnostics and specialized tools (Nsight, Radeon GPU Profiler, Intel GPA, gfxreconstruct, hotspot, KernelShark, BCC, rr, AFL++, SPIR-V tools) have platform, license or dependency limits; consult official current docs for the selected target rather than bulk installing them.

For Perfetto/trace processor not packaged in configured repos, use the official [Perfetto installation/build instructions](https://perfetto.dev/docs/quickstart/trace-analysis) or [source build](https://perfetto.dev/docs/contributing/build-instructions). Choose an explicit release/architecture, verify published checksum/signature when available, retain URL/version/hash, and install into a task/user tool directory with explicit PATH. If no published checksum exists, record that provenance limitation; do not invent verification. Do not pipe a network response directly into a shell or silently execute a self-downloading launcher. An offline Perfetto UI requires its own verified distribution. Native Lumen JSON can still be collected without installing a recorder; analyzing system scheduling needs the separate supported capture path.

Runtime diagnosis before escalation: `perf_event_paranoid`, Yama ptrace_scope, tracefs/BTF availability, kernel version, capabilities/seccomp, render-node permissions, VK ICD/layer manifests, XDG runtime directory, D-Bus/user systemd/logind, input nodes and VKMS. Read settings; do not disable seccomp, add all capabilities, make device nodes world-writable, change host sysctls or reboot to pass a test. Any separately authorized temporary capability change needs precise scope and restoration evidence. Missing hardware/permissions/services remain unsupported or could-not-run, never PASS.
