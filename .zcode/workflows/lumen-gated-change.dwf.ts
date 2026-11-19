/* zcode-workflow
description: "The gated change workflow for Lumen: baseline build+ctest on the
  clean tree, implement in a DEDICATED WORKTREE on a fresh branch (the main
  checkout stays on main, untouched), build+test fix loop, fresh-eyes review
  of the diff, independent confirmation of every finding (pre-existing defects
  confirmed but recorded, not fixed), apply confirmed findings, then the final
  full-suite gate plus the seat/bench evidence the diff itself requires
  (scripts/classify-change.py derives it — operator knobs can add evidence,
  never waive it). Ports the multi-agent pattern the Claude sessions used for
  every significant Lumen change."
whenToUse: "A single non-trivial Lumen change (feature, optimization, refactor)
  that must pass the no-degradation rule: run when the user asks for a gated
  workflow for one Lumen change. For multi-task requests or full studio delivery
  (worktrees, commits, PR text, memory records) use lumen-development-studio
  instead. Not for small fixes or doc-only edits."
args:
  branch:
    type: string
    description: Branch name to create for the change (must not already exist)
    default: zcode/gated-change
  run_bench:
    type: boolean
    description: "Also run scripts/ui-bench.sh even when the change classifier
      does not require a bench. Additive only: a render-path change gets its
      A/B bench whether or not this is set, and cannot be waived by it."
    default: false
  run_smokes:
    type: boolean
    description: "Also run drag-smoke and app-smoke with --join-seat-session
      even when the classifier does not require them. Additive only: required
      seat evidence cannot be waived by this knob."
    default: false
  task:
    type: string
    description: The change to implement (what you would tell the implementer)
    required: true
*/
interface ImplResult {
  /** 2-4 sentences: what was implemented and the shape of the solution. */
  summary: string;
  /** Workspace-relative paths created or modified. */
  changedFiles: string[];
  /** What the reviewer should look at hardest, and why. */
  notesForReviewer: string;
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
  /** "verified" when an independent subagent confirmed it; "unconfirmed" when confirmation failed. */
  status: "verified" | "unconfirmed";
  /** What happened to it; every finding leaves the workflow with exactly one. */
  disposition: "fixed" | "refuted" | "pre-existing" | "deferred-to-user";
}

