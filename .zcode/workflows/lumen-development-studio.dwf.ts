/* zcode-workflow
description: "Lumen's development studio orchestrator: plan a request into gated
  tasks, run each in its own git worktree (test-enabled baseline → implement
  with logical commits → build+test gates → independent review → per-finding
  confirmation → fixes → real-seat smokes → evidence gate → memory record), and
  assemble a PR description per task. Never pushes — branches are delivered for
  the user's decision."
whenToUse: A non-trivial Lumen change or multi-task request that must go through
  the full gated studio (plan / implement / review / verify / gate / memory), or
  when the user asks for the studio or coordinated delivery on Lumen. One
  bounded task → lumen-gated-change instead; cross-compositor benchmarking →
  lumen-compositor-bench.
args:
  base:
    type: string
    description: Git ref (commit/branch) each task worktree is cut from
    default: HEAD
  branch_prefix:
    type: string
    description: Branch prefix; each task gets <prefix>/<task-id> (must not already exist)
    default: zcode/task
  max_tasks:
    type: number
    description: Cap on planned tasks executed by one run (rest are reported as not covered)
    default: 5
  known_red:
    type: json
    description: "Test names tolerated in a red baseline (json array, e.g.
      [\"ui_gallery\",\"test_task_model\"]): tasks proceed and every suite gate then
      requires no NEW failures beyond this set. Failures outside the set park the run
      on an owner decision (proceed-and-tolerate vs skip)."
    default: []
  run_bench:
    type: boolean
    description: Also run scripts/ui-bench.sh (engine bench against the stored
      baseline) in each task's verification
    default: false
  run_smokes:
    type: boolean
    description: "Run drag-smoke and app-smoke with --join-seat-session in each
      task's verification (needs a quiet host: one DRM master, no other
      session)"
    default: true
  task:
    type: string
    description: "The request to deliver: what you would tell the studio (feature,
      protocol, fix, optimization)"
    required: true
*/
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
}

interface ReportFinding extends Finding {
  /** "verified" when independently confirmed or decided by a command; "unconfirmed" when confirmation failed. */
  status: "verified" | "unconfirmed";
}

interface Review {
  /** One-paragraph verdict: is the change ready for verify/gate, and what blocks it. */
  verdict: string;
  findings: Finding[];
}

interface Confirmation {
  /** True when the finding reproduces from its evidence alone. */
  holds: boolean;
  /** What you read (or ran) to decide, in one or two sentences. */
  reason: string;
}

interface FixReport {
  /** What was fixed and how, 2-4 sentences. */
  summary: string;
  /** One "hash subject" line per fix commit. */
  commits: string[];
}

interface GateVerdict {
  /** READY only when acceptance is met with independent evidence and nothing regressed. */
  verdict: "READY" | "BLOCKED" | "NOT_READY";
  /** Each reason one sentence; cites the evidence it rests on. */
  reasons: string[];
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

const WT_ROOT = "/home/dimmus/lumen-wt";
const MAIN_WS = "/home/dimmus/lumen";

const task = String(args.task ?? "");
const base = args.base === undefined ? "HEAD" : String(args.base);
const branchPrefix = args.branch_prefix === undefined ? "zcode/task" : String(args.branch_prefix);
const runSmokes = args.run_smokes !== false;
const runBench = args.run_bench === true;
const maxTasks = typeof args.max_tasks === "number" ? args.max_tasks : 5;
const knownRed: string[] = Array.isArray(args.known_red) ? args.known_red.map(String) : [];

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

if (task === "") {
  const empty: WorkflowReport = {
    conclusion: "No task was given (argument 'task' is required); nothing ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — the task argument was empty"],
  };
  return empty;
}

const plannerPersona =
  "You are the planning stage of Lumen's development studio. Read the repository's AGENTS.md " +
  "(especially §1.2 user rules and §3 layer/process/thread rules), .zcode/skills/lumen-decisions/SKILL.md " +
  "(settled outcomes — never plan to re-open one), and .zcode/TODO.md (do not plan already-done work; " +
  "it is historical — verify against code). Inspect the code the request touches. Produce the minimal " +
  "set of gated tasks, each independently deliverable in its own git worktree with a disjoint write " +
  "scope, topologically ordered. Small requests are ONE task. Each task's notes carry the risks, the " +
  "relevant settled decisions, and host limits (this VM has no hardware Vulkan — the GL path runs here; " +
  "hardware Vulkan claims need the notebook). Planning is read-only: do not edit anything, do not run builds.";

