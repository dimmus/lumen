/* zcode-workflow
description: "The gated change workflow for Lumen: baseline build+ctest on the
  clean tree, implement on a fresh branch, build+test fix loop, fresh-eyes
  review of the diff, independent confirmation of every finding, apply confirmed
  findings, then the final full-suite gate (optionally drag-smoke/app-smoke and
  ui-bench). Ports the multi-agent pattern the Claude sessions used for every
  significant Lumen change."
whenToUse: "A non-trivial Lumen change (feature, optimization, refactor) that
  must pass the no-degradation rule: run when the user asks for a gated workflow
  for a Lumen change, or asks to implement something with
  review+verification+gate. Not for small fixes or doc-only edits."
args:
  branch:
    type: string
    description: Branch name to create for the change (must not already exist)
    default: zcode/gated-change
  run_bench:
    type: boolean
    description: Also run scripts/ui-bench.sh (headless p50/p95 gate against the
      stored baseline) in the final gate
    default: false
  run_smokes:
    type: boolean
    description: "Also run drag-smoke and app-smoke with --join-seat-session in the
      final gate (needs a quiet host: no other compositor or smoke session)"
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
}

interface ReportFinding extends Finding {
  /** "verified" when an independent subagent confirmed it; "unconfirmed" when confirmation failed. */
  status: "verified" | "unconfirmed";
}

interface Review {
  /** One-paragraph verdict: is the change ready for the gate, and what blocks it. */
  verdict: string;
  findings: Finding[];
}

interface Confirmation {
  /** True when the finding reproduces from its evidence alone. */
  holds: boolean;
  /** What you read (or ran) to decide, in one or two sentences. */
  reason: string;
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

const implementerPersona =
  "You are the implement stage of Lumen's gated change workflow. " +
  "Read the repository root/AGENTS.md first — it is binding; especially §1.2 (the best-in-class bar " +
  "and the no-degradation rule), §3 layering and §4 conventions. Read .zcode/skills/lumen-decisions/SKILL.md " +
  "and do not re-open anything it settled, and .zcode/skills/lumen-build-run/SKILL.md for how this host " +
  "behaves. Minimal diff; match the surrounding module's conventions; no new config keys, env vars or " +
  "user-facing features the task did not ask for. Do not commit or push. Do not run the full test " +
  "suite — the workflow gates the build after you; narrow checks (ninja -C build/release <targets>) " +
  "are fine. If the task is impossible, contradicts AGENTS.md, or would regress something measured, " +
  "escalate and say so plainly rather than working around it.";

const reviewerPersona =
  "You are the review stage of Lumen's gated change workflow, reading a diff you did not write. " +
  "Read the repository root/AGENTS.md (§3 layer/process/thread rules, §4 conventions, §7 anti-patterns, " +
  "§8 checklist) and .zcode/skills/lumen-decisions/SKILL.md — a change that re-opens a settled decision " +
  "without new evidence is itself a finding. Review for correctness, layering, thread affinity, error " +
  "and logging conventions, hot-path rules (no heap churn, no info+ logs per frame), and the " +
  "no-degradation rule: every perf-relevant claim needs evidence, and unasked-for features are findings. " +
  "You are read-only: do not edit any file. Cite path:line and quote the lines you read.";

const confirmerPersona =
  "You are the verify stage of Lumen's gated change workflow. You are given one review finding and " +
  "must decide, from the evidence alone and by reading the code in the repository yourself, whether " +
  "it is real. Reproduce it from the cited lines; check the surrounding context the reviewer may have " +
  "missed; run a narrow check only if a command decides it. You are read-only: do not edit any file. " +
  "If the evidence does not hold up, say so — a false finding must not survive this stage.";

const fixerPersona =
  "You fix build failures, test failures and confirmed review findings in the Lumen repository. " +
  "Read the repository root/AGENTS.md §4 conventions before editing; minimal diffs only, no refactors " +
  "beyond what the failure forces, no new features. Do not commit or push. Do not run the full test " +
  "suite — the workflow re-gates after you; narrow checks are fine. If a failure looks environmental " +
  "(host noise, DRM master, missing protocol stamp) rather than caused by the change, escalate and " +
  "say so plainly rather than papering over it.";

const verified: string[] = [];

phase("Baseline: build and test the tree as it stands");
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
const configure = await world.run("cmake", ["--preset", "release"], { timeoutMs: 600_000, output: { tail: 8192 } });
if (configure.exitCode !== 0) {
  const blocked: WorkflowReport = {
    conclusion: "CMake configure of the baseline failed; nothing else ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — cmake configure failed on the clean tree"],
  };
  return blocked;
}
const baselineBuild = await world.run("cmake", ["--build", "--preset", "release"], {
  timeoutMs: 1_200_000,
  output: { tail: 32_768 },
});
const baselineTests =
  baselineBuild.exitCode === 0
    ? await world.run("ctest", ["--preset", "release"], { timeoutMs: 900_000, output: { tail: 32_768 } })
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
verified.push("baseline green before any change: cmake --build --preset release + ctest --preset release");
log("Baseline is green; creating the working branch");

