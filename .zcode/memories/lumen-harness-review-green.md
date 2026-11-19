---
name: lumen-harness-review-green
description: "2026-11-19: the .zcode harness review's findings all fixed on branch zcode/task/harness-review (worktree harness-review, 10 commits) — seat broker + run lock + hooks fail-closed + agents as single source + derived evidence + layer_graph ctest + diff-hygiene + memory schema with generated indexes; every fix verified on this host"
summary: "Harness review fixed: seat broker, run lock, fail-closed hooks, agents single-source, classifier-derived evidence, layer_graph ctest, diff-hygiene, memory schema + generated indexes"
modules: harness, scripts
topics: zcode, review, broker, layer-graph, memory
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-19 on the **notebook** host (i915+nouveau; a live lumen-compositor
held the seat all session — every seat-path test ran against the broker's refusal, no
seat job was started). Branch `zcode/task/harness-review`, worktree
`../lumen-wt/harness-review`. The review document's P0/P1/P2 lists drove the work;
every mechanical claim in this record was re-run, not asserted.

- **Seat broker** (`scripts/seat-lock.sh` + `lumen-seat` skill): acquire/release/renew/
  status/preflight/`with`; priority queue bench>smoke>screenshot, FIFO within class,
  never preempt; TTL bounds only dead holders, stale = owner decision with the exact
  force-release command. Verified: queueing, priority jump, wait-timeout exit 4, stale
  exit 3, preflight blocks on the live compositor, no false self-match (three bugs found
  by test: `now()` inside `(( ))` parses as a variable — bash arithmetic does not call
  functions; the EXIT trap outliving locals; `return $?` after `echo` losing the code).
  Ten seat scripts re-exec under it (10-line guard after ROOT=, `--help` lock-free).
- **Run lock** (`scripts/run-lock.sh`): per-WT_ROOT, heartbeat per task, stale never
  auto-broken.
- **Hooks**: protect-generated-paths fails closed (python3-missing/malformed blocks,
  no lossy regex fallback when python exists, relative paths covered); new
  guard-bash-edits.sh (matcher Bash) catches sed -i / redirects / cp / python open-w
  onto `build/generated`/`protocols/upstream` while cmake/fetch/read-only pass. 12/12
  matrix.
- **Agents are the single source of truth**; workflows point at
  `.zcode/agents/*.md` (fixer/confirmer/memory-writer got files; reviewer got
  disallowedTools + the reading-list rule + the test-quality bar; gatekeeper got
  NEEDS_REDESIGN, BLOCKED-ON-HOST, computed counts, knob sign-off, broker-line check).
- **Derived evidence**: `scripts/classify-change.py` (8 synthetic cases + live diff);
  run_smokes/run_bench are additive-only; render-path on VM → BLOCKED-ON-HOST.
- **layer_graph ctest** (`scripts/check-layer-graph.py`): 319 files, 811 edges, zero
  upward edges on the real tree — the graph was intact, just unguarded; synthetic
  upward/unknown/unmapped each fail.
- **diff-hygiene.sh**: found that the "lx.shell.cppm is CRLF" premise was already dead
  (.gitattributes pins LF; both files LF at HEAD) AND that 12 evidence blobs on main
  carry CRLF past the attributes (git diff CANNOT see a CRLF flip — commit normalizes
  it; only a worktree scan catches it).
- **Memory schema**: gen-memory-index.py, MEMORY.md 28 KB→9.4 KB with ≤200-char rows,
  INDEX.md by module; the three 300–430-line records split byte-preserving;
  two-strikes Rule-proposal rule in the skill.
- **Trap (bash)**: functions called inside `(( ))` are parsed as variables —
  `(( now() - x ))` + `set -u` = "unbound variable". Precompute into a local first.

**Why:** the harness's quality claim was real but unenforced — prose invariants,
advisory locks, knobs that could waive evidence, and indexes no agent could load. Every
fix here converts one prose rule into a mechanism that fails loudly when violated.

**How to apply:** run `ctest -R layer_graph` in any tests-enabled build; run
`scripts/diff-hygiene.sh <base>` before committing; never start a seat job outside the
brokered scripts; regenerate indexes with `scripts/gen-memory-index.py` after memory
writes; when a workflow stage contract changes, change `.zcode/agents/*.md` — the
workflow personas only point.

Related: [[lumen-zcode-agent-harness]], [[lumen-smoke-bench-harness-green]],
[[lumen-notebook-full-green-audit]], [[lumen-unit-pair-green]].

Independent review round (lumen-reviewer, base-ref policy): 14 findings — 2 CRITICAL,
2 MAJOR, 6 MINOR, 4 NOTE — 14 confirmed / 0 refuted, all fixed in the review round. The
CRITICALs were the classifier's own diff: it never saw uncommitted tracked edits (docs-only
under-requiring for exactly the targeted change classes) and gated-change diffed main's
last commit instead of the change. Both fixed (working-tree diff by default; the branch
point captured as a concrete SHA; the studio's symbolic HEAD base resolved once at run
start — a pre-existing degeneracy the new consumers had inherited). MAJORs: RENDER_PATH
now carries the compositor present pipeline (compositor_impl, ui_composite,
presentation_feedback + wire_presentation, output_manager/policy, content_update) so
BLOCKED-ON-HOST triggers on §1.2's decisive surface. MINORs: holder-TTL persisted (a
--ttl-min 90 holder is not stale at 25 min), atomic queue tickets, run-lock acquire under
flock with owner-checked heartbeat, broker-origin smoke exits (2/3/4, distinguished by
the seat-lock: line) never counted as change failures, the release-preset ctest count
truth, and this record's own on-branch exception. NOTEs: layer-graph phantom flags and
partition imports, the Bash guard's best-effort honesty, the spool recipe's tmp-index
leak, run-lock's manual-invocation caveat.

Rides this task branch deliberately (the record documents it); on merge approval,
land it on main through the spool path per lumen-memory-writer — if the branch is
dropped instead, this record dies with it, which is exactly the loss mode the spool
rule exists for.