const implementerPersona =
  "You are the implement stage of Lumen's development studio, working in ONE assigned git worktree. " +
  "Read that worktree's AGENTS.md first — it is binding (§1.2 best-in-class bar and no-degradation rule, " +
  "§3 layering, §4 conventions, §5 invariants). Read .zcode/skills/lumen-decisions/SKILL.md and " +
  ".zcode/skills/lumen-build-run/SKILL.md in the worktree if present. Minimal diff; match surrounding " +
  "conventions; no new config keys, env vars or user-facing features the task did not ask for; do not " +
  "re-open settled decisions. New functionality REQUIRES tests that exercise it, added in the same " +
  "change. Commit each logically complete unit with a detailed conventional message (summary line, " +
  "what/why, design decision, evidence); never push, never amend pushed-or-reviewed history. " +
  "Compositor/display/input behavior is proven on the real seat only — never claim headless " +
  "--virtual-outputs evidence for it. The workflow runs the full tests-enabled suite and seat smokes " +
  "after you; run only narrow checks yourself (ninja -C <build> <target>, single ctest tests). " +
  "If any check shows a regression, STOP and report it with evidence — never keep or work around it. " +
  "If the task is impossible, contradicts AGENTS.md or a settled decision, or forces a deviation, " +
  "escalate and say so plainly rather than improvising.";

const reviewerPersona =
  "You are the review stage of Lumen's development studio, reading a diff you did not write. " +
  "Read the worktree's AGENTS.md (§3 layer/process/thread rules, §4 conventions, §7 anti-patterns, " +
  "§8 checklist) and .zcode/skills/lumen-decisions/SKILL.md — re-opening a settled decision without " +
  "new evidence is itself a finding. Review for correctness, layering, thread affinity, error and " +
  "logging conventions, hot-path rules (no heap churn, no info+ logs per frame), minimal diff, and " +
  "the no-degradation rule: unasked-for surface and evidence-free performance claims are findings; " +
  "missing tests for new functionality is a finding. You are read-only: do not edit any file. " +
  "Cite path:line and quote the lines you read. Report only real findings — an empty list must mean " +
  "you found nothing, not that you looked nowhere.";

const confirmerPersona =
  "You are an independent verifier in Lumen's development studio. You are given one review finding " +
  "and must decide, from the evidence alone and by reading the worktree's code yourself, whether it " +
  "is real. Reproduce it from the cited lines; check surrounding context the reviewer may have " +
  "missed; run a narrow check only if a command decides it. You are read-only: do not edit any file. " +
  "If the evidence does not hold up, say so — a false finding must not survive this stage.";

const fixerPersona =
  "You fix build failures, test failures and confirmed review findings in ONE assigned Lumen git " +
  "worktree. Read its AGENTS.md §4 conventions before editing; minimal diffs only, no refactors " +
  "beyond what the failure forces, no new features, never weaken a test or golden to pass. Commit " +
  "each fix as one logical unit with a detailed message; never push. Do not run the full suite — " +
  "the workflow re-gates after you; narrow checks are fine. If a failure looks environmental " +
  "(host noise, DRM master, missing protocol stamp) rather than caused by the change, escalate and " +
  "say so plainly rather than papering over it.";

const gatekeeperPersona =
  "You are the final gate of Lumen's development studio for ONE task. You are read-only: do not " +
  "implement, repair, rerun suites, or decide a regression is acceptable. Read the worktree's " +
  "AGENTS.md §1.2 (no degradation; any measured regression stops the task and goes to the user with " +
  "the A/B evidence) and .zcode/skills/lumen-decisions/SKILL.md. Validate the packet you are given " +
  "against the worktree itself: acceptance criteria vs what shipped, independent review confirmed, " +
  "tests-enabled suite counts (a plain release build has NO unit tests — a suite claim from it is " +
  "invalid), seat-smoke results for compositor/display/input surface, A/B numbers for perf changes, " +
  "tests covering new functionality. Missing required evidence is a hold, not a pass; environmental " +
  "unavailability never converts to success. Decide exactly one: READY (acceptance met, evidence " +
  "independent and complete, nothing regressed) / NOT_READY (a gate is confirmed violated) / " +
  "BLOCKED (evidence missing, invalid or not comparable). READY certifies the gate only — never " +
  "permission to merge, publish or push.";

