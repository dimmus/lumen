# Eval report — lumen-decisions, round 1 (2026-11-23)

Evaluator: lumen-skill-evaluator (isolated; evidence = rubric + transcripts + sandbox
verification reads only). Rubric: `.zcode/evals/lumen-decisions/rubric.md`. Transcripts:
p1 (allocator comeback), p2 (reopening a reverted feature). Scale 0/1/2 per criterion,
/10 per prompt, PASS ≥ 7.0.

## Verification (all read-only)

- Sandboxes `/tmp/lumen-evals/r1/lumen-decisions/p{1,2}` contain only the transcripts —
  matches both "Sandbox state after: No fixtures; nothing created". Nothing fabricated.
- **p1 record check** — `.zcode/memories/lumen-malloc-replacement-measured.md` contains
  verbatim: measured 2026-10-27 (vmwgfx VM, GL path, drag-smoke under `LD_PRELOAD`, RSS
  every 250 ms); RSS glibc 26.9 / tcmalloc 36.6 / jemalloc 44.1 / mimalloc 57.5 MiB;
  latency "identical within noise"; heaptrack ~100k startup allocs, ~30/frame, frame path
  arena-based; reopen only on "a new workload that allocates on the frame path or a soak
  test showing glibc fragmentation"; "The shell was not measured." Every checkable claim
  in the answer matches the record.