interface Review {
  /** One-paragraph verdict: is the change ready for the gate, and what blocks it. */
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

/** What scripts/classify-change.py derives from the diff. */
interface RequiredEvidence {
  classes: string[];
  required: { ctest: boolean; smokes: boolean; bench_ab: boolean; goldens: boolean };
  render_path: boolean;
  decisive_backend_blocked_on_host: boolean;
  host: string;
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

const task = String(args.task ?? "");
const branch = args.branch === undefined ? "zcode/gated-change" : String(args.branch);
const runSmokes = args.run_smokes === true;
const runBench = args.run_bench === true;

if (task === "") {
  const empty: WorkflowReport = {
    conclusion: "No task was given (argument 'task' is required); nothing ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — the task argument was empty"],
  };
  return empty;
}

// The main checkout is where this workflow runs; its path is where world.run's
// cwd is. The worktree root is a sibling 'lumen-wt' directory (overridable with
// LUMEN_WT_ROOT) — derived, never hardcoded to one machine's home.
const pwdRun = await world.run("pwd", [], { output: { tail: 512 } });
const MAIN_WS = pwdRun.stdout.trim().split("\n").pop() ?? ".";
const WT_ROOT = (await world.run("bash", ["-c", `echo "\${LUMEN_WT_ROOT:-$(dirname '${MAIN_WS}')/lumen-wt}"`], { output: { tail: 512 } }))
  .stdout.trim().split("\n").pop() ?? "../lumen-wt";
const slug = branch.replace(/[^a-zA-Z0-9-]+/g, "-").replace(/^-+|-+$/g, "") || "gated";
const wt = `${WT_ROOT}/gated-${slug}`;

// The role contracts live in .zcode/agents/*.md — the single source of truth.
// The personas below only orient and point there; the files are binding.
const implementerPersona =
  "You are the implement stage of Lumen's gated change workflow. Your full contract is " +
  ".zcode/agents/lumen-implementer.md — read it first, it is binding. You work in the " +
  "worktree you are given; commit nothing unless told (this workflow hands off uncommitted; " +
  "state so plainly if you did). Do not run the full test suite — the workflow gates the " +
  "build after you; narrow checks are fine. If the task is impossible, contradicts AGENTS.md, " +
  "or would regress something measured, escalate and say so plainly rather than working around it.";

const reviewerPersona =
  "You are the review stage of Lumen's gated change workflow, reading a diff you did not write. " +
  "Your full contract is .zcode/agents/lumen-reviewer.md — read it first, it is binding " +
  "(including the minimal reading list every finding must carry and the mechanical checks: " +
  "layer-graph and diff-hygiene). You are read-only: do not edit any file.";

const confirmerPersona =
  "You are the verify stage of Lumen's gated change workflow: a bounded auditor, not a second " +
  "reviewer. Your full contract is .zcode/agents/lumen-confirmer.md — read it first, it is " +
  "binding. At most ONE targeted follow-up beyond the cited lines; an honest inconclusive " +
  "beats a grind. You are read-only: do not edit any file.";

const fixerPersona =
  "You fix build failures, test failures and confirmed review findings in the Lumen worktree " +
  "you are given. Your full contract is .zcode/agents/lumen-fixer.md — read it first, it is " +
  "binding. Minimal diffs only; never weaken a test to pass; say 'design-level' when a failure " +
  "is not patchable so the task routes back to planning instead of burning another round.";

const verified: string[] = [];

/** "Total Tests: N" from ctest -N — the tree's own computed count, never quoted from memory. */
function ctestTotal(out: string): number {
  const m = /Total Tests: (\d+)/.exec(out);
  return m !== null && m[1] !== undefined ? Number(m[1]) : -1;
}

phase("Baseline: build and test the tree as it stands (main checkout, untouched)");
const stamp = await world.run("test", ["-f", "protocols/upstream/.fetch-stamp"]);
if (stamp.exitCode !== 0) {
  log("Protocols not fetched yet; fetching (needs network)");
  const fetch = await world.run("./scripts/fetch-protocols.sh", [], { timeoutMs: 300_000, output: { tail: 8192 } });
  if (fetch.exitCode !== 0) {
    const blocked: WorkflowReport = {
      conclusion: "Could not fetch the upstream protocols, so no baseline build was possible.",
      findings: [],
      verified: [],
      notCovered: ["everything — protocol fetch failed; run ./scripts/fetch-protocols.sh with network access first"],
    };
    return blocked;
  }
}
const configure = await world.run(
  "cmake",
  ["--preset", "release", "-B", "build/test", "-DLUMEN_BUILD_TESTS=ON"],
  { timeoutMs: 600_000, output: { tail: 8192 } },
);
if (configure.exitCode !== 0) {
  const blocked: WorkflowReport = {
    conclusion: "CMake configure of the baseline failed; nothing else ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — cmake configure failed on the clean tree"],
  };
  return blocked;
}
const baselineBuild = await world.run("cmake", ["--build", "build/test", "-j2"], {
  timeoutMs: 1_200_000,
  output: { tail: 32_768 },
});
const baselineTests =
  baselineBuild.exitCode === 0
    ? await world.run("ctest", ["--test-dir", "build/test", "--output-on-failure", "-LE", "bench"], { timeoutMs: 900_000, output: { tail: 32_768 } })
    : undefined;
if (baselineBuild.exitCode !== 0 || baselineTests === undefined || baselineTests.exitCode !== 0) {
  const red: WorkflowReport = {
    conclusion:
      "The baseline itself is red — the tree without any change does not build or pass its tests. " +
      "Per the no-degradation rule there is no clean baseline to measure against; fix main first.",
    findings: [],
    verified: ["the baseline was attempted before any change: " + (baselineBuild.exitCode !== 0 ? "the build failed" : "tests failed")],
    notCovered: ["the requested change — never started because the baseline is red"],
  };
  return red;
}
const baselineCountRun = await world.run("ctest", ["--test-dir", "build/test", "-N"], { output: { tail: 2048 } });
const baselineCount = ctestTotal(baselineCountRun.stdout + baselineCountRun.stderr);
verified.push(
  "baseline green before any change: cmake --build build/test (tests-enabled) + ctest --test-dir build/test -LE bench " +
    `(computed count: ${baselineCount >= 0 ? String(baselineCount) + " tests via ctest -N" : "ctest -N unreadable — counts below are unverified"})`,
);
log("Baseline is green; creating the worktree (the main checkout stays on its branch)");

