#!/usr/bin/env python3
"""Validate a Lumen workflow packet; this is validation, not a scheduler."""

import argparse
import json
import sys
from pathlib import Path

STAGES = {
    "proposed", "baseline", "implement", "review", "verify",
    "findings/fix", "gate", "settled", "blocked", "paused",
    "failed-needs-decision", "abandoned-by-user",
}
ADVANCED = {"implement", "review", "verify", "findings/fix", "gate", "settled"}


def sample_packet():
    return {
        "run_id": "sample-run",
        "tasks": [
            {
                "id": "core", "dependencies": [], "stage": "review",
                "candidate": "tree-core-1", "base": "base-abc",
                "worktree": "worktrees/core", "owner": "implementer-core",
                "gates": [],
            },
            {
                "id": "docs", "dependencies": [], "stage": "implement",
                "candidate": "tree-docs-1", "base": "base-abc",
                "worktree": "worktrees/docs", "owner": "implementer-docs",
                "gates": [],
            },
        ],
    }


def validate(packet):
    errors = []
    if not isinstance(packet, dict):
        return ["packet must be a JSON object"]
    if not isinstance(packet.get("run_id"), str) or not packet["run_id"].strip():
        errors.append("run_id must be a nonempty string")
    tasks = packet.get("tasks")
    if not isinstance(tasks, list):
        return errors + ["tasks must be a list"]
    if not tasks:
        errors.append("tasks must contain at least one task")

    by_id = {}
    for i, task in enumerate(tasks):
        label = f"tasks[{i}]"
        if not isinstance(task, dict):
            errors.append(f"{label} must be an object")
            continue
        task_id = task.get("id")
        if not isinstance(task_id, str) or not task_id.strip():
            errors.append(f"{label}.id must be a nonempty string")
            continue
        if task_id in by_id:
            errors.append(f"duplicate task id: {task_id}")
        else:
            by_id[task_id] = task

    edges = {}
    for task_id, task in by_id.items():
        deps = task.get("dependencies")
        if not isinstance(deps, list) or any(not isinstance(d, str) for d in deps):
            errors.append(f"{task_id}.dependencies must be a list of task IDs")
            deps = []
        edges[task_id] = deps
        for dep in deps:
            if dep == task_id:
                errors.append(f"{task_id} cannot depend on itself")
            elif dep not in by_id:
                errors.append(f"{task_id} has missing dependency: {dep}")

        stage = task.get("stage")
        if not isinstance(stage, str) or stage not in STAGES:
            errors.append(f"{task_id}.stage is not a legal stage")
            stage = None
        candidate = task.get("candidate")
        if stage in ADVANCED and (not isinstance(candidate, str) or not candidate.strip()):
            errors.append(f"{task_id} requires a nonempty candidate at stage {stage}")
        for field in ("base", "worktree", "owner"):
            if not isinstance(task.get(field), str) or not task[field].strip():
                errors.append(f"{task_id}.{field} must be a nonempty string")
        gates = task.get("gates")
        if not isinstance(gates, list):
            errors.append(f"{task_id}.gates must be a list")
            gates = []
        if stage in {"gate", "settled"} and not gates:
            errors.append(f"{task_id} requires gates at stage {stage}")
        if stage in {"gate", "settled"}:
            for dep in deps:
                if dep in by_id and by_id[dep].get("stage") != "settled":
                    errors.append(f"{task_id} cannot reach {stage}: dependency {dep} is not settled")
        for n, gate in enumerate(gates):
            prefix = f"{task_id}.gates[{n}]"
            if not isinstance(gate, dict):
                errors.append(f"{prefix} must be an object")
                continue
            if gate.get("status") != "PASS":
                errors.append(f"{prefix} is not PASS (missing/unknown evidence never passes)")
            if gate.get("candidate") != candidate:
                errors.append(f"{prefix} candidate does not match task candidate")
            evidence = gate.get("evidence")
            digest = gate.get("sha256")
            if not isinstance(evidence, str) or not evidence.strip():
                errors.append(f"{prefix} requires an evidence reference")
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest.lower()):
                errors.append(f"{prefix} requires a SHA-256 artifact hash")
            roles = [gate.get(k) for k in ("author", "reviewer", "verifier", "gatekeeper")]
            if any(not isinstance(role, str) or not role.strip() for role in roles):
                errors.append(f"{prefix} requires author, reviewer, verifier, and gatekeeper identities")
            elif len(set(roles)) != len(roles):
                errors.append(f"{prefix} roles must be independent (no self-review or shared role)")

    # DFS detects cycles, including cycles through nodes with missing references excluded.
    visiting, visited = set(), set()

    def visit(node):
        if node in visiting:
            errors.append(f"dependency cycle includes {node}")
            return
        if node in visited:
            return
        visiting.add(node)
        for dep in edges.get(node, []):
            if dep in by_id:
                visit(dep)
        visiting.remove(node)
        visited.add(node)

    for task_id in edges:
        visit(task_id)
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--example", action="store_true", help="print a valid two-task packet")
    group.add_argument("--validate", metavar="FILE", help="validate a workflow JSON packet")
    args = parser.parse_args(argv)
    if args.example:
        print(json.dumps(sample_packet(), indent=2))
        return 0
    try:
        packet = json.loads(Path(args.validate).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot read JSON packet: {exc}", file=sys.stderr)
        return 2
    errors = validate(packet)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 2
    print("valid workflow packet")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
