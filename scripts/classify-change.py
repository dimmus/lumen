#!/usr/bin/env python3
"""Classify a Lumen change from its diff and derive the REQUIRED evidence.

The gate's requirements come from what the diff touches, not from operator
knobs: a render-path change with run_bench=false previously reached the
gatekeeper with no A/B and could still be READY. This tool is the single
derivation — the studio and gated-change workflows call it, the planner
cross-checks against it, and the gatekeeper refuses READY when derived
evidence is missing (an operator knob can add evidence, never remove it).

Usage:
    python3 scripts/classify-change.py [--base REF] [--head REF] [--host vm|notebook|auto]

--head defaults to the working tree (committed diff base..HEAD plus untracked
files). --host auto-detects from the DRM drivers (vmwgfx → vm, anything else
→ notebook — the two hosts share the name iritur). Output is JSON on stdout:

    classes      what the change touches (docs-only, logic, seat-surface,
                 render-path, ui-engine, protocol, build, harness-scripts, display-manager)
    required     the evidence class the gate must see: ctest, smokes,
                 bench_ab, goldens, display_manager_boot
    render_path  the change rides the decisive present path
    decisive_backend_blocked_on_host
                 render-path change on a host with no hardware Vulkan (the
                 vmware VM): the gate cannot be certified on this host at all
                 (BLOCKED-ON-HOST per lumen-gatekeeper) and needs a notebook
                 ticket before merge
    changed      the file list the verdict was derived from (bounded)
"""
import argparse
import json
import os
import re
import subprocess
import sys

RENDER_PATH = ("modules/lx.gfx/", "modules/lx.scene/", "modules/lx.text/", "modules/lx.theme/")
# The compositor-owned present pipeline (AGENTS.md §5: scanout evaluate →
# assign_plane OR merged_render_pass, presentation feedback, the composite
# tick): render-path by the gatekeeper's own BLOCKED-ON-HOST definition, even
# though the module as a whole is seat-surface. Narrow by file — the rest of
# lx.compositor (wire_*, input, tiling, …) is not the present path.
PRESENT_PIPELINE = (
    "modules/lx.compositor/compositor_impl",
    "modules/lx.compositor/ui_composite",
    "modules/lx.compositor/presentation_feedback",
    "modules/lx.compositor/wire_presentation",
    "modules/lx.compositor/output_manager",
    "modules/lx.compositor/output_policy",
    "modules/lx.compositor/content_update",
)
DISPLAY_MANAGER = ("executables/lumen-dm/", "deploy/lumen-dm/", "deploy/systemd/lumen-dm.service.in")
SEAT_SURFACE = (
    "modules/lx.compositor/", "modules/lx.input/", "modules/lx.drm/",
    "modules/lx.session/", "modules/lx.shell/", "modules/lx.wayland.server/",
    "executables/lumen-compositor/", "executables/lumen-shell/",
) + DISPLAY_MANAGER
UI_ENGINE = ("modules/lx.ui/", "modules/lx.ui.host/", "modules/lx.app/")
DOC_PATTERNS = (re.compile(r"^docs/"), re.compile(r"^\.zcode/"), re.compile(r"\.md$"),
                re.compile(r"^AGENTS\.md$"), re.compile(r"^README"))


def run_git(repo, args):
    return subprocess.run(["git", "-C", repo] + args, capture_output=True, text=True)


def detect_host():
    drivers = set()
    base = "/sys/class/drm"
    for card in (os.path.join(base, d) for d in os.listdir(base) if d.startswith("card")):
        link = os.path.realpath(os.path.join(card, "device", "driver"))
        if os.path.isdir(link):
            drivers.add(os.path.basename(link))
    if drivers == {"vmwgfx"}:
        return "vm"
    return "notebook"


def changed_files(repo, base, head):
    """Changed paths vs base. With no --head, the diff is against the WORKING
    TREE (committed + uncommitted tracked edits) — the classifier must see
    uncommitted work, or a seat-surface change silently classifies docs-only."""
    files = []
    if head:
        r = run_git(repo, ["diff", "--name-only", base + ".." + head])
    else:
        r = run_git(repo, ["diff", "--name-only", base])
    if r.returncode == 0:
        files += [f for f in r.stdout.splitlines() if f.strip()]
    r = run_git(repo, ["ls-files", "--others", "--exclude-standard"])
    if r.returncode == 0:
        files += [f for f in r.stdout.splitlines() if f.strip()]
    return sorted(set(files))


def classify(files, host):
    classes = set()
    required = dict.fromkeys(("ctest", "smokes", "bench_ab", "goldens", "display_manager_boot"), False)
    render_path = False

    def is_doc(p):
        return any(rx.search(p) for rx in DOC_PATTERNS)

    for p in files:
        if p.startswith(RENDER_PATH) or p.startswith(PRESENT_PIPELINE) or p.startswith("tests/bench"):
            classes.add("render-path")
            render_path = True
        if p.startswith(SEAT_SURFACE) or p.startswith("protocols/"):
            classes.add("seat-surface")
        if p.startswith(DISPLAY_MANAGER):
            classes.add("display-manager")
        if p.startswith(UI_ENGINE):
            classes.add("ui-engine")
        if p.startswith(("modules/", "executables/", "tests/")):
            classes.add("logic")
        if p.startswith(("CMakeLists.txt", "cmake/", "CMakePresets.json")):
            classes.add("build")
        if p.startswith("scripts/"):
            classes.add("harness-scripts")
        if not is_doc(p):
            classes.discard("docs-only-marker")
    if files and all(is_doc(p) for p in files):
        return {
            "classes": ["docs-only"], "required": required,
            "render_path": False, "decisive_backend_blocked_on_host": False,
            "changed_files_count": len(files),
        }

    if classes & {"logic", "build", "harness-scripts", "seat-surface", "render-path", "ui-engine"} \
            or "protocol" in classes:
        required["ctest"] = True
    if "seat-surface" in classes:
        required["smokes"] = True
    if "display-manager" in classes:
        required["display_manager_boot"] = True
    if "render-path" in classes:
        required["bench_ab"] = True
    if "ui-engine" in classes:
        required["goldens"] = True
    if any(p.startswith("protocols/") for p in files):
        classes.add("protocol")

    return {
        "classes": sorted(classes) or ["empty-diff"],
        "required": required,
        "render_path": render_path,
        "decisive_backend_blocked_on_host": render_path and host == "vm",
        "changed_files_count": len(files),
    }


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="HEAD", help="diff base ref (default HEAD)")
    ap.add_argument("--head", default="", help="head ref (default: working tree)")
    ap.add_argument("--host", default="auto", choices=["auto", "vm", "notebook"])
    ap.add_argument("--repo", default=os.getcwd())
    ap.add_argument("--max-files", type=int, default=400, help="cap the reported file list")
    a = ap.parse_args()

    host = detect_host() if a.host == "auto" else a.host
    files = changed_files(a.repo, a.base, a.head)
    verdict = classify(files, host)
    verdict["host"] = host
    verdict["changed"] = files[: a.max_files]
    json.dump(verdict, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