phase("Create the dedicated worktree — never a branch switch in the main checkout");
const status = await git.status();
if (!status.clean) {
  const dirty: WorkflowReport = {
    conclusion: "The main working tree is not clean; the gated change refuses to start on top of unknown edits.",
    findings: [],
    verified: [],
    notCovered: ["the requested change — commit or stash the existing edits first"],
  };
  return dirty;
}
// The branch point as a CONCRETE SHA: the classifier diffs the worktree's
// working tree against it (the new branch has no commits of its own yet —
// 'branch~1' would be main's last commit, the wrong diff entirely).
const branchPoint = (await world.run("git", ["rev-parse", "HEAD"], { output: { tail: 512 } }))
  .stdout.trim().split("\n").pop() ?? "HEAD";
const add = await world.run("git", ["worktree", "add", wt, "-b", branch], { output: { tail: 4096 } });
if (add.exitCode !== 0) {
  const taken: WorkflowReport = {
    conclusion:
      `Could not create worktree ${wt} / branch ${branch} (one of them probably exists from an earlier run). ` +
      `Inspect it, remove it (git worktree remove ${wt} && git branch -D ${branch}) or pass a fresh 'branch' argument.`,
    findings: [],
    verified: [],
    notCovered: ["the requested change — worktree/branch creation failed"],
  };
  return taken;
}
await world.run("bash", ["-c", `test -d '${wt}/protocols/upstream' || cp -r protocols/upstream '${wt}/protocols/'`], { output: { tail: 1024 } });
const wtConfigure = await world.run(
  "cmake",
  ["--preset", "release", "-S", wt, "-B", `${wt}/build/test`, "-DLUMEN_BUILD_TESTS=ON"],
  { timeoutMs: 600_000, output: { tail: 4096 } },
);
if (wtConfigure.exitCode !== 0) {
  const blockedWt: WorkflowReport = {
    conclusion: `CMake configure failed in the fresh worktree at ${wt}; nothing else ran.`,
    findings: [],
    verified: verified.concat([`worktree ${wt} created on branch ${branch} (left in place)`]),
    notCovered: ["everything after worktree configure"],
  };
  return blockedWt;
}
verified.push(`implementation worktree ${wt} on branch ${branch} — the main checkout stayed on its branch, untouched`);

phase("Implement the change in the worktree");
const implementer = agent("implementer", { system: implementerPersona });
const impl = await implementer.ask<ImplResult>(
  `Implement this task in the Lumen worktree at ${wt} (branch ${branch}, uncommitted handoff; ` +
    `build directory ${wt}/build/test, tests-enabled):\n\n${task}\n\nKeep the diff minimal. ` +
    `Answer with your summary, the changed files, and what the reviewer should look at hardest.`,
);
log(`Implemented; ${impl.changedFiles.length} files touched`);

// The diff itself decides what evidence the gate must see — an operator knob
// can add evidence, never waive it.
phase("Derive the required evidence from the diff");
const classifyRun = await world.run(
  "python3",
  ["scripts/classify-change.py", "--repo", wt, "--base", branchPoint],
  { output: { tail: 8192 }, },
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
    log("classifier output unparseable — falling back to ctest-only required evidence");
  }
} else {
  log("classifier failed — falling back to ctest-only required evidence");
}
verified.push(
  `required evidence derived from the diff: classes [${required.classes.join(", ")}] → ` +
    `ctest=${required.required.ctest} smokes=${required.required.smokes} bench=${required.required.bench_ab} ` +
    `goldens=${required.required.goldens} (host ${required.host})`,
);

const fixer = agent("fixer", { system: fixerPersona, budget: { webSearch: 0 } });
let gateGreen = false;
let gateRounds = 0;

