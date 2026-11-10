#!/usr/bin/env python3
"""Check and install selected Lumen diagnostics from configured Linux repositories."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys

# Each entry: executable alternatives, then package candidates for apt/pacman/dnf/zypper.
TOOLS = {
    "gdb": (("gdb",), ("gdb",) * 4),
    "strace": (("strace",), ("strace",) * 4),
    "heaptrack": (("heaptrack",), ("heaptrack",) * 4),
    "valgrind": (("valgrind",), ("valgrind",) * 4),
    "perf": (("perf",), ("linux-perf", "perf", "perf", "perf")),
    "sysstat": (("pidstat",), ("sysstat",) * 4),
    "time": (("/usr/bin/time",), ("time",) * 4),
    "lsof": (("lsof",), ("lsof",) * 4),
    "bpftrace": (("bpftrace",), ("bpftrace",) * 4),
    "trace-cmd": (("trace-cmd",), ("trace-cmd",) * 4),
    "perfetto": (("perfetto",), ("perfetto",) * 4),
    "trace-processor": (("trace_processor", "trace_processor_shell"), ("perfetto",) * 4),
    "vulkaninfo": (("vulkaninfo",), ("vulkan-tools",) * 4),
    "renderdoc": (("renderdoccmd",), ("renderdoc",) * 4),
    "wayland-info": (("wayland-info",), ("wayland-utils",) * 4),
    "wev": (("wev",), ("wev",) * 4),
    "drm-info": (("drm_info",), ("drm-info", "drm_info", "drm_info", "drm_info")),
    "evtest": (("evtest",), ("evtest",) * 4),
    "shellcheck": (("shellcheck",), ("shellcheck",) * 4),
    "hyperfine": (("hyperfine",), ("hyperfine",) * 4),
}
GROUPS = {
    "core": ["gdb", "strace", "heaptrack", "sysstat", "time", "lsof"],
    "memory": ["heaptrack", "valgrind"],
    "cpu": ["perf", "sysstat", "time", "hyperfine"],
    "tracing": ["perfetto", "trace-processor", "trace-cmd", "bpftrace"],
    "gpu": ["vulkaninfo", "renderdoc", "drm-info"],
    "input": ["wayland-info", "wev", "evtest"],
    "scripts": ["shellcheck"],
}
FAMILIES = {"debian": "apt", "ubuntu": "apt", "arch": "pacman",
            "manjaro": "pacman", "fedora": "dnf", "rhel": "dnf",
            "opensuse-tumbleweed": "zypper", "opensuse-leap": "zypper", "suse": "zypper"}
MANAGERS = ("apt", "pacman", "dnf", "zypper")


def detect_manager(release=None):
    release = release if release is not None else platform.freedesktop_os_release()
    for name in (release.get("ID", "") + " " + release.get("ID_LIKE", "")).split():
        if name in FAMILIES:
            return FAMILIES[name]
    raise ValueError("Unsupported Linux distribution; use --manager for a reviewed package plan")


def selected_tools(groups):
    names = []
    for group in groups:
        for name in GROUPS[group]:
            if name not in names:
                names.append(name)
    return names


def probe(names):
    return {name: next((shutil.which(cmd) for cmd in TOOLS[name][0] if shutil.which(cmd)), None)
            for name in names}


def packages_for(manager, names, release):
    index = MANAGERS.index(manager)
    packages = [TOOLS[name][1][index] for name in names]
    # Ubuntu's perf wrapper needs tools matching the running kernel. Availability is
    # checked before mutation; Docker's kernel may have no matching distro package.
    if manager == "apt" and release.get("ID") == "ubuntu" and "perf" in names:
        packages = ["linux-tools-" + platform.release() if p == "linux-perf" else p for p in packages]
    return list(dict.fromkeys(packages))


def install_command(manager, packages, yes=False):
    if manager == "apt":
        return ["apt-get", "install", "--no-install-recommends"] + (["-y"] if yes else []) + packages
    if manager == "pacman":
        return ["pacman", "-S", "--needed"] + (["--noconfirm"] if yes else []) + packages
    if manager == "dnf":
        return ["dnf", "install"] + (["-y"] if yes else []) + packages
    return ["zypper"] + (["--non-interactive"] if yes else []) + ["install", "--no-recommends"] + packages


def available(manager, package):
    commands = {
        "apt": ["apt-cache", "policy", package],
        "pacman": ["pacman", "-Si", package],
        "dnf": ["dnf", "-q", "list", "--available", package],
        "zypper": ["zypper", "--non-interactive", "search", "--match-exact", "--type", "package", package],
    }
    result = subprocess.run(commands[manager], capture_output=True, text=True, timeout=90, check=False,
                            env={**os.environ, "LC_ALL": "C"})
    if result.returncode:
        return False
    if manager == "apt":
        return any(line.strip().startswith("Candidate:") and "(none)" not in line
                   for line in result.stdout.splitlines())
    if manager == "zypper":
        return any(len(parts := line.split("|")) >= 3 and parts[1].strip() == package
                   for line in result.stdout.splitlines())
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    for flag in ("list", "check", "plan", "install"):
        mode.add_argument("--" + flag, action="store_true")
    parser.add_argument("--groups", nargs="+", choices=GROUPS, default=["core"])
    parser.add_argument("--manager", choices=MANAGERS, help="Override distro detection; plan can run on Windows")
    parser.add_argument("--yes", action="store_true", help="Use package-manager noninteractive confirmation for --install")
    args = parser.parse_args(argv)
    if args.list:
        print(json.dumps(GROUPS, indent=2))
        return 0
    names = selected_tools(args.groups)
    found = probe(names)
    if args.check:
        print(json.dumps({"platform": platform.system(), "tools": found}, indent=2))
        return 1 if any(path is None for path in found.values()) else 0
    is_linux = platform.system() == "Linux"
    release = platform.freedesktop_os_release() if is_linux else {}
    manager = args.manager or (detect_manager(release) if is_linux else None)
    if manager is None:
        parser.error("Package plans on non-Linux require --manager; installation requires Linux")
    missing = [name for name in names if not found[name]]
    packages = packages_for(manager, missing, release)
    command = install_command(manager, packages, args.yes)
    print(json.dumps({"manager": manager, "tools_missing": missing, "packages": packages,
                      "command": shlex.join(command) if packages else None,
                      "note": "Candidate package names; --install preflights repository availability. No repository, kernel or security changes."}, indent=2), flush=True)
    if args.plan:
        return 0
    if not is_linux:
        parser.error("Install runs only inside the selected Linux development system")
    if not packages:
        print("No packages needed; executable discovery does not prove runtime capability.")
        return 0
    if not shutil.which(command[0]):
        raise ValueError("Package manager executable missing: " + command[0])
    unavailable = [package for package in packages if not available(manager, package)]
    if unavailable:
        print("Unavailable in configured repositories (nothing installed): " + ", ".join(unavailable), file=sys.stderr)
        print("Select a smaller group or install reviewed upstream releases per tool-setup.md. Repository metadata may need an explicit refresh.", file=sys.stderr)
        return 2
    if os.geteuid() != 0:
        if not shutil.which("sudo"):
            raise ValueError("Installation needs root or sudo in the selected Linux system")
        command = ["sudo", "--"] + command
    # Inherit stdio: show full package-manager transaction and preserve its exit status.
    result = subprocess.run(command, check=False)
    if result.returncode:
        return result.returncode if result.returncode > 0 else 1
    after = probe(names)
    print(json.dumps({"tools_after_install": after}, indent=2))
    return 1 if any(path is None for path in after.values()) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        sys.exit(2)
