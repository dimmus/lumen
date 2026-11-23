/* zcode-workflow
description: "Lumen's development studio orchestrator: plan a request into gated
  tasks, run each in its own git worktree (test-enabled baseline → implement
  with logical commits → build+test gates → independent review → per-finding
  confirmation (pre-existing defects recorded, not fixed) → fixes → real-seat
  smokes → evidence gate → memory record), assemble a PR description per task,
  then merge every gate-READY branch into main (topological order, merged tree
  re-gated) and push. Anything below READY is delivered as branch + evidence
  for the owner — never merged, never pushed."
whenToUse: "A non-trivial Lumen change or multi-task request that must go through
  the full gated studio (plan / implement / review / verify / gate / memory), or
  when the user asks for the studio or coordinated delivery on Lumen. One
  bounded task → lumen-gated-change instead; cross-compositor benchmarking →
  lumen-compositor-bench."
args:
  app_smoke_tolerated_failset:
    type: json
    description: 'Row names app-smoke may FAIL on this host without it being the
      change''s fault (json array, e.g. the notebook''s VM-geometry rows).
      Tolerance is by exact row NAME, never by pass-count floor — a change that
      breaks exactly one new row never clears a real failing-set comparison.
      Rows in the set that PASS are reported as improvements. Empty = the exit
      code decides.'
    default: "[]"
  base:
    type: string
    description: Git ref (commit/branch) each task worktree is cut from
    default: HEAD
  branch_prefix:
    type: string
    description: Branch prefix; each task gets <prefix>/<task-id> (must not already exist)
    default: zcode/task
  known_red:
    type: json
    description: 'Test names tolerated in a red baseline (json array, e.g.
      ["some_red_test"]): tasks proceed and every suite gate then
      requires no NEW failures beyond this set. Failures outside the set park
      the run on an owner decision (proceed-and-tolerate vs skip). This is an
      owner sign-off item: the gate lists every non-empty set in its reasons.'
    default: "[]"
  max_tasks:
    type: number
    description: Cap on planned tasks executed by one run (rest are reported as not covered)
    default: 5
  run_bench:
    type: boolean
    description: "Also run scripts/ui-bench.sh even in tasks whose diff does not
      require a bench. ADDITIVE only: the classifier (scripts/classify-change.py)
      derives required evidence from each task's diff and an operator knob can
      add evidence, never waive it."
    default: false
  run_smokes:
    type: boolean
    description: "Run drag-smoke and app-smoke with --join-seat-session even in
      tasks whose diff does not require them (additive — required seat evidence
      cannot be waived). The smoke/bench scripts take the seat-broker ticket
      themselves (scripts/seat-lock.sh; the lumen-seat skill)."
    default: true
  smoke_env:
    type: json
    description: 'Extra KEY=VALUE env for the seat smokes (json array of strings).
      Host recipe — vmware VM: [] (defaults are right); iritur notebook:
      ["LUMEN_SMOKE_SCALE=1","LUMEN_INJECT_POINTER=/dev/input/eventN","LUMEN_INJECT_RELATIVE=/dev/input/eventN","LUMEN_INJECT_KEYBOARD=/dev/input/event3"]
      where eventN is the virtual ABS mouse held open by
      scripts/uinput-mouse.py.'
    default: "[]"
  task:
    type: string
    description: "The request to deliver: what you would tell the studio (feature,
      protocol, fix, optimization)"
    required: true
*/
// Scope: current-project — the pipeline shape is portable; commands, scripts and hosts are Lumen's.
interface TaskCard {
  /** Short kebab-case id, unique across this plan, e.g. "toplevel-drag-protocol". */
  id: string;
  /** One line: what this task delivers. */
  title: string;
  /** Paths/modules this task may write; disjoint from sibling tasks. */
  scope: string[];
  /** Observable criteria that decide acceptance. */
  acceptance: string[];
  /** Ids of tasks that must finish before this one; the plan lists tasks in execution order. */
  dependsOn: string[];
  /** Risks, settled decisions and host limits the implementer must know. */
  notes: string[];
}

interface Plan {
  /** The objective restated in one or two sentences. */
  objective: string;
  /** Tasks in topological execution order. */
  tasks: TaskCard[];
  /** Explicitly out of scope. */
  notInScope: string[];
}

interface ImplResult {
  /** 2-5 sentences: what was implemented and the shape of the solution. */
  summary: string;
  /** Workspace-relative paths created or modified. */
  changedFiles: string[];
  /** One "hash subject" line per logical commit made in the worktree. */
  commits: string[];
  /** Tests added or extended, one line each ("none" is a deviation to explain). */
  testsAdded: string[];
  /** What the reviewer should look at hardest, and why. */
  notesForReviewer: string;
  /** Deviations from the task card or "none". */
  deviations: string;
}

interface Finding {
  /** Workspace-relative path with line when it applies: "modules/lx.gfx/atlas.cppm:42". */
  where: string;
  /** One sentence: what is wrong, not how to fix it. */
  what: string;
  /** The lines read that show it, quoted. */
  evidence: string;
  /** Reserve "high" for a crash, data loss, or a regression of measured behaviour. */
  severity: "low" | "medium" | "high";
  /** True when the defect exists at the diff base — the confirmer decides it from the diff. */
  preexisting?: boolean;
}

interface ReportFinding extends Finding {
  /** "verified" when independently confirmed or decided by a command; "unconfirmed" when confirmation failed. */
  status: "verified" | "unconfirmed";
  /** What happened to it; every finding leaves the run with exactly one (unset ones are
      normalized to deferred-to-user before the report is written). */
  disposition?: "fixed" | "refuted" | "pre-existing" | "deferred-to-user";
}

interface Review {
  /** One-paragraph verdict: is the change ready for verify/gate, and what blocks it. */
  verdict: string;
  findings: Finding[];
}

interface Confirmation {
  /** True when the finding reproduces from its evidence alone. */
  holds: boolean;
  /** True when the defect exists at the diff base — the diff does not touch the cited lines. */
  preexisting: boolean;
  /** What you read (or ran) to decide, in one or two sentences. */
  reason: string;
}

interface FixReport {
  /** What was fixed and how, 2-4 sentences. */
  summary: string;
  /** One "hash subject" line per fix commit. */
  commits: string[];
  /** True when the failure is NOT patchable (wrong approach, invariant conflict) and the
      task should route back to planning instead of burning another fix round. */
  designLevel?: boolean;
}

interface GateVerdict {
  /** READY only when acceptance is met with independent evidence and nothing regressed. */
  verdict: "READY" | "BLOCKED" | "NOT_READY" | "NEEDS_REDESIGN";
  /** Each reason one sentence; cites the evidence it rests on. */
  reasons: string[];
}

/** Independent read of the evidence by the analyst stage (never the runner). */
interface Analysis {
  /** One paragraph: what the evidence establishes and what it does not. */
  verdict: string;
  /** Coverage gaps and provenance problems, one sentence each (may be empty). */
  gaps: string[];
}

/** What scripts/classify-change.py derives from a task's diff. */
interface RequiredEvidence {
  classes: string[];
  required: { ctest: boolean; smokes: boolean; bench_ab: boolean; goldens: boolean };
  render_path: boolean;
  decisive_backend_blocked_on_host: boolean;
  host: string;
}

interface MemoryRecord {
  /** Path of the memory file written under /home/dimmus/lumen/.zcode/memories/. */
  file: string;
  /** One line: what the record preserves. */
  summary: string;
}

interface WorkflowReport {
  /** Two or three sentences answering what the user asked for. */
  conclusion: string;
  findings: ReportFinding[];
  /** What the run checked and how: the commands it ran. */
  verified: string[];
  /** What the run did not check, and why. */
  notCovered: string[];
}