phase("Build and test the change");
for (let round = 1; round <= 3 && !gateGreen; round++) {
  gateRounds = round;
  const build = await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], {
    timeoutMs: 1_200_000,
    output: { tail: 32_768 },
  });
  if (build.exitCode !== 0) {
    await fixer.ask(`The build failed (round ${round}) in ${wt}. Fix it with minimal changes. Tail of the output:\n${(build.stderr + build.stdout).slice(-4000)}`);
    continue;
  }
  const tests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 900_000, output: { tail: 32_768 } });
  if (tests.exitCode !== 0) {
    await fixer.ask(`The tests failed (round ${round}) in ${wt}. Fix them with minimal changes. Tail of the output:\n${(tests.stderr + tests.stdout).slice(-4000)}`);
    continue;
  }
  gateGreen = true;
}
if (!gateGreen) {
  const red: WorkflowReport = {
    conclusion: "The change never built and passed its tests within the fix budget; the branch holds the partial work.",
    findings: [],
    verified: verified.concat([`build+ctest attempted for ${gateRounds} fix rounds`]),
    notCovered: ["review — never reached", "the final gate — never reached"],
  };
  return red;
}
verified.push(`change green after ${gateRounds} fix round(s): tests-enabled build+ctest in ${wt}/build/test`);

phase("Review the diff");
const reviewer = agent("reviewer", { system: reviewerPersona, budget: { webSearch: 0 } });
const review = await reviewer.ask<Review>(
  `Review the uncommitted change in the worktree ${wt} on branch ${branch} (git -C ${wt} diff HEAD plus ` +
    `untracked files) against this task:\n\n${task}\n\nThe author's notes:\n${impl.notesForReviewer}\n\nReport every real ` +
    `finding with evidence. An empty findings list must mean you found nothing — not that you ` +
    `looked nowhere — and approving takes evidence, objecting is the useful move.`,
);
log(`Review returned ${review.findings.length} finding(s)`);

phase("Confirm each finding against the code");
const confirmed: Finding[] = [];
const refuted: Finding[] = [];
for (let i = 0; i < review.findings.length; i++) {
  const finding = review.findings[i];
  if (finding === undefined) continue;
  const confirmation = await agent(`confirmer-${i + 1}`, { system: confirmerPersona, budget: { webSearch: 0 } }).ask<Confirmation>(
    `Decide whether this finding is real — from its cited evidence, by reading those exact lines.\n\n` +
      `Change under review: the uncommitted diff in worktree ${wt} on branch ${branch} (git -C ${wt} diff HEAD plus untracked files).\n\n` +
      `Finding:\n${JSON.stringify(finding, null, 2)}\n\n` +
      `Bounded procedure: open the finding's cited path:line first; one targeted follow-up at most ` +
      `(e.g. git -C ${wt} diff HEAD -- <the cited path> — it also decides preexisting: cited lines untouched ` +
      `by the diff mean the defect exists at the base); do not re-derive the change. If that budget cannot ` +
      `decide it, return holds=false with reason "inconclusive from cited evidence" and the one read ` +
      `that would decide it. Report exactly what you read.`,
  );
  if (confirmation.holds) confirmed.push({ ...finding, preexisting: confirmation.preexisting });
  else refuted.push(finding);
  report({ ...finding, preexisting: confirmation.preexisting, holds: confirmation.holds, reason: confirmation.reason });
}
// Dispositions: refuted stays refuted; confirmed findings on code this change introduced are
// fixed below; confirmed pre-existing defects are recorded for .zcode/TODO.md and deferred to
// the user — the fixer must not widen the diff by repairing debt the change did not introduce.
const reportFindings: ReportFinding[] = [
  ...confirmed.map((f) => ({
    ...f,
    status: "verified" as const,
    disposition: f.preexisting === true ? ("deferred-to-user" as const) : ("fixed" as const),
  })),
  ...refuted.map((f) => ({ ...f, status: "unconfirmed" as const, disposition: "refuted" as const })),
];