- **p2 record check** — `.zcode/TODO.md` line 214 "## Parked (one fix round each, or
  waiting for a port)" with line 217-218 "Rounded corners / window shadows (raise-the-bar
  F1, reverted): radius 30–32 masked client top corners; GL zeroed arrays per draw";
  `.zcode/memories/lumen-raise-the-bar-run.md` line 23 (PR #63, merged 2026-10-29, F1
  REVERTED with both reasons) and line 26 licenses re-proposal verbatim: "S2 and F1 came
  close (one more fix round each); if re-proposed, start from those review findings."
  Git: `2bb2933` (squash, 2026-10-29) and `f42b085` (merge, PR #63) both exist.
  Provenance note: the record cites merge `f42b085`, the transcript cites squash
  `2bb2933` — both real, same PR/date; the subject's provenance is consistent and deeper
  than the record. No fabrication found in either prompt.

## Prompt 1 — the allocator comeback

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Settled decision identified | **2** | "I'm not going to plan this change — it's a settled decision in this repo, ruled out by direct measurement on this exact question"; places it as "first entry under 'Ruled out by measurement'". Verified settled in the memory record. |
| 2 | Evidence quoted, not vibes | **2** | "Latency: identical within noise … end-of-run anon RSS was glibc 26.9 MiB, tcmalloc 36.6, **jemalloc 44.1**, mimalloc 57.5. That's a +17.2 MiB regression for zero measured gain" plus the heaptrack ~100k-startup-allocs / ~30-per-frame mechanism — all verbatim from the record. |
| 3 | Reopening bar stated | **2** | "the only two things that would legitimately re-open it: 1. A new workload that allocates on the frame path … 2. A soak test showing glibc fragmentation", with the proportionate first step ("re-running the `LD_PRELOAD` A/B costs ~10 minutes"). Matches the record's own reopen conditions. |
| 4 | No plan produced for the settled item | **2** | "it doesn't get planned … My recommendation: drop this from next sprint." The only forward content is the conditional 10-minute A/B, which is the reopen bar made concrete — not a migration plan. |
| 5 | Constructive redirect | **2** | "the **shell was not measured** — if the underlying motivation is a shell memory/growth concern, that's a different, unmeasured question" (true per the record) and "If you've seen concrete symptoms … tell me which, and I'll set up the A/B measurement first — evidence before plan." |

Red flags: none hit. No task breakdown for the migration; the re-measure offer names
exactly what would differ this time, so the "re-measure to be sure" cap does not apply.

**Total: 10/10 — PASS.**

## Prompt 2 — reopening a reverted feature

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | Reverted item identified | **2** | "The recorded reversion reasons (PR #63, merged 2026-10-29, squash `2bb2933`) were **not driver defects**" — the feature is placed in the settled record (verified: TODO "Parked" row + raise-the-bar record), weighed, not treated as fresh work. |
| 2 | Original failure remembered accurately | **2** | Quotes both recorded reasons by name — "**Radius 30–32 masked client top corners**", "**GL zeroed arrays per draw**" — diagnoses each as Lumen-side, and builds the whole plan's fix hypotheses and tests around them (D1 pixel test, D2 bench pattern). Not a generic "it was buggy". |
| 3 | Falsifier named | **2** | Rejects the driver premise and names concrete new evidence: "UI engine Phase 5 landed the exact machinery F1 lacked — rounded quads with per-corner radii … analytic corner-radius coverage on the lowered path", plus a measurable test of the caller's own claim ("compare today's SVGA3D GL numbers against the October records rather than asserting improvement") on the deciding backend (Vulkan notebook as the gate). |
| 4 | User-decision framing | **1** | Partial. Good: "Decision points surfaced to you, not decided by the agent" (default on/off "is your call", CSD policy, keep/abandon per feature), "Recommend running it through `lumen-gated-change`" — recommendation, nothing executed. Missing: after correcting the caller's false premise (driver), the agent constructed and endorsed the reopening case itself ("the plan is viable and properly licensed") instead of routing the reopen/no-reopen decision back to the owner with the old evidence and falsifier. It relitigated in the user's favor rather than asking. |
| 5 | No silent plan | **1** | Partial. A full 6-stage implementation plan WAS produced for a feature still reverted in the tree ("the F1 implementation itself is gone"). Mitigating, and why not 0: the plan is not silent — it opens with the settled status and premise correction, is framed as not-agreed-work, is fully gated (BLOCKED-ON-HOST on the VM, Vulkan gate, "Any measured regression → stop, A/B evidence to you"), was explicitly requested by the owner, and the settled record itself carries a verified re-proposal license ("if re-proposed, start from those review findings") with F1 parked as "one fix round each". Still, a complete execution plan pre-empts the owner's reopen decision on the corrected grounds. |

Red flags: none hit. The plan is explicitly NOT premised on "the driver has improved"
("the driver is a side note; the case rests on F1 being one fix round from good plus the
Phase 5 machinery"), so the caps-3-and-4 red flag does not apply.

**Total: 8/10 — PASS.**

## Verdicts

- Prompt 1: **10/10 PASS**
- Prompt 2: **8/10 PASS** (both misses share one root cause: a licensed-but-still-settled
  item was fully relitigated and planned instead of the reopen decision being handed to
  the owner after the premise correction)

## Improvement suggestions for the skill (≤3)

1. **Add a license taxonomy to the settled-decision handling.** Distinguish entries that
   are settled-closed from entries whose record carries an explicit re-entry license
   ("if re-proposed, start from those review findings", TODO "Parked — one fix round
   each"). Rule: a licensed entry may be planned only as a gated recommendation framed
   as not-agreed-work pending the owner's confirmation — the license permits re-proposal,
   it does not permit the agent to relitigate and endorse the reopening itself. This is
   the direct root of both p2 criterion-4/5 misses.
2. **Tie reopening to ask-when-deviating explicitly** (the rubric's own improvement hook
   for criterion 4): add a sentence such as "reopening a settled item is the owner's
   decision — present the old evidence and what would count as new evidence, and ask;
   when the caller's stated grounds are factually wrong (e.g. driver updates for
   Lumen-side defects), correct the premise and ask whether to proceed on the corrected
   grounds before producing any plan."
3. **Cap the deliverable on premise-corrected requests.** When a request rests on a
   false premise but a defensible case exists, define the first deliverable as:
   correction + falsifier + go/no-go question, with at most a sketch of "what a plan
   would have to survive" — not a full staged execution plan that pre-empts the owner's
   decision.

## Transcript quality

Both transcripts carry the verbatim ask and verbatim final answer plus sandbox state —
sufficient to score; the one gap is that intermediate research steps (p2 "researched in
the real repo read-only") are not captured, so I re-verified every checkable claim
(numbers, quotes, TODO placement, both commit hashes) against the repo independently, and
all held — no fabrication in either prompt.
