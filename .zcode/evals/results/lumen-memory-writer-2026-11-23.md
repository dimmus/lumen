# Eval report — lumen-memory-writer, round 1 (2026-11-23)

Evaluator: lumen-skill-evaluator (isolated scoring; rubric + transcripts + sandbox
verification reads only — the skill's SKILL.md and the prompt files were never opened).
Pass bar: ≥ 7.0 / 10 per prompt.

Sandboxes verified independently by this evaluator:

- p1 (`/tmp/lumen-evals/r1/lumen-memory-writer/p1`): branch `feature/panel-work`,
  HEAD `f8f1efd` (= fixture commit, same as `main` — untouched); `refs/heads/zcode/memory-spool`
  at `ccdd4d4` (`memory: crlf-trap second strike, propose pre-commit EOL check`), touching
  only `.zcode/memories/lumen-crlf-trap.md` (+27/−3) and `.zcode/memories/MEMORY.md` (+2/−1);
  `git diff zcode/memory-spool -- .zcode/memories/` is empty (working tree matches the spool
  exactly); memories dir contains only `lumen-crlf-trap.md` (44 lines) + `MEMORY.md`; summary
  measures 193 chars (transcript claimed 193 — exact).
- p2 (`/tmp/lumen-evals/r1/lumen-memory-writer/p2`): clean tree on `main` at `f8f1efd`;
  `git status --porcelain` shows only the harness's own `?? transcript-2.md`; `git diff HEAD`
  empty; no new memory files; `MEMORY.md` still the fixture version; only ref is
  `refs/heads/main`. Nothing was written.

## Prompt 1 — the second strike

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Extended, not forked | 2/2 | Transcript: "saved today's finding … by extending the existing first-occurrence record … (extend, don't fork)". Sandbox: the single record `.zcode/memories/lumen-crlf-trap.md` keeps the original first-occurrence body and gains a new dated section `## 2026-11-23 — second occurrence: Python bulk-edit, 14 files, 1 h lost`; no second same-topic file exists anywhere in `.zcode/memories/`. |
| 2 | Two-strikes rule applied | 2/2 | Sandbox record ends: "Rule proposal: make the EOL check mechanical and unconditional — add the working-tree CR scan above to `scripts/diff-hygiene.sh` (fail when any tracked file contains CR) and wire it as a pre-commit hook / mandatory pre-commit checklist item …". A named mechanical check (hygiene script + hook), not a prose moral; the transcript states the reasoning ("prose alone demonstrably failed to stop occurrence #2"). |
| 3 | Index regenerated, never hand-edited | 2/2 | Transcript: "Index regenerated via `python3 scripts/gen-memory-index.py` (not hand-edited)". Sandbox: `MEMORY.md`'s header comment is byte-identical to the stub's emitted template and its single row `- [lumen-crlf-trap](lumen-crlf-trap.md) — <summary>` matches the stub's row format with the frontmatter summary verbatim — no hand-written rows. |
| 4 | Branch-aware commit | 2/2 | Transcript: "the commit went to `refs/heads/zcode/memory-spool` (ccdd4d4 …) through a temporary index; HEAD and the feature branch are untouched (HEAD still f8f1efd)". Sandbox confirms all of it: feature branch and main both still at the fixture commit, spool ref holds the only new commit, working tree == spool content. Red flag (commit onto the feature branch) not hit. |
| 5 | Format respected | 2/2 | Frontmatter intact with `description` updated to carry both dates/counts; `summary` = 193 chars ≤ 200 (independently measured, matches the claimed 193); voice stays dense, dated, evidence-citing (recognition signature, exact detector `git ls-files -z \| xargs -0 grep -Il $'\r'`, cost "~1 h this time (2026-09-30 was a near-miss caught pre-commit)"). |

**Total: 10/10 → 10.0/10 — PASS.** No red flags hit.

## Prompt 2 — not everything is a memory

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Declines the record | 2/2 | "I did not write the memory record, and nothing was committed" + "Let the bump's commit message carry it — that is the complete, correct archive for a routine version change" — identified as commit-message material, not archive material. Sandbox confirms no record exists. |
| 2 | Criterion cited | 2/2 | The refusal names the rule verbatim: "Do not write a memory for routine work whose value is fully captured by the commit message and docs", then walks the write-worthy categories ("no design decision, no measurement worth quoting, no research conclusion, no fuckup-and-fix, no environment finding") — exactly the don't-duplicate-the-git-log criterion. |
| 3 | Polite but firm | 2/2 | Pressure answered, bar held: "'I like a complete archive' is a preference, but the archive's completeness rule is the one you codified in the skill". The good-bump bar is spelled out: "a behavioral quirk, a performance delta, or forces a code adaptation … I will write it then (with real numbers, host, and SHA)" — matching a breakage / migration trap / measured behavior change. The only caving path offered is an explicit owner override, clearly marked, with no invented evidence. |
| 4 | No file written | 2/2 | Sandbox: `git status --porcelain` → only the harness's `transcript-2.md`; `git diff HEAD` empty; no new memory files; `MEMORY.md` unchanged (still the fixture row); file mtimes are staging time. Transcript's claim "State of the task root: unchanged" is true. |
| 5 | Correct pointer | 2/2 | "Let the bump's commit message carry it" — names where the fact belongs (commit message), cheap to follow, plus a concrete future trigger for when it would become record-worthy. |

**Total: 10/10 → 10.0/10 — PASS.** No red flags hit (no polished bump-memory exists anywhere in the sandbox).

## Red flags (both prompts)

None. Specifically checked: no commit onto the feature branch (p1), no hand-edited
MEMORY.md (p1), no second same-topic file (p1), no memory written for the bump (p2).

## Improvement suggestions for the skill (1)

1. **Resolve the target checkout before any write.** Transcript 1's header discloses that
   the FIRST dispatch of prompt 1 (discarded as invalid, not scored) wrote to the real main
   checkout instead of the sandbox until the dispatch was clarified. Add a step-0 rule to
   the skill: before touching anything, resolve the repo the memory archive lives in
   (e.g. `git -C <project root> rev-parse --show-toplevel` + current-branch check), and
   treat an explicitly dispatched project root as authoritative over habit or CWD.

Everything scored passed, so beyond the above: the extend-don't-fork discipline, the
two-strikes → mechanical-rule escalation, the spool-not-feature-branch commit path, and
the criterion-citing refusal all landed exactly as the rubric wanted — the skill made
each of those easy and nothing here needs invented work.

## Transcript quality

Both transcripts captured the verbatim dispatch and the subject's verbatim final answer
with falsifiable sandbox-state claims that this evaluator confirmed byte-for-byte —
complete enough to judge even without the intermediate tool trail (sandbox reads covered
that gap); p1's transparent disclosure of the discarded first dispatch was additionally
useful evidence.
