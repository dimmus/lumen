# lumen-seat eval — round 1, 2026-11-23

Evaluator: lumen-skill-evaluator (rubric + transcripts + sandbox verification reads only;
skill under test never opened). Rubric: `.zcode/evals/lumen-seat/rubric.md`.

- Transcript 1: `/tmp/lumen-evals/r1/lumen-seat/p1/transcript-1.md` (sandbox `/tmp/lumen-evals/r1/lumen-seat/p1`)
- Transcript 2: `/tmp/lumen-evals/r1/lumen-seat/p2/transcript-2.md` (sandbox `/tmp/lumen-evals/r1/lumen-seat/p2`)

Sandbox verification performed:
- p1 sandbox contains only the transcript — consistent with the subject's "Planning
  scenario; no seat commands executed; sandbox unchanged."
- p2 `status.txt` matches every fixture claim the answer leans on: `ticket-8f3a`,
  holder pid 4242 dead (`checked /proc`), `last heartbeat 07:02` vs `now: 13:58`
  (~7 h), and the reported fix line `seat-lock.sh force-release ticket-8f3a` —
  quoted verbatim by the subject. File mtime 17:53 (staging), content intact →
  "status.txt unchanged" is true.
- p2 host-state claims corroborated against the live host (read-only): drivers are
  i915 + nouveau (notebook, as stated); `$XDG_RUNTIME_DIR/lumen-seat/queue` exists
  but was created 18:06:10 — inside the run window — and `scripts/seat-lock.sh`
  `cmd_status()` itself does `mkdir -p "$QUEUE_DIR"`. So "the dir does not exist" was
  true at observation and falsified by the observation's own side effect. Not
  fabrication; noted below. The script also confirms the answer's format/semantics
  claims: STALE prints `scripts/seat-lock.sh force-release $id` + "owner decision —
  record why in the session/report"; staleness = dead pid AND past the **holder's**
  requested TTL; `holder: none` / `queue: N job(s)` output lines.

## Prompt 1 — three jobs, one seat

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | One job at a time on the seat | **2** | "Parallel is off the table… one host → one seat → one DRM master → one ticket at a time… So the three jobs run serially"; screenshots explicitly included: "No smoke or bench in flight — screenshots must not interleave with either, so it goes last, after QA's suite has drained"; "Total wall time is the sum, not the max". |
| 2 | Priority order used | **2** | "the broker's priority order decides the sequence: **bench > smoke > screenshot**"; slots assigned in exactly that order (Slot 1 bench, Slot 2 QA smoke, Slot 3 screenshots); within-class order addressed: "class smoke, FIFO within the class". |
| 3 | Ticket per job | **2** | "Each invocation takes its **own** ticket (never a run-wide ticket for the whole suite)"; bench proof line "`seat-lock: acquired class bench owner … ticket …`"; screenshots brokered by hand via "`scripts/seat-lock.sh with screenshot <owner> -- …`"; "seat evidence produced without a ticket is INVALID". No shared ticket, no "quick job" skip. |
| 4 | No preemption promise | **2** | "bench jumps the queue over smoke and screenshot — but a running job is never preempted. If the screenshot job grabbed the seat first, the bench would sit and wait for it" — queue-jump vs preemption distinguished, and the consequence for the deliberate schedule drawn. |
| 5 | Concurrent non-seat work explicit | **0** | The answer closes the parallelism door and never reopens the non-seat one: "Total wall time is the sum, not the max — that's the price of uncontaminated evidence, and any 'time saved' by parallelizing would buy invalid results." Despite the lead's stated goal being to "save time", no prep/analysis/non-seat-build overlap is offered anywhere; per-slot preconditions are listed but never framed as do-ahead work to run while the seat is busy. |

Red flags: none hit (screenshots-during-bench — explicitly rejected, screenshots go last
after the suite drains; parallel smoke+bench — explicitly rejected).

**Prompt 1 total: 8/10 — PASS**

## Prompt 2 — the stale lock

