---
name: lumen-todo-fixes-round
description: "2026-11-23/24 notebook, TODO-fixes round (interactive; lead commits 73ee6a3, 8086418, 6a902f2, 0796540, a89cbad on main, unpushed): all three saved workflows unloadable for four days — a scope: key in the /* zcode-workflow block, 2e6f772 (11-19 19:57) to 73ee6a3 (11-23 20:11), ListSavedWorkflows 0 to 3; evidence/ untracked (37 blobs, 12 CRLF); 3 TODO rows closed + 5 claims corrected on evidence (researcher sweep: 27 rows, 5.17M tokens, 44 min); compare.sh self-brokers; studio run dwfrun-e4cd1b91 (135,875,194 tokens, 6h10m, 2 tasks READY, 9 TODO rows out of its reviews): landing declined on untracked .mimosa/, LANDING BYPASSED — spool commit f836bc6 parented on task tip f936cda, ff-merged onto main 23:37:46; AmendWorkflow drops a run's args and does not replay finished stages; by-hand merge a89cbad re-gated 141/146 green, not pushed; known_red=[smoke_harness] hid the test under repair from every gate (:529 :672 :793 :1172)"
summary: "Workflows unloadable 4 days (scope: key) fixed; evidence/ untracked; 8 TODO rows reconciled. Landing declined on untracked .mimosa/, then BYPASSED by a spool-parent ff; by-hand a89cbad 141/141."
modules: harness, scripts
topics: landing, memory-spool, workflows, amend, todo-reconciliation
metadata:
  node_type: memory
  type: project
---
Verified 2026-11-24 in the main checkout (main at a89cbad, 22 commits ahead of origin/main
844cbbe): every SHA, parent, count and file:line below re-read with `git log` / `git show` /
the main reflog and the merge-gate's LastTest.log; token and duration figures and the
harness-tool observations are the lead's run reports. An interactive notebook session
(i915+nouveau), 2026-11-23/24 — the owner took all four scope groups, "commit on main
now" for harness/TODO bookkeeping and "untrack evidence/" in one consolidated question.

**Lead's commits on main (owner-approved, none pushed):**

- **73ee6a3 — saved workflows load again.** A `scope:` key in the `/* zcode-workflow` block
  fails the loader (`invalid_metadata: Unrecognized key: "scope"`) and in two files also sat
  inside the unquoted multi-line `whenToUse` scalar — not even YAML. All three workflows in
  `.zcode/workflows/` were unloadable from 2e6f772 (2026-11-19 19:57 — the commit that added
  the keys; the repair message blames fa9015e, which touches no workflow file) to 73ee6a3
  (2026-11-23 20:11): `ListSavedWorkflows` returned 0 for four days — the evals round
  [[lumen-harness-evals-green]] parse-checked the TypeScript bodies only. Repair: `whenToUse`
  quoted, the Scope label moved to a `// Scope: current-project — …` comment after the block,
  script bodies untouched; verified by `ListSavedWorkflows` (3) and a by-name launch.
- **8086418 — `evidence/` untracked** (`git rm -r --cached`): 37 blobs under
  `evidence/2026-11-18-sanitizer-sweep/`, 12 with CRLF bytes against `eol=lf`; all 37 stay
  on disk and in history, the `/evidence/` rule (0eb19ac) covers them. Branches cut earlier
  that edit those paths show delete/modify conflicts — keep them untracked.
- **6a902f2 / 0796540 — TODO reconciled on evidence:** three rows closed —
  snprintf-log-migration (merged 08bde4e, 2026-11-18), Vulkan capture green (on main by
  content, not ancestry: the capture-manager symbol, `read_back_wait_fence` and
  `test_vk_capture` are all there), compare.sh; five stale claims corrected: ADR-013 item
  2's compositor side is on main since 0439bc4, the settings banner's lead is
  `session::repaint()`, six invariant GAPs not four, "LONG" = 12 records / 13 files, the
  tap-grab evidence in the `settings-key-capture-close-out` worktree.
- **compare.sh self-brokers** (`~/lumen-compare/compare.sh:21-27`, outside the repo): a
  direct run re-execs through `scripts/seat-lock.sh with bench compare --` before its first
  side effect, `LUMEN_SEAT_LOCKED=1` from an outer wrap letting it proceed (lead-verified:
  `ROUNDS=1 NAMES=lumen ./compare.sh`: ticket held mid-run, released after, exit 0).

**Studio run dwfrun-e4cd1b91** ("run 1": tests-only, `run_smokes: false`,
`known_red: ["smoke_harness"]`) — 135,875,194 tokens, 6h10m, two tasks gate-READY, each with
its own record, not restated here: [[lumen-smoke-harness-broker-fixture]] (tip f936cda,
`smoke_harness` 49 ok / 13 FAIL → 82 ok / 0 FAIL) and [[lumen-invariant-gap-tests]] (tip
976b3a1, four designated tests, suite 142 → 146). The reviews confirmed nine pre-existing or
unimplemented items, now TODO rows (six product defects, the GPU pools' missing frame path,
two harness items); the Invariant GAPs row names what no test can reach: no direct scanout
exists, `page_flip_event::applied_damage` is written nowhere, `present_mode_for` has no caller.