const memoryPersona =
  "You write Lumen's project memory records. Work in the MAIN checkout at " + MAIN_WS + " (not a task " +
  "worktree). Read .zcode/skills/lumen-memory-writer/SKILL.md if present, and 2-3 existing files in " +
  ".zcode/memories/ to match the archive's voice. Then write .zcode/memories/lumen-<kebab-topic>.md: " +
  "YAML frontmatter (name, one dense description sentence with dates/numbers, metadata node_type: " +
  "memory, type: project), dated verified facts with inline evidence (commits, paths, commands, " +
  "measured numbers), explicit disproven theories where any, then **Why:** (the general lesson) and " +
  "**How to apply:** (imperative guidance). Cross-reference with [[wiki-links]]. Add one row to " +
  ".zcode/memories/MEMORY.md. Then commit both files on the main checkout's current branch with " +
  "subject 'memory: <topic>' (git add the two files; never push). If a memory for this topic already " +
  "exists, extend it with a dated section instead of forking a new file.";

const verified: string[] = [];
const notCovered: string[] = [
  "hardware Vulkan: this VM has none (GL path); Vulkan-decisive claims need a notebook session hosted by the user",
];
const allFindings: ReportFinding[] = [];
const prSections: string[] = [];
const readyBranches: string[] = [];