const toFix = confirmed.filter((f) => f.preexisting !== true);
const preexistingFindings = confirmed.filter((f) => f.preexisting === true);
if (preexistingFindings.length > 0) {
  verified.push(
    `${preexistingFindings.length} confirmed pre-existing defect(s) — recorded for .zcode/TODO.md, not fixed here ` +
      `(they predate the change; fixing them would widen the diff)`,
  );
}
phase("Apply the confirmed findings");
if (toFix.length > 0) {
  await fixer.ask(
    `These findings were each independently confirmed on code this change introduced. Fix every one in ${wt}, minimal diffs:\n${JSON.stringify(toFix)}`,
  );
  gateGreen = false;
  for (let round = 1; round <= 2 && !gateGreen; round++) {
    const build = await world.run("cmake", ["--build", `${wt}/build/test`, "-j2"], {
      timeoutMs: 1_200_000,
      output: { tail: 32_768 },
    });
    if (build.exitCode !== 0) {
      await fixer.ask(`After applying review fixes the build failed (round ${round}). Tail:\n${(build.stderr + build.stdout).slice(-4000)}`);
      continue;
    }
    const tests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 900_000, output: { tail: 32_768 } });
    if (tests.exitCode !== 0) {
      await fixer.ask(`After applying review fixes the tests failed (round ${round}). Tail:\n${(tests.stderr + tests.stdout).slice(-4000)}`);
      continue;
    }
    gateGreen = true;
  }
  if (!gateGreen) {
    const red: WorkflowReport = {
      conclusion: "The confirmed findings were applied but the tree no longer builds/tests green within the budget.",
      findings: reportFindings.map((f) =>
        f.disposition === "fixed" ? { ...f, disposition: "deferred-to-user" as const } : f,
      ),
      verified: verified.concat(["confirmed findings were applied; build+ctest re-gate attempted"]),
      notCovered: ["the final gate — the tree is red after applying fixes"],
    };
    return red;
  }
  verified.push("confirmed findings applied, then build+ctest green again");
}

phase("Final gate: the full suite and the evidence the diff requires");
const finalTests = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "--output-on-failure", "-LE", "bench"], { timeoutMs: 900_000, output: { tail: 32_768 } });
const finalCountRun = await world.run("ctest", ["--test-dir", `${wt}/build/test`, "-N"], { output: { tail: 2048 } });
const finalCount = ctestTotal(finalCountRun.stdout + finalCountRun.stderr);
const countNote =
  baselineCount >= 0 && finalCount >= 0
    ? finalCount >= baselineCount
      ? `suite count computed at the gate: ${finalCount} (baseline ${baselineCount})`
      : `INVALID as a suite run: the tree registered ${finalCount} tests against the baseline's ${baselineCount} — tests were dropped`
    : "suite count not computable (ctest -N unreadable on one side)";
verified.push(`final gate: tests-enabled ctest on the finished change (${wt}/build/test, bench label excluded); ${countNote}`);

// Seat smokes: required by the classifier for seat-surface changes, addable by
// the operator for anything. The scripts are brokered (scripts/seat-lock.sh).
const needSmokes = required.required.smokes || runSmokes;
const needBench = required.required.bench_ab || runBench;
let smokeNote = needSmokes ? "" : "drag-smoke and app-smoke not required by the diff and run_smokes was not set";
let benchNote = needBench ? "" : "ui-bench not required by the diff and run_bench was not set";
const failRows = (out: string): string[] =>
  out.split("\n").filter((l) => /^FAIL\s+\S+/.test(l)).map((l) => ( /^FAIL\s+(\S+)/.exec(l) ?? [l, l] )[1] ?? l);