/** What the owner decided to do about a busy seat: freed it, or smoke-less delivery. */
type SeatPlan = "freed" | "skip";

const task = String(args.task ?? "");
const branchPrefix = args.branch_prefix === undefined ? "zcode/task" : String(args.branch_prefix);
const runSmokes = args.run_smokes !== false;
const runBench = args.run_bench === true;
const maxTasks = typeof args.max_tasks === "number" ? args.max_tasks : 5;
const knownRed: string[] = Array.isArray(args.known_red) ? args.known_red.map(String) : [];
const smokeEnv: string[] = Array.isArray(args.smoke_env) ? args.smoke_env.map(String) : [];
const envPrefix = smokeEnv.length > 0 ? smokeEnv.join(" ") + " " : "";
const toleratedFailset: string[] = Array.isArray(args.app_smoke_tolerated_failset)
  ? args.app_smoke_tolerated_failset.map(String)
  : [];

// Derived, never hardcoded to one machine's home: the main checkout is where
// world.run's cwd is; the worktree root is its sibling 'lumen-wt' directory
// (env override LUMEN_WT_ROOT), shared with lumen-gated-change.
const pwdRun = await world.run("pwd", [], { output: { tail: 512 } });
const MAIN_WS = pwdRun.stdout.trim().split("\n").pop() ?? ".";
// Resolve the base arg to a CONCRETE SHA in the MAIN checkout before any
// worktree exists: the symbolic default "HEAD" resolves to the TASK BRANCH
// inside every later 'git -C <wt>' command (diff HEAD..HEAD = nothing, and
// the resume detector would count zero unique commits on a leftover branch).
const baseRaw = args.base === undefined ? "HEAD" : String(args.base);
const baseResolved = baseRaw === "HEAD"
  ? (await world.run("git", ["rev-parse", "HEAD"], { output: { tail: 512 } })).stdout.trim().split("\n").pop() ?? "HEAD"
  : baseRaw;
const wtRootRun = await world.run(
  "bash",
  ["-c", `echo "\${LUMEN_WT_ROOT:-$(dirname '${MAIN_WS}')/lumen-wt}"`],
  { output: { tail: 512 } },
);
const WT_ROOT = wtRootRun.stdout.trim().split("\n").pop() ?? "../lumen-wt";

/** Test names ctest reported under "The following tests FAILED:" (empty when it never got there). */
function failedTests(out: string): string[] {
  const marker = out.indexOf("The following tests FAILED:");
  if (marker < 0) return [];
  const names: string[] = [];
  for (const line of out.slice(marker).split("\n").slice(1)) {
    const m = /^\s*-?\s*\d+ - (\S+) \(/.exec(line);
    if (m !== null && m[1] !== undefined) names.push(m[1]);
  }
  return names;
}

/** "Total Tests: N" from ctest -N — the tree's own computed count, never quoted from memory. */
function ctestTotal(out: string): number {
  const m = /Total Tests: (\d+)/.exec(out);
  return m !== null && m[1] !== undefined ? Number(m[1]) : -1;
}

/** app-smoke failing row names, e.g. ["Z2", "N8"] (empty when none). */
function smokeFailRows(out: string): string[] {
  const names: string[] = [];
  for (const line of out.split("\n")) {
    const m = /^FAIL\s+(\S+)/.exec(line);
    if (m !== null && m[1] !== undefined) names.push(m[1]);
  }
  return names;
}

if (task === "") {
  const empty: WorkflowReport = {
    conclusion: "No task was given (argument 'task' is required); nothing ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — the task argument was empty"],
  };
  return empty;
}

// One studio run per worktree root (scripts/run-lock.sh): two concurrent runs
// collide on <WT_ROOT>/<task-id> worktrees and on the main checkout's memory
// commits. Heartbeat at every task; stale (no heartbeat for 6 h) is an owner
// decision with the exact force-release command — never auto-broken.
phase("Take the run lock");
const runOwner = `studio ${task.split("\n")[0].slice(0, 60)}`;
const runLock = await world.run("bash", ["-c", `exec '${MAIN_WS}/scripts/run-lock.sh' acquire '${runOwner.replace(/'/g, "")}'`], { output: { tail: 2048 } });
if (runLock.exitCode !== 0) {
  const busy: WorkflowReport = {
    conclusion:
      "Another studio run holds the run lock for this worktree root (or a stale lock needs an owner decision). " +
      `${(runLock.stderr + runLock.stdout).trim().split("\n").slice(0, 2).join(" — ")}`,
    findings: [],
    verified: ["preflight: run-lock check ran"],
    notCovered: ["everything — the run lock is held"],
  };
  return busy;
}
/** Releases the run lock before every workflow exit path. */
const finishRun = async (r: WorkflowReport): Promise<WorkflowReport> => {
  await world.run("bash", ["-c", `exec '${MAIN_WS}/scripts/run-lock.sh' release '${runOwner.replace(/'/g, "")}'`], { output: { tail: 1024 } });
  return r;
};

// The role contracts live in .zcode/agents/*.md — the single source of truth
// (agents and workflows drifted before; the files are binding, the personas
// below only orient and point there).
const plannerPersona =
  "You are the planning stage of Lumen's development studio. Your full contract is " +
  ".zcode/agents/lumen-planner.md — read it first, it is binding (disjoint scopes, topological " +
  "order, verification per task, host limits). Planning is read-only.";

const implementerPersona =
  "You are the implement stage of Lumen's development studio, working in ONE assigned git worktree. " +
  "Your full contract is .zcode/agents/lumen-implementer.md — read it first, it is binding " +
  "(scope, conventions, tests for new functionality, one logical commit per unit with a detailed " +
  "message, stop-on-regression, no headless seat claims, clang-format + diff hygiene).";

const reviewerPersona =
  "You are the review stage of Lumen's development studio, reading a diff you did not write. " +
  "Your full contract is .zcode/agents/lumen-reviewer.md — read it first, it is binding " +
  "(base-ref policy, memory traps, the mechanical checks, the minimal reading list every finding " +
  "must carry, the test-quality bar). You are read-only.";

const confirmerPersona =
  "You are an independent verifier in Lumen's development studio: a bounded auditor, not a second " +
  "reviewer. Your full contract is .zcode/agents/lumen-confirmer.md — read it first, it is binding. " +
  "At most ONE targeted follow-up beyond the cited lines; an honest inconclusive beats a grind. " +
  "You are read-only.";

const fixerPersona =
  "You fix build failures, test failures and confirmed review findings in ONE assigned Lumen git " +
  "worktree. Your full contract is .zcode/agents/lumen-fixer.md — read it first, it is binding. " +
  "Minimal diffs; never weaken a test to pass; say 'design-level' when a failure is not patchable " +
  "so the task routes back to planning instead of burning another round.";

const gatekeeperPersona =
  "You are the final gate of Lumen's development studio for ONE task. Your full contract is " +
  ".zcode/agents/lumen-gatekeeper.md — read it first, it is binding: derived evidence cannot be " +
  "waived, counts are computed from the tree's own ctest -N, seat evidence is brokered, " +
  "BLOCKED-ON-HOST applies to render-path changes on hosts without hardware Vulkan, non-default " +
  "tolerance knobs are owner sign-off items, and the verdicts are READY / NOT_READY / BLOCKED / " +
  "NEEDS_REDESIGN (design-level findings route the task back to planning). You are read-only.";

const analystPersona =
  "You are the evidence-analysis stage of Lumen's development studio: you read the checks' " +
  "output and say what it does and does not establish. Your full contract is " +
  ".zcode/agents/lumen-test-analyst.md — read it first, it is binding (provenance, " +
  "coverage vs the diff, environmental-vs-change failure diagnosis, A/B verdicts, honest " +
  "gaps). You did not run the checks — that independence is your value. You are read-only.";