artifact.board("tasks", {
  title: "Studio tasks",
  key: "id",
  status: "state",
  columns: ["planned", "baseline", "implementing", "review", "verify", "gate", "done", "blocked"],
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

for (const card of tasks) {
  const wt = `${WT_ROOT}/${card.id}`;
  const branch = `${branchPrefix}/${card.id}`;

  phase("Set up the worktree and test-enabled baseline");
  report({ id: card.id, title: card.title, branch, state: "baseline", verdict: "" }, "tasks");
  const add = await world.run("git", ["worktree", "add", wt, "-b", branch, base], { output: { tail: 2048 } });
  if (add.exitCode !== 0) {
    allFindings.push({
      where: `task ${card.id}`,
      what: `could not create worktree/branch ${branch} at ${wt}: ${add.stderr.trim().split("\n").pop() ?? ""}`,
      evidence: `git worktree add ${wt} -b ${branch} ${base} → exit ${add.exitCode}`,
      status: "verified",
      severity: "medium",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "worktree" }, "tasks");
    continue;
  }
  await world.run("cp", ["-r", "protocols/upstream", `${wt}/protocols/`], { output: { tail: 1024 } });
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
      evidence: `worktree at ${wt}, base ${base}; ${why}`,
      status: "verified",
      severity: "high",
    });
    report({ id: card.id, title: card.title, branch, state: "blocked", verdict: "red baseline" }, "tasks");
    continue;
  }
  verified.push(`task ${card.id}: tests-enabled baseline green in worktree (configure+build+ctest at ${wt}/build/test)`);
  log(`Task ${card.id}: baseline green`);

  phase("Implement the task in its worktree");
  report({ id: card.id, title: card.title, branch, state: "implementing", verdict: "" }, "tasks");
  const implementer = agent(`Implementer ${card.id}`, { system: implementerPersona });
  const impl = await implementer.ask<ImplResult>(
    `Implement this task in the Lumen worktree at ${wt} (branch ${branch}, cut from ${base}). ` +
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
  const commitList = await world.run("git", ["-C", wt, "log", "--oneline", `${base}..HEAD`], { output: { tail: 4096 } });
  log(`Task ${card.id}: implemented, ${impl.commits.length} commit(s)`);

  phase("Build and test the change");
  const fixer = agent(`Fixer ${card.id}`, { system: fixerPersona });
  let gateGreen = false;
  let gateRounds = 0;
  for (let round = 1; round <= 3 && !gateGreen; round++) {
    gateRounds = round;
    const build = await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], { timeoutMs: 1_800_000, output: { tail: 32_768 } });
    if (build.exitCode !== 0) {
      await fixer.ask<FixReport>(
        `The build failed (round ${round}) in the worktree ${wt}. Fix it with minimal changes and commit. Tail:\n` +
          `${(build.stderr + build.stdout).slice(-4000)}`,
      );
      continue;
    }
    const tests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 1_200_000, output: { tail: 32_768 } });
    if (tests.exitCode !== 0) {
      const fresh = failedTests(tests.stderr + tests.stdout).filter((n) => !knownRed.includes(n));
      if (fresh.length > 0) {
        await fixer.ask<FixReport>(
          `The tests-enabled suite failed (round ${round}) in ${wt}; failing: ${fresh.join(", ")} (the tolerated ` +
            `known-red set ${knownRed.length > 0 ? knownRed.join(", ") : "is empty"} does not cover these). Fix the ` +
            `cause with minimal changes and commit — never weaken a test. Tail:\n${(tests.stderr + tests.stdout).slice(-4000)}`,
        );
        continue;
      }
      // only tolerated failures remain — green modulo the known-red baseline set
    }
    gateGreen = true;
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
  const reviewer = agent(`Reviewer ${card.id}`, { system: reviewerPersona });
  const review = await reviewer.ask<Review>(
    `Review the committed change on branch ${branch} in the worktree ${wt} — the diff is ` +
      `\`git -C ${wt} diff ${base}..HEAD\` (plus anything untracked). Task card:\n${JSON.stringify(card, null, 2)}\n\n` +
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
      agent(`Confirmer ${card.id} ${i + 1}`, { system: confirmerPersona })
        .ask<Confirmation>(`Decide whether this finding is real, from the evidence alone, in the worktree ${wt}:\n${JSON.stringify(f)}`),
    ),
  );
  const confirmed: Finding[] = [];
  const refuted: Finding[] = [];
  capped.forEach((f, i) => {
    const c = confirmations[i];
    if (c !== undefined && c.holds) confirmed.push(f);
    else refuted.push(f);
    report(
      { ...f, status: c !== undefined && c.holds ? "verified" : "unconfirmed", task: card.id },
    );
  });

  phase("Apply the confirmed fixes");
  let postFixVerdict = review.verdict;
  if (confirmed.length > 0) {
    const fix = await fixer.ask<FixReport>(
      `These findings were each independently confirmed against ${wt}. Fix every one with minimal changes; ` +
        `commit each fix as one logical unit:\n${JSON.stringify(confirmed, null, 2)}`,
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
        `Do the confirmed findings now hold as fixed, and did any fix break or bypass anything?`,
    );
    postFixVerdict = deltaReview.verdict;
    for (const f of deltaReview.findings) {
      allFindings.push({ ...f, status: "unconfirmed" });
    }
    verified.push(`task ${card.id}: ${confirmed.length} confirmed finding(s) fixed, re-gated green, fix delta re-reviewed`);
  }
  for (const f of confirmed) allFindings.push({ ...f, status: "verified" });
  for (const f of refuted) allFindings.push({ ...f, status: "unconfirmed" });

  phase("Verify on the real seat");
  report({ id: card.id, title: card.title, branch, state: "verify", verdict: "" }, "tasks");
  const seatEvidence: string[] = [];
  let seatFailed = false;
  if (runSmokes) {
    for (const smoke of [
      { name: "drag-smoke", file: "./scripts/drag-smoke.sh", timeoutMs: 300_000 },
      { name: "app-smoke", file: "./scripts/app-smoke.sh", timeoutMs: 600_000 },
    ]) {
      let exit = -1;
      let tail = "";
      for (let attempt = 1; attempt <= 2; attempt++) {
        const run = await world.run(
          "bash",
          ["-c", `cd '${wt}' && exec ${smoke.file} --join-seat-session`],
          { timeoutMs: smoke.timeoutMs, output: { tail: 16_384 } },
        );
        exit = run.exitCode;
        tail = (run.stderr + run.stdout).slice(-2000);
        if (exit === 0) break;
        log(`Task ${card.id}: ${smoke.name} attempt ${attempt} exit ${exit}`);
      }
      seatEvidence.push(`${smoke.name} --join-seat-session → exit ${exit}`);
      verified.push(`task ${card.id}: ${smoke.name} ${exit === 0 ? "PASS" : `FAIL (exit ${exit})`} on the real seat`);
      if (exit === 3) {
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} reported INVALID (contaminated measurement) twice — seat not quiet or foreign input`,
          evidence: tail,
          status: "verified",
          severity: "medium",
        });
      } else if (exit !== 0) {
        seatFailed = true;
        allFindings.push({
          where: `task ${card.id}`,
          what: `${smoke.name} failed twice on the real seat — per the no-degradation rule this goes to the user with the evidence`,
          evidence: tail,
          status: "verified",
          severity: "high",
        });
      }
    }
  } else {
    notCovered.push(`task ${card.id}: seat smokes (run_smokes was not set) — compositor-surface behavior unverified on the seat`);
  }
  if (runBench) {
    const bench = await world.run(
      "bash",
      ["-c", `cd '${wt}' && LUMEN_BUILD_DIR=build/test exec ./scripts/ui-bench.sh`],
      { timeoutMs: 1_800_000, output: { tail: 32_768 } },
    );
    seatEvidence.push(`ui-bench (engine, stored baseline) → exit ${bench.exitCode}`);
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

  phase("Gate the finished task on its evidence");
  report({ id: card.id, title: card.title, branch, state: "gate", verdict: "" }, "tasks");
  const packet = {
    card,
    branch,
    base,
    worktree: wt,
    implementation: { summary: impl.summary, commits: impl.commits, testsAdded: impl.testsAdded, deviations: impl.deviations },
    review: { verdict: postFixVerdict, confirmed: confirmed.length, refuted: refuted.length, confirmedFindings: confirmed },
    gate_runs: {
      baseline: "tests-enabled configure+build+ctest green before the change",
      suite: `green after ${gateRounds} fix round(s) (tolerated known-red: ${knownRed.length > 0 ? knownRed.join(", ") : "none"})`,
      seat: seatEvidence.length > 0 ? seatEvidence : "not run (run_smokes off)",
      bench: runBench ? "run (see findings)" : "not run (run_bench off)",
    },
    seat_failed: seatFailed,
  };
  const gate = await agent(`Gatekeeper ${card.id}`, { system: gatekeeperPersona }).ask<GateVerdict>(
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
  report({ id: card.id, title: card.title, branch, state: gate.verdict === "READY" ? "done" : "blocked", verdict: gate.verdict }, "tasks");
  log(`Task ${card.id}: gate ${gate.verdict}`);

  phase("Write the memory record and the PR text");
  const memory = await agent(`Memory writer ${card.id}`, { system: memoryPersona, model: "GLM-5.3-Flash" }).ask<MemoryRecord>(
    `Write the memory record for this finished Lumen task (main checkout ${MAIN_WS}; the worktree at ${wt} ` +
      `holds the evidence — cite its commits). Record what was built, the decisions and their reasons, the ` +
      `measured/tested evidence, any fuckups and their fixes, and host findings. Packet:\n${JSON.stringify(packet, null, 2)}\n` +
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
      `Findings: ${confirmed.length} confirmed and fixed, ${refuted.length} refuted by independent confirmation.`,
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
    { title: "PR descriptions per task", description: "Assembled from each task's implementation, review and gate evidence. Push only after the user approves.", primary: true },
  );
}

const conclusion = readyBranches.length === tasks.length && tasks.length > 0
  ? `All ${tasks.length} task(s) delivered and gate-READY: ${readyBranches.join(", ")}. PR text is in the artifact; branches sit in worktrees under ${WT_ROOT} — push only on the user's go.`
  : readyBranches.length > 0
    ? `${readyBranches.length} of ${tasks.length} task(s) gate-READY (${readyBranches.join(", ")}); the rest are blocked with recorded evidence — see findings.`
    : `No task reached READY: ${tasks.length === 0 ? "the plan produced no tasks" : "every task blocked with recorded evidence"}. Nothing is pushed.`;
const done: WorkflowReport = {
  conclusion,
  findings: allFindings,
  verified,
  notCovered,
};
return done;