**LANDING — the fuckups, in order (the valuable part):**

1. **Declined on untracked tool state.** The clean-main check is `git status --porcelain`
   (`lumen-development-studio.dwf.ts:1123-1124`); Mimosa's untracked `.mimosa/` at the repo
   root read as a dirty main → "merge by hand or re-run". Local fix: `.mimosa/` in
   `.git/info/exclude`; the `.gitignore` line is the owner's call.
2. **LANDING BYPASS.** Memory commit f836bc6 was spooled with the unlanded task tip as
   its parent (`f836bc6 parent=f936cda` — the recipe falls back to `git rev-parse HEAD`,
   `lumen-memory-writer.md:31`, and HEAD was the task tip), "so it can only fast-forward
   onto main once that tip is there". Inverted: the tip descends from main, so a later
   memory-writer's contract step `git merge --ff-only zcode/memory-spool` moved main onto
   it at once (reflog `Fast-forward`, 2026-11-23 23:37:46; its record 8f06533 on top at
   23:47) — 3f317c8, 9579cdc, bc33b6a, f936cda on main ahead of the landing phase,
   regardless of the gate, without the merged-tree re-gate. Verify:
   `git log --format='%h parent=%p %s' -6 main`.
3. **AmendWorkflow could not re-issue the landing.** (a) An amended run does NOT inherit
   the run's arguments: dwfrun-13a56143 exited at once with "No task was given". (b) With
   the arguments restated in the draft (dwfrun-ed413453) the finished stages did not
   replay — `world.run` steps executed live (run lock re-taken 03:12:06) and on the second
   hop the Planner itself re-ran — so the lead stopped it and released the lock it left
   behind: `scripts/run-lock.sh release '<exact owner string>'`.
4. **By-hand landing**, the workflow's own fallback wording: `git merge --no-edit
   zcode/task/invariant-gap-tests` → a89cbad (tree ee28737 = the `git merge-tree --write-tree`
   preview), then the landing's three commands on `build/merge-gate`: configure rc=0, build
   rc=0 (197 steps), 141 of 146 registered tests passed, 0 failed (`-LE bench`, 81.48 s),
   `smoke_harness` #77 passing. NOT pushed — the next by-the-run landing pushes the 22 commits.
5. **known_red blinds the gate that matters.** With `known_red = ["smoke_harness"]`
   every workflow gate filters out the one test under repair (`:529`, `:672`, `:793` and
   the landing re-gate `:1172`): its pass rested on the reviewer's unfiltered run and the
   gatekeeper's targeted run, never on the orchestrator's own "green" line. Both
   gatekeepers flagged it; first recorded in [[lumen-smoke-harness-broker-fixture]].

Run 2 (the three seat defect hunts, `run_smokes: true`, dwfrun-bfaf0612) started after
these facts were fixed — its outcome is not part of this record.

**Why:** a landing gate is a gate only if nothing else can move main — the spool landed four
commits without one. Tolerating the test under repair, a clean check blind to tool state, a
repair tool that forgets its inputs: one failure — a "fine" that does not mean what it says.

**How to apply:** before `git merge --ff-only zcode/memory-spool`, read
`git log --format='%h %s' main..zcode/memory-spool` — a line that is not a `memory:` commit
is an unlanded task commit: do not merge. By hand: `git merge --no-edit <branch>`, check
`HEAD^{tree}` against `git merge-tree --write-tree`, then configure / build / `ctest -LE bench`
on `build/merge-gate` before anything pushes. Restate a run's args in an amended draft; expect
finished stages to run again. Never `known_red` a test the diff touches. After a
workflow-block edit, `ListSavedWorkflows` must list it. Reconcile TODO rows by content on
main, not ancestry — the 27-row researcher sweep (5.17M tokens, 96 calls, 44 min: 4 done /
13 open / 6 blocked / 4 notes) says do the 2–3 decisive checks per row yourself first.

Rule proposal (spool → landing bypass): the spool recipe parents on main's tip, never HEAD;
the merge step refuses unless every line of `git log main..zcode/memory-spool` is a `memory:`
commit; and before each merge and the push the landing asserts that main moved only by its own
merges since the base its tasks were cut from (`preMerge` at `:1134` is read too late).
Rule proposal (clean-main check): untracked tool state must not fail the landing — either
each tool-state directory carries a `.gitignore` line (a reviewer checklist item; the strict
check stays), or the check becomes `--untracked-files=no` AND the merge-gate stops building
the working tree — its module glob (cmake/LumenCXXModules.cmake:51-56) would compile an
untracked source into a gate whose commit the push leaves behind.
Rule proposal (known_red): a gate may not filter a known-red name the task's diff touches —
it intersects `known_red` with the changed paths and runs each hit by name unfiltered
(`ctest -R '^<name>$' -LE bench`), recording the `Passed` line in the gate reasons; READY
is refused without it.

Related: [[lumen-harness-evals-green]] (previous round: flipped push policy, landing phase),
[[lumen-harness-review-green]] (the spool's purpose), [[lumen-smoke-harness-broker-fixture]],
[[lumen-invariant-gap-tests]], [[lumen-zcode-agent-harness]].