const memoryPersona =
  "You write Lumen's project memory records. Your full contract is .zcode/agents/lumen-memory-writer.md " +
  "and .zcode/skills/lumen-memory-writer/SKILL.md — read them first, they are binding. Work in the " +
  "MAIN checkout at " + MAIN_WS + " (not a task worktree). Frontmatter summary/modules/topics " +
  "(supersedes when replacing), ≤200-char index row, the index regenerated with " +
  "scripts/gen-memory-index.py, the two-strikes Rule-proposal line for repeated traps, and the " +
  "commit rule: on main directly, or SPOOLED to refs/heads/zcode/memory-spool when the main " +
  "checkout sits on any other branch (a memory committed onto a feature branch is lost on switch " +
  "back). When the task packet lists preexistingFindings, add one open row each to .zcode/TODO.md.";

const verified: string[] = [];
const notCovered: string[] = [
  "the decisive present backend is not verified by this run — name it from the smoke logs before quoting one (notebook: hasvk Vulkan; vmware VM: GL path, no hardware Vulkan)",
];
const allFindings: ReportFinding[] = [];
const prSections: string[] = [];
const readyBranches: string[] = [];
/** Per-task branch and gate verdict, so dependent tasks can chain onto READY predecessors. */
const branchOf = new Map<string, string>();
const verdictOf = new Map<string, string>();
/** Per-task computed suite count (ctest -N at baseline) — the gate compares against it. */
const baselineCountOf = new Map<string, number>();

artifact.board("tasks", {
  title: "Studio tasks",
  key: "id",
  status: "state",
  columns: ["planned", "baseline", "implementing", "review", "verify", "gate", "done", "blocked", "redesign"],
  cardTitle: "title",
  detail: [{ field: "branch", label: "branch" }, { field: "verdict", label: "gate" }],
});

phase("Plan the request into gated tasks");
const plan = await agent("Planner", { system: plannerPersona }).ask<Plan>(
  `Turn this Lumen request into a minimal set of gated tasks:\n\n${task}\n\n` +
    `The main checkout is at ${MAIN_WS}. Answer with the objective, the ordered task cards, and what is out of scope.`,
);
const tasks = plan.tasks.slice(0, maxTasks);
if (plan.tasks.length > maxTasks) {
  notCovered.push(`${plan.tasks.length - maxTasks} planned task(s) beyond max_tasks=${maxTasks}: ` +
    plan.tasks.slice(maxTasks).map((t) => t.title).join("; "));
}
log(`Plan: ${tasks.length} task(s) — ${plan.objective}`);

let smokesEnabled = runSmokes;
if (runSmokes) {
  phase("Check the seat is free for smoke verification (the broker owns the check)");
  // One implementation of the quiet-seat triple lives in scripts/seat-lock.sh
  // (the lumen-seat skill); the smoke/bench scripts take their own per-job
  // tickets at run time. This is the run-level advisory look ahead of them.
  const seat = await world.run("bash", ["-c", `exec '${MAIN_WS}/scripts/seat-lock.sh' status`], {
    output: { tail: 4096 },
  });
  const busyLines = seat.stdout.trim().split("\n").filter((l) => l.startsWith("FAIL:"));
  if (seat.exitCode !== 0 && busyLines.length > 0) {
    const seatPlan = await hole<SeatPlan>(
      "Free the seat for smoke verification",
      `The seat broker reports the seat busy (one DRM master at a time):\n${busyLines.join("\n")}\n` +
        `Broker status:\n${seat.stdout.trim().split("\n").slice(0, 4).join("\n")}\n` +
        `Return "freed" once the owner has freed the seat (the run then proceeds with smokes), or "skip" to ` +
        `deliver without seat smokes — gates will record the missing seat evidence honestly.`,
    );
    if (seatPlan === "skip") smokesEnabled = false;
  } else {
    log("Seat is free for smoke verification");
  }
}

