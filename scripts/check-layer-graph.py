#!/usr/bin/env python3
"""Layer-graph invariant check for the Lumen module tree.

The layer graph (AGENTS.md §3.1, docs/architecture.md §3,
docs/uml/01-package-layers.mmd) — Foundation → Runtime → Platform → Graphics
→ Compositor → Desktop → Application, dependencies flow downward only — was
prose enforced by one tired reviewer per change. This check makes it
mechanical: it parses every `import lx.*` in modules/ and fails on any upward
edge and on any import of a module that does not exist. Partition-qualified
imports (import lx.foo:bar) resolve to their owning module lx.foo. New modules
must be added to LAYERS here in the same change that adds them (a module
missing from the map is an error, not a default).

Usage:
    python3 scripts/check-layer-graph.py [--root REPO] [--quiet]
"""
import argparse
import os
import re
import sys

# The layer map, mirroring docs/uml/01-package-layers.mmd. Same-layer imports
# are allowed (COMP→SCENE, SHELL→POLICY, THEME→GFX are same-layer by design).
LAYERS = {
    "lx.foundation": 0,
    # Runtime
    "lx.runtime": 1, "lx.sync": 1, "lx.trace": 1, "lx.health": 1, "lx.config": 1,
    # Platform
    "lx.wayland.server": 2, "lx.wayland.client": 2, "lx.wayland.protocols": 2,
    "lx.input": 2, "lx.session": 2, "lx.drm": 2,
    # Graphics
    "lx.gfx": 3, "lx.text": 3, "lx.layout": 3, "lx.theme": 3,
    # Compositor
    "lx.compositor": 4, "lx.compositor.xwayland": 4, "lx.scene": 4, "lx.scheduler": 4,
    # Desktop
    "lx.shell": 5, "lx.shell.policy": 5, "lx.ui": 5, "lx.ui.host": 5,
    "lx.a11y": 5, "lx.portal": 5,
    # Application
    "lx.app": 6,
}
LAYER_NAMES = {0: "Foundation", 1: "Runtime", 2: "Platform", 3: "Graphics",
               4: "Compositor", 5: "Desktop", 6: "Application"}

# Generated protocol modules (facade re-exports them; sources live in
# build/generated, not modules/) — Platform layer, existence accepted.
GENERATED_PREFIX = "lx.wayland.protocols"

IMPORT_RE = re.compile(r"^\s*(?:export\s+)?import\s+(lx\.[a-zA-Z0-9_.]+)(?::[a-zA-Z0-9_]+)?\s*;", re.M)


def module_of(path):
    """modules/lx.foo.bar/... → lx.foo.bar (the owning module)."""
    parts = path.split(os.sep)
    if len(parts) >= 2 and parts[0] == "modules" and parts[1].startswith("lx."):
        return parts[1]
    return None


def layer_of(name):
    if name.startswith(GENERATED_PREFIX):
        return LAYERS["lx.wayland.protocols"]
    if name in LAYERS:
        return LAYERS[name]
    return None


def exists(name, root):
    if name.startswith(GENERATED_PREFIX):
        return True
    return os.path.isdir(os.path.join(root, "modules", name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    violations = []
    edges = 0
    files = 0
    for dirpath, _dirs, names in os.walk(os.path.join(a.root, "modules")):
        for fname in names:
            if not fname.endswith((".cppm", ".cpp")):
                continue
            full = os.path.join(dirpath, fname)
            rel = os.path.relpath(full, a.root)
            owner = module_of(rel)
            if owner is None:
                continue
            files += 1
            try:
                text = open(full, encoding="utf-8", errors="replace").read()
            except OSError as e:
                violations.append(f"{rel}: unreadable ({e})")
                continue
            for m in IMPORT_RE.finditer(text):
                target = m.group(1)
                edges += 1
                if not exists(target, a.root):
                    violations.append(f"{rel}: imports {target} — no such module under modules/")
                    continue
                if target == owner:
                    continue  # self/facade import
                lo, lt = layer_of(owner), layer_of(target)
                if lo is None:
                    violations.append(f"{rel}: owning module {owner} is not in the layer map — add it to scripts/check-layer-graph.py LAYERS in this change")
                    continue
                if lt is None:
                    violations.append(f"{rel}: imported module {target} is not in the layer map — add it to scripts/check-layer-graph.py LAYERS in this change")
                    continue
                if lt > lo:
                    violations.append(
                        f"{rel}: UPWARD EDGE — {owner} ({LAYER_NAMES[lo]}) imports {target} "
                        f"({LAYER_NAMES[lt]}); dependencies flow downward only (AGENTS.md §3.1)")

    if not a.quiet:
        print(f"layer graph: {files} files, {edges} import edges checked")
    if violations:
        for v in violations:
            print(f"FAIL: {v}")
        print(f"{len(violations)} layer-graph violation(s)")
        return 1
    if not a.quiet:
        print("layer graph: no upward edges, all imports resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