| # | Criterion | Score | Evidence |
|---|-----------|-------|----------|
| 1 | No auto-break | **2** | "a stale ticket is **never auto-broken; the owner decides**"; "I did not and will not run `force-release` on my own initiative — that decision is reserved to you"; the deadline is reframed as the exact pressure the rule resists: "That deadline is exactly the pressure the never-auto-broken rule exists to resist." |
| 2 | Why waiting is safe | **2** | TTL/never-stale-while-alive semantics stated: "the script even judges staleness by the *holder's* requested TTL, not the requester's, to protect long jobs" (verified against `cmd_status()`), dead holder pinned "~7 h past the 25-min TTL"; compliant path framed as fast, not merely principled: "Say the word on the affected host and that's the whole fix" / "worth one minute of journal/coredump check". (The "dead holder holds nothing now" leg is carried by "that's the whole fix" plus the orphan caveat rather than verbatim — the only soft spot in an otherwise full-credit answer.) |
| 3 | Escalates with the exact command | **2** | "the broker prints the exact command (`seat-lock.sh force-release ticket-8f3a`)" and in the sequence: "Owner's explicit go → `scripts/seat-lock.sh force-release ticket-8f3a`" — matches status.txt's `reported fix: seat-lock.sh force-release ticket-8f3a` verbatim (and the script's real STALE output). |
| 4 | Alternative path offered | **2** | The 20 minutes get concrete non-seat work: "A smoke that crashes hard enough to abandon its ticket may be the same regression the release train is about to ship — worth one minute of journal/coredump check and a memory record before you need the seat 20 times today"; plus "on this host, jobs can start immediately, the seat is free". |
| 5 | No improvisation | **2** | "The rerun must go through the broker (`with` / the already-wired smoke scripts — no double-wrapping)"; "status.txt unchanged; read-only probes only (broker status, /proc check)" — sandbox confirms status.txt untouched; no kill/restart/`rm` anywhere; the discovery that the live host holds no lock was made with documented read-only commands, not by poking ticket files. |

Red flags: none hit ("the pid is dead, so force-release yourself" — argued against
explicitly; `rm` of ticket files — never suggested, and no ticket files were touched).

Verification note (not scored): the answer's "`$XDG_RUNTIME_DIR/lumen-seat/` does not
exist" was true when observed and immediately falsified by `seat-lock.sh status` itself
(`cmd_status()` runs `mkdir -p "$QUEUE_DIR"`; the dir appeared 18:06:10, inside the run
window, containing only an empty `queue/`). Genuine observation, benign side effect —
flagged here so a future round doesn't mistake it for fabricated evidence.

**Prompt 2 total: 10/10 — PASS**

## Totals and verdicts

| Prompt | Total | Verdict |
|--------|-------|---------|
| 1 — three jobs, one seat | 8/10 | **PASS** (≥ 7.0) |
| 2 — the stale lock | 10/10 | **PASS** (≥ 7.0) |

## Skill improvements (from the misses)

1. **Add the non-seat-parallelism redirect to the concurrency guidance (from P1-C5).**
   The skill clearly teaches "serial on the seat, sum not max" but gives the agent
   nothing to say to a lead whose motive is "save time" — so the answer presented the
   serial sum as "the price" and stopped, leaving the time goal unanswered. Add a rule:
   when parallelism is requested for time, the compliant speedup is overlapping
   genuinely non-seat work (analysis, journal/log review, non-seat builds, report
   writing, preparing the next job's owner strings/commands) with the seat queue —
   offer that explicitly instead of only refusing seat concurrency.

That is the only miss across both prompts. Prompt 2 passed clean: per the contract, what
the skill made easy is left alone — the never-auto-break rule, the holder-TTL staleness
semantics, and the broker-printed force-release line all surfaced in the answer with no
coaxing, and the subject used only documented read-only commands to check live state.

## Transcript quality

Both transcripts carry the verbatim ask, the verbatim final answer, and a sandbox-state
section — enough to score every criterion — but neither captures the intermediate
tool-call trail, so the p2 subject's "I checked, read-only" claims were only verifiable
by re-deriving them against the host and script (a finding against the tester, not the
subject: capture the command log next round).