for (const card of tasks) {
  // Run-lock heartbeat at every task boundary (a live run never goes stale);
  // owner-checked so a racing second run cannot keep the first's lock alive.
  await world.run("bash", ["-c", `exec '${MAIN_WS}/scripts/run-lock.sh' heartbeat '${runOwner.replace(/'/g, "")}'`], { output: { tail: 512 } });

  const wt = `${WT_ROOT}/${card.id}`;
  const branch = `${branchPrefix}/${card.id}`;
  verdictOf.set(card.id, "BLOCKED");

  // Chain onto ALL READY dependencies: a task with two predecessors integrates
  // BOTH branches — basing on the last one alone silently dropped the other's
  // commits and gated a tree that was never tested.
  const depIds = card.dependsOn;
  const unreadyDeps = depIds.filter((d) => verdictOf.get(d) !== "READY");
  if (unreadyDeps.length > 0) {
    allFindings.push({
      where: `task ${card.id}`,
      what: `dependency not READY: ${unreadyDeps.join(", ")} — this task is skipped, nothing lost`,
      evidence: `dependsOn ${JSON.stringify(depIds)}`,
      status: "verified",
      severity: "medium",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "dependency" }, "tasks");
    continue;
  }
  const firstDep = depIds.length > 0 ? depIds[0] : undefined;
  const effBase = firstDep !== undefined ? (branchOf.get(firstDep) ?? baseResolved) : baseResolved;
  const extraDeps = depIds.slice(1).map((d) => branchOf.get(d)).filter((b): b is string => b !== undefined);

  phase("Set up the worktree and test-enabled baseline");
  report({ id: card.id, title: card.title, branch, state: "baseline", verdict: "" }, "tasks");
  let haveWt = false;
  let resumeMode = false;
  const add = await world.run("git", ["worktree", "add", wt, "-b", branch, effBase], { output: { tail: 2048 } });
  if (add.exitCode === 0) {
    haveWt = true;
  } else {
    // The add failed. Reuse ONLY what is provably this task's own leftover: the
    // worktree must be on the expected branch (a foreign branch's commits are
    // not this task's implementation), and a hard reset is only safe when the
    // branch tip is already contained in the effective base (nothing to lose).
    const onBranch = await world.run("git", ["-C", wt, "branch", "--show-current"], { output: { tail: 512 } });
    const leftover = await world.run("git", ["-C", wt, "status", "--porcelain"], { output: { tail: 2048 } });
    const unique = await world.run("git", ["rev-list", "--count", `${effBase}..${branch}`], { output: { tail: 1024 } });
    const uniqueCount = unique.exitCode === 0 ? Number(unique.stdout.trim()) : Number.NaN;
    const isTaskBranch = onBranch.exitCode === 0 && onBranch.stdout.trim() === branch;
    if (isTaskBranch && leftover.exitCode === 0 && leftover.stdout.trim() === "" && uniqueCount === 0) {
      const contained = await world.run("git", ["-C", wt, "merge-base", "--is-ancestor", "HEAD", effBase], { output: { tail: 512 } });
      if (contained.exitCode === 0) {
        const reset = await world.run("git", ["-C", wt, "reset", "--hard", effBase], { output: { tail: 1024 } });
        haveWt = reset.exitCode === 0;
        if (haveWt) log(`Task ${card.id}: reusing existing worktree (tip contained in ${effBase}), branch reset`);
      }
    } else if (isTaskBranch && leftover.exitCode === 0 && leftover.stdout.trim() === "" && uniqueCount > 0) {
      haveWt = true;
      resumeMode = true;
      verified.push(`task ${card.id}: resuming an already-implemented branch (${uniqueCount} commit(s) beyond ${effBase}; baseline was proven in the earlier run)`);
      log(`Task ${card.id}: resume mode — branch already holds ${uniqueCount} commit(s)`);
    }
  }
  if (!haveWt) {
    allFindings.push({
      where: `task ${card.id}`,
      what: `could not create worktree/branch ${branch} at ${wt}: ${add.stderr.trim().split("\n").pop() ?? ""}`,
      evidence: `git worktree add ${wt} -b ${branch} ${effBase} → exit ${add.exitCode}`,
      status: "verified",
      severity: "medium",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "worktree" }, "tasks");
    continue;
  }
  // Diamond dependencies: integrate every additional predecessor branch. A
  // conflict between READY predecessors is a planning failure — record it,
  // park the task, and let the owner re-plan; never auto-resolve.
  let depConflict = false;
  for (const depBranch of extraDeps) {
    const merge = await world.run("git", ["-C", wt, "merge", "--no-edit", depBranch], { output: { tail: 4096 } });
    if (merge.exitCode !== 0) {
      await world.run("git", ["-C", wt, "merge", "--abort"], { output: { tail: 1024 } });
      depConflict = true;
      allFindings.push({
        where: `task ${card.id}`,
        what: `predecessor branches ${branchOf.get(depIds[0] ?? "") ?? effBase} and ${depBranch} conflict — the plan's tasks are not independently integrable; re-plan needed`,
        evidence: `git -C ${wt} merge --no-edit ${depBranch} → exit ${merge.exitCode} (aborted)`,
        status: "verified",
        severity: "high",
      });
      break;
    }
    verified.push(`task ${card.id}: integrated predecessor ${depBranch} (diamond base)`);
  }
  if (depConflict) {
    report({ id: card.id, title: card.title, branch, state: "redesign", verdict: "predecessor conflict" }, "tasks");
    continue;
  }
  await world.run(
    "bash",
    ["-c", `test -d '${wt}/protocols/upstream' || cp -r protocols/upstream '${wt}/protocols/'`],
    { output: { tail: 1024 } },
  );
  if (!resumeMode) {
  const configure = await world.run(
    "cmake",
    ["--preset", "release", "-S", wt, "-B", `${wt}/build/test`, "-DLUMEN_BUILD_TESTS=ON"],
    { timeoutMs: 600_000, output: { tail: 4096 } },
  );
  const baselineBuild = configure.exitCode === 0
    ? await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], { timeoutMs: 1_800_000, output: { tail: 32_768 } })
    : undefined;
  const baselineTests = baselineBuild !== undefined && baselineBuild.exitCode === 0
    ? await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } })
    : undefined;
  if (baselineTests !== undefined && baselineTests.exitCode !== 0) {
    // One rerun discounts flakes; a persistent failure set is either tolerated (known_red)
    // or escalated to the owner — never silently accepted, never silently skipped past.
    const rerun = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } });
    const names = rerun.exitCode === 0 ? [] : failedTests(rerun.stderr + rerun.stdout);
    const fresh = names.filter((n) => !knownRed.includes(n));
    if (fresh.length === 0) {
      allFindings.push({
        where: `task ${card.id}`,
        what: `baseline red only in the tolerated known-red set (${names.join(", ")}) — gates now require no NEW failures beyond it`,
        evidence: `baseline rerun at ${wt}/build/test: ${names.join(", ")}`,
        status: "verified",
        severity: "medium",
      });
    } else {
      // A parked hole is visible on the board, not just in the transcript.
      report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "awaiting owner: red baseline" }, "tasks");
      const proceed = await hole<boolean>(
        "Decide whether to proceed on a red baseline",
        `The unchanged baseline at ${wt} fails ${names.length} test(s): ${names.join(", ")}. None are in the ` +
          `tolerated set (${knownRed.length > 0 ? knownRed.join(", ") : "empty"}). Tail:\n` +
          `${(rerun.stderr + rerun.stdout).slice(-1500)}\n` +
          `Return true to tolerate these failures for the rest of this run (they join the known-red set, and ` +
          `gates require no NEW failures beyond it); false to skip this task with a red-baseline block.`,
      );
      if (proceed) {
        allFindings.push({
          where: `task ${card.id}`,
          what: `owner tolerated a red baseline (${names.join(", ")}) — gates now require no NEW failures beyond this set`,
          evidence: `baseline rerun at ${wt}/build/test`,
          status: "verified",
          severity: "medium",
        });
        knownRed.push(...fresh);
      } else {
        allFindings.push({
          where: `task ${card.id}`,
          what: `red baseline (${names.join(", ")}) — per the no-degradation rule there is no clean baseline to gate against; task skipped`,
          evidence: `baseline rerun at ${wt}/build/test: ${names.join(", ")}`,
          status: "verified",
          severity: "high",
        });
        report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "red baseline" }, "tasks");
        continue;
      }
    }
  }
  if (baselineTests === undefined) {
    const why = configure.exitCode !== 0
      ? "configure failed"
      : (baselineBuild === undefined || baselineBuild.exitCode !== 0 ? "baseline build failed" : "baseline ctest failed");
    allFindings.push({
      where: `task ${card.id}`,
      what: `red baseline (${why}) — per the no-degradation rule there is no clean baseline to gate against; task skipped`,
      evidence: `worktree at ${wt}, base ${baseResolved}; ${why}`,
      status: "verified",
      severity: "high",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "red baseline" }, "tasks");
    continue;
  }
  verified.push(`task ${card.id}: tests-enabled baseline green in worktree (configure+build+ctest at ${wt}/build/test)`);
  const baseCountRun = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "-N"], { output: { tail: 2048 } });
  const baseCount = ctestTotal(baseCountRun.stdout + baseCountRun.stderr);
  baselineCountOf.set(card.id, baseCount);
  if (baseCount >= 0) log(`Task ${card.id}: baseline registers ${baseCount} tests (ctest -N)`);
  log(`Task ${card.id}: baseline green`);
  }

  phase("Implement the task in its worktree");
  report({ id: card.id, title: card.title, branch, state: "implementing", verdict: "" }, "tasks");
  const implementer = agent(`Implementer ${card.id}`, { system: implementerPersona });
  const impl = resumeMode
    ? await implementer.ask<ImplResult>(
        `This task was already implemented on branch ${branch} in the worktree ${wt} (resuming an interrupted run). ` +
          `Do NOT edit anything. Read the commits (git -C ${wt} log --oneline ${effBase}..HEAD and their diffs) and the ` +
          `task card below, then answer the standard handoff: summary, changed files, commits, tests added, notes for ` +
          `the reviewer, and deviations.\n\nTask card:\n${JSON.stringify(card, null, 2)}`,
      )
    : await implementer.ask<ImplResult>(
    `Implement this task in the Lumen worktree at ${wt} (branch ${branch}, cut from ${effBase}). ` +
      `Build directory: ${wt}/build/test (tests-enabled). Commit every logical unit; never push.\n\n` +
      `Task card:\n${JSON.stringify(card, null, 2)}\n\nPlan objective: ${plan.objective}\n` +
      `After implementing, commit anything left over and answer with your summary, changed files, ` +
      `commits, tests added, notes for the reviewer, and deviations.`,
  );
  const porcelain = await world.run("git", ["-C", wt, "status", "--porcelain"], { output: { tail: 2048 } });
  if (porcelain.exitCode === 0 && porcelain.stdout.trim() !== "") {
    await implementer.ask(
      `The worktree still has uncommitted changes:\n${porcelain.stdout}\nCommit them as logical units ` +
        `(or revert scratch), then confirm; reply with the final commit list only.`,
    );
  }
  const commitList = await world.run("git", ["-C", wt, "log", "--oneline", `${effBase}..HEAD`], { output: { tail: 4096 } });
  log(`Task ${card.id}: implemented, ${impl.commits.length} commit(s)`);

  // The diff itself decides what evidence this task's gate must see — the
  // run_smokes/run_bench knobs can add, never waive (lumen-gatekeeper).
  const classifyRun = await world.run(
    "python3",
    [`${MAIN_WS}/scripts/classify-change.py`, "--repo", wt, "--base", effBase],
    { output: { tail: 8192 } },
  );
  let required: RequiredEvidence = {
    classes: [],
    required: { ctest: true, smokes: false, bench_ab: false, goldens: false },
    render_path: false,
    decisive_backend_blocked_on_host: false,
    host: "unknown",
  };
  if (classifyRun.exitCode === 0) {
    try {
      required = JSON.parse(classifyRun.stdout) as RequiredEvidence;
    } catch {
      log(`Task ${card.id}: classifier output unparseable — ctest-only required evidence`);
    }
  } else {
    log(`Task ${card.id}: classifier failed — ctest-only required evidence`);
  }
  verified.push(
    `task ${card.id}: required evidence derived from the diff: classes [${required.classes.join(", ")}] → ` +
      `smokes=${required.required.smokes} bench=${required.required.bench_ab} goldens=${required.required.goldens} (host ${required.host})`,
  );
  if (required.decisive_backend_blocked_on_host) {
    notCovered.push(
      `task ${card.id}: render-path change on a host without hardware Vulkan — BLOCKED-ON-HOST at the gate; a notebook verification ticket is required before merge`,
    );
  }

  phase("Build and test the change");
  const fixer = agent(`Fixer ${card.id}`, { system: fixerPersona, budget: { webSearch: 0 } });
  let gateGreen = false;
  let gateRounds = 0;
  let designEscalations = 0;
  for (let round = 1; round <= 3 && !gateGreen; round++) {
    gateRounds = round;
    const build = await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], { timeoutMs: 1_800_000, output: { tail: 32_768 } });
    if (build.exitCode !== 0) {
      const fixRep = await fixer.ask<FixReport>(
        `The build failed (round ${round}) in the worktree ${wt}. Fix it with minimal changes and commit; ` +
          `set designLevel=true ONLY if the failure is not patchable (wrong approach, invariant conflict). Tail:\n` +
          `${(build.stderr + build.stdout).slice(-4000)}`,
      );
      if (fixRep.designLevel === true) designEscalations++;
      continue;
    }
    const tests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } });
    if (tests.exitCode !== 0) {
      const fresh = failedTests(tests.stderr + tests.stdout).filter((n) => !knownRed.includes(n));
      if (fresh.length > 0) {
        const fixRep = await fixer.ask<FixReport>(
          `The tests-enabled suite failed (round ${round}) in ${wt}; failing: ${fresh.join(", ")} (the tolerated ` +
            `known-red set ${knownRed.length > 0 ? knownRed.join(", ") : "is empty"} does not cover these). Fix the ` +
            `cause with minimal changes and commit — never weaken a test; set designLevel=true ONLY if the failure ` +
            `is not patchable. Tail:\n${(tests.stderr + tests.stdout).slice(-4000)}`,
        );
        if (fixRep.designLevel === true) designEscalations++;
        continue;
      }
      // only tolerated failures remain — green modulo the known-red baseline set
    }
    gateGreen = true;
  }
  // Two design-level escalations = the approach, not the patch, is wrong:
  // park the task for re-planning instead of burning the remaining budget
  // against a stale plan.
  if (!gateGreen && designEscalations >= 2) {
    allFindings.push({
      where: `task ${card.id}`,
      what: `the fixer escalated ${designEscalations} design-level failures — the approach needs re-planning (NEEDS_REDESIGN), not more fix rounds`,
      evidence: `${gateRounds} round(s) at ${wt}/build/test with designLevel=true twice`,
      status: "verified",
      severity: "high",
    });
    report({ id: card.id, title: card.title, branch, state: "redesign", verdict: "NEEDS_REDESIGN" }, "tasks");
    continue;
  }
  if (!gateGreen) {
    allFindings.push({
      where: `task ${card.id}`,
      what: "the change never built and passed the tests-enabled suite within the fix budget",
      evidence: `${gateRounds} fix round(s) at ${wt}/build/test`,
      status: "verified",
      severity: "high",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "suite red" }, "tasks");
    continue;
  }
  verified.push(`task ${card.id}: tests-enabled build+ctest green after ${gateRounds} fix round(s)`);

  phase("Independent review of the diff");
  report({ id: card.id, title: card.title, branch, state: "review", verdict: "" }, "tasks");
  const reviewer = agent(`Reviewer ${card.id}`, { system: reviewerPersona, budget: { webSearch: 0 } });
  const review = await reviewer.ask<Review>(
    `Review the committed change on branch ${branch} in the worktree ${wt} — the diff is ` +
      `\`git -C ${wt} diff ${effBase}..HEAD\` (plus anything untracked). Task card:\n${JSON.stringify(card, null, 2)}\n\n` +
      `Author's notes:\n${impl.notesForReviewer}\n\nCommits:\n${commitList.stdout.trim()}\n\n` +
      `Report every real finding with quoted evidence; approving takes evidence, objecting is the useful move.`,
  );
  log(`Task ${card.id}: review returned ${review.findings.length} finding(s)`);

  phase("Confirm each finding independently");
  const capped = review.findings.slice(0, 12);
  if (review.findings.length > 12) {
    notCovered.push(`task ${card.id}: ${review.findings.length - 12} review finding(s) beyond the 12-finding cap were recorded unconfirmed`);
  }
  const confirmations = await Promise.all(
    capped.map((f, i) =>
      // Low-severity findings (doc/style level) confirm on the cheap model; the evidence
      // still decides. Repo-only stage: zero web budget, no search wandering.
      agent(
        `Confirmer ${card.id} ${i + 1}`,
        f.severity === "low"
          ? { system: confirmerPersona, model: "GLM-5.3-Flash", budget: { webSearch: 0 } }
          : { system: confirmerPersona, budget: { webSearch: 0 } },
      ).ask<Confirmation>(
        `Decide whether this finding is real — from its cited evidence, by reading those exact lines.\n\n` +
          `Change under review: "${card.title}" on branch ${branch} (diff base ${effBase}). Changed files:\n` +
          `${impl.changedFiles.length > 0 ? impl.changedFiles.join("\n") : "(see git log)"}\n\n` +
          `Finding:\n${JSON.stringify(f, null, 2)}\n\n` +
          `Bounded procedure: open the finding's cited path:line first; one targeted follow-up at most ` +
          `(e.g. \`git -C ${wt} diff ${effBase}..HEAD -- <the cited path>\` — it also decides preexisting: ` +
        `cited lines untouched by the diff mean the defect exists at the base); do not re-derive the change. ` +
          `If that budget cannot decide it, return holds=false with reason "inconclusive from cited evidence" ` +
          `and the one read that would decide it. Report exactly what you read.`,
      ),
    ),
  );
  const confirmed: Finding[] = [];
  const refuted: Finding[] = [];
  capped.forEach((f, i) => {
    const c = confirmations[i];
    if (c !== undefined && c.holds) confirmed.push({ ...f, preexisting: c.preexisting });
    else refuted.push(f);
    report(
      {
        ...f,
        ...(c !== undefined && c.preexisting ? { preexisting: true } : {}),
        status: c !== undefined && c.holds ? "verified" : "unconfirmed",
        task: card.id,
        note: c !== undefined ? c.reason : "no confirmation returned",
      },
    );
  });

  const toFix = confirmed.filter((f) => f.preexisting !== true);
  const preexistingFindings = confirmed.filter((f) => f.preexisting === true);
  if (preexistingFindings.length > 0) {
    notCovered.push(
      `task ${card.id}: ${preexistingFindings.length} confirmed pre-existing defect(s) (predate the branch) — ` +
        `recorded to .zcode/TODO.md by the memory writer, not fixed here per minimal-diff`,
    );
  }
  phase("Apply the confirmed fixes");
  let postFixVerdict = review.verdict;
  if (toFix.length > 0) {
    const fix = await fixer.ask<FixReport>(
      `These findings were each independently confirmed against ${wt} on code this change ` +
        `introduced. Fix every one with minimal changes; commit each fix as one logical unit:\n${JSON.stringify(toFix, null, 2)}`,
    );
    let fixGreen = false;
    for (let round = 1; round <= 2 && !fixGreen; round++) {
      const build = await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], { timeoutMs: 1_800_000, output: { tail: 32_768 } });
      if (build.exitCode !== 0) {
        await fixer.ask<FixReport>(`After the review fixes the build failed (round ${round}) at ${wt}. Tail:\n${(build.stderr + build.stdout).slice(-4000)}`);
        continue;
      }
      const tests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } });
      if (tests.exitCode !== 0) {
        const fresh = failedTests(tests.stderr + tests.stdout).filter((n) => !knownRed.includes(n));
        if (fresh.length > 0) {
          await fixer.ask<FixReport>(`After the review fixes the suite failed (round ${round}) at ${wt}; failing: ${fresh.join(", ")}. Tail:\n${(tests.stderr + tests.stdout).slice(-4000)}`);
          continue;
        }
        // only tolerated failures remain — green modulo the known-red baseline set
      }
      fixGreen = true;
    }
    if (!fixGreen) {
      allFindings.push({
        where: `task ${card.id}`,
        what: "confirmed findings were applied but the tree no longer builds/tests green within the budget",
        evidence: `fix commits then failed re-gate at ${wt}/build/test`,
        status: "verified",
        severity: "high",
      });
      report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "fix regression" }, "tasks");
      continue;
    }
    const deltaReview = await reviewer.ask<Review>(
      `Review ONLY the fix commits in ${wt}: ${fix.commits.join(", ")} (git -C ${wt} diff shows them last). ` +
        `These findings were confirmed and just fixed — do NOT re-raise them, only check they hold:\n` +
        `${JSON.stringify(toFix.map((f) => ({ where: f.where, what: f.what })))}\n` +
        `Do the confirmed findings now hold as fixed, and did any fix break or bypass anything?`,
    );
    postFixVerdict = deltaReview.verdict;
    for (const f of deltaReview.findings) {
      allFindings.push({ ...f, status: "unconfirmed", disposition: "deferred-to-user" });
    }
    verified.push(`task ${card.id}: ${toFix.length} confirmed finding(s) fixed, re-gated green, fix delta re-reviewed`);
  }
  for (const f of toFix) allFindings.push({ ...f, status: "verified", disposition: "fixed" });
  for (const f of preexistingFindings) allFindings.push({ ...f, status: "verified", disposition: "pre-existing" });
  for (const f of refuted) allFindings.push({ ...f, status: "unconfirmed", disposition: "refuted" });

  phase("Verify on the real seat");
  report({ id: card.id, title: card.title, branch, state: "verify", verdict: "" }, "tasks");
  const seatEvidence: string[] = [];
  let seatFailed = false;
  let requiredSeatMissing = false;
  const needSmokes = required.required.smokes || runSmokes;
  const needBench = required.required.bench_ab || runBench;
  if (needSmokes && smokesEnabled) {
    for (const smoke of [
      { name: "drag-smoke", file: "./scripts/drag-smoke.sh", timeoutMs: 420_000 },
      { name: "app-smoke", file: "./scripts/app-smoke.sh", timeoutMs: 900_000 },
    ]) {
      let exit = -1;
      let timedOut = false;
      let full = "";
      for (let attempt = 1; attempt <= 2; attempt++) {
        try {
          const run = await world.run(
            "bash",
            ["-c", `cd '${wt}' && ${envPrefix}LUMEN_BUILD_DIR=build/test exec ${smoke.file} --join-seat-session`],
            { timeoutMs: smoke.timeoutMs, output: { tail: 65_536 } },
          );
          exit = run.exitCode;
          full = run.stderr + run.stdout;
        } catch {
          // A timeout (or spawn failure) is a rejection, not an exit code: it must not
          // kill the run — it is COULD NOT RUN evidence.
          timedOut = true;
          exit = -1;
          full = "";
        }
        if (exit === 0 || timedOut) break;
        log(`Task ${card.id}: ${smoke.name} attempt ${attempt} exit ${exit}`);
      }
      const tail = full.slice(-3000);
      // Tolerance is by exact failing-row NAME against the owner-blessed set —
      // never by pass-count floor: a change that breaks exactly one new row
      // must not clear a row-count tolerance.
      const failRows = smokeFailRows(full);
      const freshFails = failRows.filter((r) => !toleratedFailset.includes(r));
      const improvedRows = toleratedFailset.filter((r) => !failRows.includes(r));
      const tolerated = exit !== 0 && !timedOut && freshFails.length === 0 && failRows.length > 0;
      const brokered = full.includes("seat-lock: acquired");
      seatEvidence.push(
        `${smoke.name} --join-seat-session → ` +
          (timedOut
            ? "COULD NOT RUN (timed out)"
            : tolerated
              ? `PASS at host tolerance (failing rows [${failRows.join(", ")}] ⊆ the tolerated set; exit ${exit})${brokered ? "" : " — NO BROKER TICKET"}` +
                (improvedRows.length > 0 ? `; improved: [${improvedRows.join(", ")}] now PASS` : "")
              : `exit ${exit}${brokered ? "" : " — NO BROKER TICKET"}`),
      );
      verified.push(
        `task ${card.id}: ${smoke.name} ${exit === 0 || tolerated ? "PASS" : timedOut ? "COULD NOT RUN (timed out)" : `FAIL (exit ${exit}; failing rows: ${failRows.join(", ") || "see log"})`} on the real seat`,
      );
      if (timedOut) {
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} timed out after ${smoke.timeoutMs}ms — COULD NOT RUN, it contributes no seat evidence`,
          evidence: "no output captured (timeout)",
          status: "verified",
          severity: "medium",
        });
      } else if (tolerated) {
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} exited ${exit} with every failing row ([${failRows.join(", ")}]) inside the tolerated failing set — the known host rows, not a regression`,
          evidence: tail,
          status: "verified",
          severity: "low",
        });
      } else if (exit === 3 && full.includes("seat-lock:")) {
        // Broker-origin 3: a STALE ticket refused the job — an owner decision,
        // never the change's failure.
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} could not start: the seat broker reported a stale ticket (owner decision required, force-release) — COULD NOT RUN, not a change failure`,
          evidence: tail,
          status: "verified",
          severity: "medium",
        });
      } else if (exit === 3) {
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} reported INVALID (contaminated measurement) twice — seat not quiet or foreign input`,
          evidence: tail,
          status: "verified",
          severity: "medium",
        });
      } else if ((exit === 2 || exit === 4) && full.includes("seat-lock:")) {
        // Broker-origin 2 (preflight contamination) / 4 (queue wait timeout):
        // the job never ran — environmental, never the change's.
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} could not run through the seat broker (exit ${exit}: ${exit === 2 ? "preflight found the seat contaminated" : "queue wait timed out"}) — COULD NOT RUN, not a change failure`,
          evidence: tail,
          status: "verified",
          severity: "medium",
        });
      } else if (exit !== 0) {
        seatFailed = true;
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} failed twice on the real seat (failing rows beyond the tolerated set: ${freshFails.join(", ") || "see log"}) — per the no-degradation rule this goes to the user with the evidence`,
          evidence: tail,
          status: "verified",
          severity: "high",
        });
      }
    }
  } else if (needSmokes && !smokesEnabled) {
    requiredSeatMissing = required.required.smokes;
    notCovered.push(
      `task ${card.id}: seat smokes did not run (${runSmokes ? "owner chose smoke-less delivery at the busy-seat decision" : "run_smokes was not set"})${required.required.smokes ? " — they were REQUIRED by the diff (seat-surface change): the gate holds this against the task" : ""}`,
    );
  } else {
    notCovered.push(`task ${card.id}: seat smokes not required by the diff (classes [${required.classes.join(", ")}]) and not requested via run_smokes`);
  }
  if (needBench) {
    const bench = await world.run(
      "bash",
      ["-c", `cd '${wt}' && LUMEN_BUILD_DIR=build/test exec ./scripts/ui-bench.sh`],
      { timeoutMs: 1_800_000, output: { tail: 32_768 } },
    );
    seatEvidence.push(`ui-bench (engine, stored baseline) → exit ${bench.exitCode}${required.required.bench_ab ? " (required by the diff: render-path)" : " (operator-requested)"}`);
    verified.push(`task ${card.id}: ui-bench ${bench.exitCode === 0 ? "gate pass" : "gate FAIL (p50 +25% or worse) — goes to the user, not auto-accepted"}`);
    if (bench.exitCode !== 0) {
      seatFailed = true;
      allFindings.push({
        where: `task ${card.id}`,
        what: "ui-bench gate FAILED against the stored baseline — measured regression evidence for the user",
        evidence: (bench.stderr + bench.stdout).slice(-2000),
        status: "verified",
        severity: "high",
      });
    }
  }

  const gateCountRun = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "-N"], { output: { tail: 2048 } });
  const gateCount = ctestTotal(gateCountRun.stdout + gateCountRun.stderr);
  const baseCount = baselineCountOf.get(card.id) ?? -1;
  const countLine =
    baseCount >= 0 && gateCount >= 0
      ? gateCount >= baseCount
        ? `computed ${gateCount} tests via ctest -N (baseline ${baseCount})`
        : `INVALID as a suite run: ${gateCount} registered tests vs the baseline's ${baseCount} — tests were dropped`
      : "suite count not computable (ctest -N unreadable on one side)";
  if (countLine.startsWith("INVALID")) seatFailed = true;

  phase("Independent analysis of the evidence");
  report({ id: card.id, title: card.title, branch, state: "verify", verdict: "analysis" }, "tasks");
  const analysis = await agent(`Analyst ${card.id}`, { system: analystPersona, budget: { webSearch: 0 } }).ask<Analysis>(
    `Analyze what this task's evidence does and does not establish. You did not run any of ` +
      `it — judge provenance (counts computed, broker lines present), coverage against the ` +
      `diff's required evidence, and whether any failure is environmental rather than the ` +
      `change's. Worktree: ${wt} (diff base ${effBase}).

` +
      `Required evidence (from the diff): ${JSON.stringify(required)}
` +
      `Suite count line: ${countLine}
` +
      `Seat evidence: ${JSON.stringify(seatEvidence)}
` +
      `Verified lines for this task: ${verified.filter((v) => v.startsWith(`task ${card.id}`)).join(" | ")}`,
  );

  phase("Gate the finished task on its evidence");
  report({ id: card.id, title: card.title, branch, state: "gate", verdict: "" }, "tasks");
  const packet = {
    card,
    branch,
    base: effBase,
    worktree: wt,
    implementation: { summary: impl.summary, commits: impl.commits, testsAdded: impl.testsAdded, deviations: impl.deviations },
    review: {
      verdict: postFixVerdict,
      noise: { raised: review.findings.length, confirmed: confirmed.length, refuted: refuted.length, preexisting: preexistingFindings.length },
      confirmedFindings: toFix,
      preexistingFindings,
    },
    required,
    tolerance: {
      known_red: knownRed,
      app_smoke_tolerated_failset: toleratedFailset,
      note: "owner sign-off items — every non-default value here made red look greener; READY under one carries it explicitly",
    },
    gate_runs: {
      baseline: "tests-enabled configure+build+ctest green before the change",
      suite: `green after ${gateRounds} fix round(s); ${countLine} (tolerated known-red: ${knownRed.length > 0 ? knownRed.join(", ") : "none"})`,
      seat: seatEvidence.length > 0 ? seatEvidence : "not run (see notCovered)",
      bench: needBench ? "run (see findings)" : "not required by the diff and not requested",
    },
    analysis: { verdict: analysis.verdict, gaps: analysis.gaps },
    seat_failed: seatFailed,
    required_seat_missing: requiredSeatMissing,
  };
  const gate = await agent(`Gatekeeper ${card.id}`, { system: gatekeeperPersona, budget: { webSearch: 0 } }).ask<GateVerdict>(
    `Gate this finished Lumen task. Verify the packet against the worktree at ${wt} yourself ` +
      `(read the diff, the tests, the suite definition) before deciding.\n\n${JSON.stringify(packet, null, 2)}`,
  );
  for (const r of gate.reasons) {
    allFindings.push({
      where: `task ${card.id} gate`,
      what: r,
      evidence: `gate verdict ${gate.verdict} on ${wt} (${branch})`,
      status: "verified",
      severity: gate.verdict === "READY" ? "low" : "high",
    });
  }
  if (gate.verdict === "READY") readyBranches.push(branch);
  branchOf.set(card.id, branch);
  verdictOf.set(card.id, gate.verdict);
  report(
    {
      id: card.id,
      title: card.title,
      branch,
      state: gate.verdict === "READY" ? "done" : gate.verdict === "NEEDS_REDESIGN" ? "redesign" : "blocked",
      verdict: gate.verdict,
    },
    "tasks",
  );
  log(`Task ${card.id}: gate ${gate.verdict}`);

  phase("Write the memory record and the PR text");
  const memory = await agent(`Memory writer ${card.id}`, { system: memoryPersona, model: "GLM-5.3-Flash", budget: { webSearch: 0 } }).ask<MemoryRecord>(
    `Write the memory record for this finished Lumen task (main checkout ${MAIN_WS}; the worktree at ${wt} ` +
      `holds the evidence — cite its commits). Record what was built, the decisions and their reasons, the ` +
      `measured/tested evidence, any fuckups and their fixes, and host findings. Include the review noise ` +
      `line from the packet (raised/confirmed/refuted) so the review stage's false-positive rate is ` +
      `trackable across runs, and add the .zcode/TODO.md rows for any preexistingFindings. ` +
      `Packet:\n${JSON.stringify(packet, null, 2)}\n` +
      `Gate verdict: ${gate.verdict} (${gate.reasons.join("; ")})`,
  );
  verified.push(`task ${card.id}: memory record at ${memory.file}`);
  prSections.push(
    [
      `## ${card.title} (\`${branch}\`, worktree \`${wt}\`)`,
      "",
      `**Gate: ${gate.verdict}**${gate.reasons.length > 0 ? ` — ${gate.reasons.join("; ")}` : ""}`,
      "",
      "### What and why",
      "",
      impl.summary,
      "",
      `Acceptance: ${card.acceptance.join(" / ")}`,
      "",
      "### Commits",
      "",
      ...(impl.commits.length > 0 ? impl.commits.map((c) => `- ${c}`) : ["- (none recorded — see worktree log)"]),
      "",
      "### Tests",
      "",
      ...(impl.testsAdded.length > 0 ? impl.testsAdded.map((c) => `- ${c}`) : ["- (none — see gate reasons)"]),
      "",
      "### Review",
      "",
      postFixVerdict,
      "",
      `Findings: ${toFix.length} confirmed on this change and fixed, ${preexistingFindings.length} confirmed pre-existing (recorded to .zcode/TODO.md), ${refuted.length} refuted by independent confirmation.`,
      "",
      "### Evidence",
      "",
      ...verified.filter((v) => v.startsWith(`task ${card.id}`)).map((v) => `- ${v}`),
      "",
    ].join("\n"),
  );
  await artifact.markdown(
    "pr-texts",
    [`# PR descriptions — ${plan.objective}`, "", ...prSections].join("\n"),
    { title: "PR descriptions per task", description: "Assembled from each task's implementation, review and gate evidence. READY tasks are merged and pushed by the run; this artifact is the PR of record on this gh-less host.", primary: true },
  );
}