if (finalTests.exitCode === 0 && needSmokes) {
  const drag = await world.run("bash", ["-c", `cd '${wt}' && exec ./scripts/drag-smoke.sh --join-seat-session`], { timeoutMs: 420_000, output: { tail: 16_384 } });
  const app = await world.run("bash", ["-c", `cd '${wt}' && exec ./scripts/app-smoke.sh --join-seat-session`], { timeoutMs: 900_000, output: { tail: 16_384 } });
  const appOut = app.stderr + app.stdout;
  const appFails = failRows(appOut);
  verified.push(
    `drag-smoke ${drag.exitCode === 0 ? "PASS" : `FAIL (exit ${drag.exitCode})`}; ` +
      `app-smoke ${app.exitCode === 0 ? "PASS" : `FAIL (exit ${app.exitCode}; failing rows: ${appFails.join(", ") || "see log"})`}`,
  );
  smokeNote =
    (drag.exitCode === 0 ? "drag-smoke passed; " : `drag-smoke FAILED (exit ${drag.exitCode}); `) +
    (app.exitCode === 0 ? "app-smoke passed; " : `app-smoke FAILED (exit ${app.exitCode}, failing rows: ${appFails.join(", ") || "see log"}); `) +
    (required.required.smokes ? "required by the diff (seat-surface change)" : "operator-requested (run_smokes)");
}
if (finalTests.exitCode === 0 && needBench) {
  const bench = await world.run(
    "bash",
    ["-c", `cd '${wt}' && LUMEN_BUILD_DIR=build/test exec ./scripts/ui-bench.sh`],
    { timeoutMs: 1_800_000, output: { tail: 32_768 } },
  );
  benchNote =
    bench.exitCode === 0
      ? "ui-bench gate passed (FAIL on p50 +25%, WARN on p50 +10% or p95 +25% against the stored baseline)"
      : "ui-bench gate FAILED — per the no-degradation rule this goes to the user with the evidence, not auto-accepted";
  benchNote += required.required.bench_ab ? "; required by the diff (render-path change)" : "; operator-requested (run_bench)";
  verified.push(`ui-bench ${bench.exitCode === 0 ? "gate pass" : "gate FAIL"}`);
}
const countInvalid = countNote.startsWith("INVALID");
const gateFailed = finalTests.exitCode !== 0 || countInvalid || smokeNote.includes("FAILED") || benchNote.includes("FAILED");
const blockedOnHost = required.decisive_backend_blocked_on_host;
if (blockedOnHost) {
  verified.push(
    "BLOCKED-ON-HOST: this is a render-path change and this host has no hardware Vulkan — the decisive " +
      "present backend cannot be certified here (AGENTS.md §1.2); a notebook verification ticket is required before merge",
  );
}

const markdown = [
  `# Gated change: ${task.split("\n")[0]}`,
  "",
  `Worktree \`${wt}\`, branch \`${branch}\`, uncommitted there, ready to review and commit. The main checkout was never switched.`,
  "",
  "## Implementation",
  "",
  impl.summary,
  "",
  "## Review",
  "",
  review.verdict,
  "",
  `Findings: ${toFix.length} confirmed on this change (and fixed), ${preexistingFindings.length} confirmed pre-existing (record for .zcode/TODO.md), ${refuted.length} refuted by independent verification.`,
  "",
  `Review noise: ${review.findings.length} raised, ${confirmed.length} confirmed, ${refuted.length} refuted — the refuted share is the review stage's false-positive rate.`,
  "",
  ...reportFindings.map((f) => `- **${f.status}/${f.severity}${f.preexisting === true ? "/PRE" : ""}** \`${f.where}\` — ${f.what} (${f.disposition})`),
  "",
  "## Gate",
  "",
  `- Required evidence (derived from the diff): classes [${required.classes.join(", ")}] → ctest=${required.required.ctest}, smokes=${required.required.smokes}, bench=${required.required.bench_ab}, goldens=${required.required.goldens} on host ${required.host}`,
  `- ${verified.map((v) => v.replace(/^./, (c) => c.toUpperCase())).join("\n- ")}`,
  "",
  `Not covered: ${smokeNote || "seat smokes ran (see above)"}; ${benchNote || "bench ran (see above)"}.`,
  blockedOnHost ? "\n**BLOCKED-ON-HOST**: render-path change on a host without hardware Vulkan — notebook ticket required before merge." : "",
  "",
].join("\n");
await artifact.markdown("change-report", markdown, {
  title: "Change report",
  description: `Baseline → implement → review → verify → fix → gate, for: ${task.split("\n")[0]}`,
  primary: true,
});

const notCovered = [
  smokeNote || "seat smokes ran",
  benchNote || "bench ran",
  blockedOnHost
    ? "the decisive Vulkan present path cannot be certified on this host (no hardware Vulkan) — BLOCKED-ON-HOST, notebook ticket required"
    : "the KMS/Vulkan present path cannot be exercised on this VM (no hardware Vulkan)",
];
const done: WorkflowReport = {
  conclusion: gateFailed
    ? "The change is implemented and reviewed, but a final gate FAILED — per the no-degradation rule this needs the user's decision before commit."
    : blockedOnHost
      ? `The change is implemented, reviewed and suite-green on branch ${branch} in worktree ${wt} — but this host cannot certify the decisive backend (BLOCKED-ON-HOST); a notebook ticket is required before merge.`
      : `The change is implemented, independently reviewed (confirmed findings fixed) and gate-green on branch ${branch} in worktree ${wt}, uncommitted and ready.`,
  findings: reportFindings,
  verified,
  notCovered,
};
return done;