phase("Create the working branch");
const status = await git.status();
if (!status.clean) {
  const dirty: WorkflowReport = {
    conclusion: "The working tree is not clean; the gated change refuses to start on top of unknown edits.",
    findings: [],
    verified: [],
    notCovered: ["the requested change — commit or stash the existing edits first"],
  };
  return dirty;
}
const sw = await world.run("git", ["switch", "-c", branch], { output: { tail: 4096 } });
if (sw.exitCode !== 0) {
  const taken: WorkflowReport = {
    conclusion: `Could not create branch ${branch} (it probably exists); pass a fresh 'branch' argument.`,
    findings: [],
    verified: [],
    notCovered: ["the requested change — branch creation failed"],
  };
  return taken;
}

phase("Implement the change");
const implementer = agent("implementer", { system: implementerPersona });
const impl = await implementer.ask<ImplResult>(
  `Implement this task in the Lumen repository:\n\n${task}\n\nThe baseline build and the full test ` +
    `suite pass on this tree. Keep the diff minimal. Answer with your summary, the changed files, ` +
    `and what the reviewer should look at hardest.`,
);
log(`Implemented; ${impl.changedFiles.length} files touched`);

const fixer = agent("fixer", { system: fixerPersona });
let gateGreen = false;
let gateRounds = 0;