// Every finding leaves the run with a disposition — review-path ones are stamped where they
// resolve; infra/status findings default to deferred-to-user (the owner decides on them).
for (const f of allFindings) if (f.disposition === undefined) f.disposition = "deferred-to-user";

// Positive work ends merged into main and pushed (owner standing rule, 2026-11-19):
// every gate-READY branch merges in topological order, the MERGED tree re-runs the
// tests-enabled suite once (each task was gated alone; the combination never was),
// and only a green merged main is pushed. Anything below READY — BLOCKED (incl.
// BLOCKED-ON-HOST), NOT_READY, NEEDS_REDESIGN — never merges here; its branch and
// evidence go to the owner. No gh CLI / HTTPS credentials on this host: the PR-text
// artifact plus the merge commit is the PR of record, pushed over SSH.
// The headline states what actually landed, not what was attempted: landingLanded
// is set only when the merges survived the merged-tree re-gate, landingPushed only
// when origin took them.
let landingLanded = false;
let landingPushed = false;
if (readyBranches.length > 0) {
  phase("Merge READY branches into main, re-gate the merged tree, push");
  const onMain = await world.run("git", ["-C", MAIN_WS, "branch", "--show-current"], { output: { tail: 512 } });
  const mainClean = await world.run("git", ["-C", MAIN_WS, "status", "--porcelain"], { output: { tail: 2048 } });
  if (onMain.exitCode !== 0 || onMain.stdout.trim() !== "main" || mainClean.stdout.trim() !== "") {
    allFindings.push({
      where: "merge+push",
      what: `READY branches (${readyBranches.join(", ")}) were NOT merged: the main checkout at ${MAIN_WS} is not a clean main (on "${onMain.stdout.trim()}", ${mainClean.stdout.trim() === "" ? "clean" : "dirty"}) — merge by hand or re-run`,
      evidence: `git -C ${MAIN_WS} branch --show-current + status --porcelain at merge time`,
      status: "verified",
      severity: "medium",
    });
    notCovered.push("merge+push of READY branches — the main checkout was not on a clean main");
  } else {
    const preMerge = (await world.run("git", ["-C", MAIN_WS, "rev-parse", "HEAD"], { output: { tail: 512 } })).stdout.trim().split("\n").pop() ?? "";
    let mergeOk = true;
    // Merge notices are held back from `verified` until the landing survives the
    // re-gate — a rolled-back merge must not live on as a "merged" evidence row.
    const mergedNow: string[] = [];
    for (const card of tasks) {
      if (verdictOf.get(card.id) !== "READY") continue;
      const branch = branchOf.get(card.id);
      if (branch === undefined) continue;
      const merge = await world.run("git", ["-C", MAIN_WS, "merge", "--no-edit", branch], { output: { tail: 4096 } });
      if (merge.exitCode !== 0) {
        // Stop at the first conflict: every later merge would sit on an untested base.
        await world.run("git", ["-C", MAIN_WS, "merge", "--abort"], { output: { tail: 1024 } });
        await world.run("git", ["-C", MAIN_WS, "reset", "--hard", preMerge], { output: { tail: 1024 } });
        mergeOk = false;
        allFindings.push({
          where: `merge ${card.id}`,
          what: `branch ${branch} conflicts when merged into main — the READY tasks are not integrable together; main was reset to ${preMerge}, branches kept; the owner decides the integration`,
          evidence: `git -C ${MAIN_WS} merge --no-edit ${branch} → exit ${merge.exitCode} (aborted, main reset)`,
          status: "verified",
          severity: "high",
        });
        break;
      }
      mergedNow.push(`merged ${branch} into main (${card.title})`);
    }
    if (mergeOk) {
      const cfg = await world.run(
        "cmake",
        ["--preset", "release", "-S", MAIN_WS, "-B", `${MAIN_WS}/build/merge-gate`, "-DLUMEN_BUILD_TESTS=ON"],
        { timeoutMs: 600_000, output: { tail: 4096 } },
      );
      const build = cfg.exitCode === 0
        ? await world.run("cmake", ["--build", `${MAIN_WS}/build/merge-gate`, "-j2"], { timeoutMs: 1_800_000, output: { tail: 32_768 } })
        : undefined;
      const tests = build !== undefined && build.exitCode === 0
        ? await world.run("ctest", ["--test-dir", `${MAIN_WS}/build/merge-gate`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } })
        : undefined;
      const fresh = tests === undefined ? ["(no test run)"] : failedTests(tests.stderr + tests.stdout).filter((n) => !knownRed.includes(n));
      if (tests !== undefined && fresh.length === 0) {
        const push = await world.run(
          "git",
          ["-C", MAIN_WS, "push", "origin", "main", ...readyBranches],
          { timeoutMs: 300_000, output: { tail: 4096 } },
        );
        verified.push(...mergedNow);
        if (push.exitCode === 0) {
          landingLanded = true;
          landingPushed = true;
          verified.push(`pushed main + ${readyBranches.length} READY branch(es) to origin (owner standing rule 2026-11-19)`);
        } else {
          landingLanded = true;
          allFindings.push({
            where: "push",
            what: `merged main is green but the push failed — the merges are local and complete; nothing is lost (main at the merge, branches kept); the owner pushes (SSH remote required)`,
            evidence: (push.stderr + push.stdout).trim().split("\n").slice(-3).join(" | "),
            status: "verified",
            severity: "medium",
          });
        }
      } else {
        // Never leave main red: undo the merges, keep the READY branches, report.
        await world.run("git", ["-C", MAIN_WS, "reset", "--hard", preMerge], { output: { tail: 1024 } });
        allFindings.push({
          where: "merge re-gate",
          what: `the merged main failed its tests-enabled re-gate (${fresh.join(", ")}) — main was reset to ${preMerge}; each branch was green alone, the combination is not; the owner decides the integration`,
          evidence: tests === undefined ? "configure/build of the merged tree failed" : (tests.stderr + tests.stdout).slice(-1500),
          status: "verified",
          severity: "high",
        });
        verified.push(`merge attempt rolled back: main reset to ${preMerge} after the merged-tree re-gate failed — nothing landed, branches and worktrees kept`);
        notCovered.push("push — the merged tree is not green");
      }
    }
  }
}