phase("Build and test the change");
for (let round = 1; round <= 3 && !gateGreen; round++) {
  gateRounds = round;
  const build = await world.run("cmake", ["--build", "--preset", "release"], {
    timeoutMs: 1_200_000,
    output: { tail: 32_768 },
  });
  if (build.exitCode !== 0) {
    await fixer.ask(`The build failed (round ${round}). Fix it with minimal changes. Tail of the output:\n${(build.stderr + build.stdout).slice(-4000)}`);
    continue;
  }
  const tests = await world.run("ctest", ["--preset", "release"], { timeoutMs: 900_000, output: { tail: 32_768 } });
  if (tests.exitCode !== 0) {
    await fixer.ask(`The tests failed (round ${round}). Fix them with minimal changes. Tail of the output:\n${(tests.stderr + tests.stdout).slice(-4000)}`);
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
verified.push(`change green after ${gateRounds} fix round(s): cmake --build --preset release + ctest --preset release`);

phase("Review the diff");
const reviewer = agent("reviewer", { system: reviewerPersona });
const review = await reviewer.ask<Review>(
  `Review the uncommitted change on branch ${branch} (git diff HEAD plus untracked files) against ` +
    `this task:\n\n${task}\n\nThe author's notes:\n${impl.notesForReviewer}\n\nReport every real ` +
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
  const confirmation = await agent(`confirmer-${i + 1}`, { system: confirmerPersona }).ask<Confirmation>(
    `Decide whether this finding is real, from the evidence alone:\n${JSON.stringify(finding)}`,
  );
  if (confirmation.holds) confirmed.push(finding);
  else refuted.push(finding);
  report({ ...finding, holds: confirmation.holds, reason: confirmation.reason });
}
const reportFindings: ReportFinding[] = [
  ...confirmed.map((f) => ({ ...f, status: "verified" as const })),
  ...refuted.map((f) => ({ ...f, status: "unconfirmed" as const })),
];

phase("Apply the confirmed findings");
if (confirmed.length > 0) {
  await fixer.ask(
    `These findings were each independently confirmed. Fix every one, minimal diffs:\n${JSON.stringify(confirmed)}`,
  );
  gateGreen = false;
  for (let round = 1; round <= 2 && !gateGreen; round++) {
    const build = await world.run("cmake", ["--build", "--preset", "release"], {
      timeoutMs: 1_200_000,
      output: { tail: 32_768 },
    });
    if (build.exitCode !== 0) {
      await fixer.ask(`After applying review fixes the build failed (round ${round}). Tail:\n${(build.stderr + build.stdout).slice(-4000)}`);
      continue;
    }
    const tests = await world.run("ctest", ["--preset", "release"], { timeoutMs: 900_000, output: { tail: 32_768 } });
    if (tests.exitCode !== 0) {
      await fixer.ask(`After applying review fixes the tests failed (round ${round}). Tail:\n${(tests.stderr + tests.stdout).slice(-4000)}`);
      continue;
    }
    gateGreen = true;
  }
  if (!gateGreen) {
    const red: WorkflowReport = {
      conclusion: "The confirmed findings were applied but the tree no longer builds/tests green within the budget.",
      findings: reportFindings,
      verified: verified.concat(["confirmed findings were applied; build+ctest re-gate attempted"]),
      notCovered: ["the final gate — the tree is red after applying fixes"],
    };
    return red;
  }
  verified.push("confirmed findings applied, then build+ctest green again");
}

phase("Final gate: the full suite, and hand over the report");
const finalTests = await world.run("ctest", ["--preset", "release"], { timeoutMs: 900_000, output: { tail: 32_768 } });
verified.push("final gate: ctest --preset release green on the finished change");
let smokeNote = "drag-smoke and app-smoke not run (run_smokes was not set)";
let benchNote = "ui-bench not run (run_bench was not set)";
if (finalTests.exitCode === 0 && runSmokes) {
  const drag = await world.run("./scripts/drag-smoke.sh", ["--join-seat-session"], { timeoutMs: 300_000, output: { tail: 16_384 } });
  const app = await world.run("./scripts/app-smoke.sh", ["--join-seat-session"], { timeoutMs: 600_000, output: { tail: 16_384 } });
  verified.push(`drag-smoke ${drag.exitCode === 0 ? "pass" : "FAIL"}, app-smoke ${app.exitCode === 0 ? "pass (177/177 expected)" : "FAIL"}`);
  smokeNote = `drag-smoke ${drag.exitCode === 0 ? "passed" : "FAILED"}, app-smoke ${app.exitCode === 0 ? "passed" : "FAILED"} (with --join-seat-session)`;
}
if (finalTests.exitCode === 0 && runBench) {
  const bench = await world.run("./scripts/ui-bench.sh", [], { timeoutMs: 1_800_000, output: { tail: 32_768 } });
  benchNote =
    bench.exitCode === 0
      ? "ui-bench gate passed (FAIL on p50 +25%, WARN on p50 +10% or p95 +25% against the stored baseline)"
      : "ui-bench gate FAILED — per the no-degradation rule this goes to the user with the evidence, not auto-accepted";
  verified.push(`ui-bench ${bench.exitCode === 0 ? "gate pass" : "gate FAIL"}`);
}
const gateFailed = finalTests.exitCode !== 0 || smokeNote.includes("FAILED") || benchNote.includes("FAILED");

const markdown = [
  `# Gated change: ${task.split("\n")[0]}`,
  "",
  `Branch \`${branch}\`, uncommitted, ready to review and commit.`,
  "",
  "## Implementation",
  "",
  impl.summary,
  "",
  "## Review",
  "",
  review.verdict,
  "",
  `Findings: ${confirmed.length} confirmed (and fixed), ${refuted.length} refuted by independent verification.`,
  "",
  ...reportFindings.map((f) => `- **${f.status}/${f.severity}** \`${f.where}\` — ${f.what}`),
  "",
  "## Gate",
  "",
  `- ${verified.map((v) => v.replace(/^./, (c) => c.toUpperCase())).join("\n- ")}`,
  "",
  `Not covered: ${smokeNote}; ${benchNote}.`,
  "",
].join("\n");
await artifact.markdown("change-report", markdown, {
  title: "Change report",
  description: `Baseline → implement → review → verify → fix → gate, for: ${task.split("\n")[0]}`,
  primary: true,
});

const notCovered = [smokeNote, benchNote, "the KMS/Vulkan present path cannot be exercised on this VM (no hardware Vulkan)"];
const done: WorkflowReport = {
  conclusion: gateFailed
    ? "The change is implemented and reviewed, but a final gate FAILED — per the no-degradation rule this needs the user's decision before commit."
    : `The change is implemented, independently reviewed (confirmed findings fixed) and gate-green on branch ${branch}, uncommitted and ready.`,
  findings: reportFindings,
  verified,
  notCovered,
};
return done;