// The headline states what actually landed, not what was attempted:
// landingLanded is set only when the merges survived the merged-tree re-gate,
// landingPushed only when origin took them.
const landingNote = readyBranches.length === 0
  ? "Nothing is merged or pushed."
  : landingPushed
    ? "All READY branches were merged into main (merged tree re-gated) and pushed; PR text per task is in the artifact (the PR of record on this gh-less host)."
    : landingLanded
      ? "READY branches were merged into main locally, but the PUSH failed — the owner pushes over SSH; nothing is lost."
      : "Landing held: the merge phase did not complete (see findings/notCovered) — branches and worktrees are kept for the owner.";
const conclusion = readyBranches.length === tasks.length && tasks.length > 0
  ? `All ${tasks.length} task(s) gate-READY (${readyBranches.join(", ")}). ${landingNote}`
  : readyBranches.length > 0
    ? `${readyBranches.length} of ${tasks.length} task(s) gate-READY (${readyBranches.join(", ")}); the rest are blocked or need redesign with recorded evidence — see findings. ${landingNote}`
    : `No task reached READY: ${tasks.length === 0 ? "the plan produced no tasks" : "every task blocked, conflicted or needing redesign with recorded evidence"}. Nothing is merged or pushed.`;
const done: WorkflowReport = {
  conclusion,
  findings: allFindings,
  verified,
  notCovered,
};
return finishRun(done);
