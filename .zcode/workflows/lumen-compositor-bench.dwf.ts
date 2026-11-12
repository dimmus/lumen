/* zcode-workflow
description: "Benchmark Lumen's speed and memory footprint against sway, weston
  and (opt-in) hyprland on the real seat: interleaved rounds via the
  ~/lumen-compare harness (pres-probe presentation client), measuring startup
  (socket, first presented frame), commit→present p50/p95, callback fps,
  animating and idle CPU %, RssAnon MB and shutdown time. Produces a comparison
  table, a markdown report and a memory record."
whenToUse: When the user asks how fast or how heavy Lumen is compared to sway,
  weston or hyprland, or asks for a cross-compositor startup/latency/CPU/memory
  comparison. Needs a quiet seat (no other compositor or smoke session) and the
  lumen release build.
args:
  include_hyprland:
    type: boolean
    description: Also run Hyprland (it wedges when launched from SSH — only enable
      where a console launch is handled)
    default: false
  rounds:
    type: number
    description: Interleaved rounds per compositor (round 1 is a warm-up and
      excluded from medians)
    default: 5
  targets:
    type: json
    description: "Compositors to compare (json array); names: lumen, weston, sway, hyprland"
    default: '["lumen", "weston", "sway"]'
*/
interface CompRow {
  /** Compositor name as compare.sh reports it: lumen, weston, sway, hyprland. */
  name: string;
  /** Median ms until the Wayland socket exists. */
  socketMs: number;
  /** Median ms from exec to the probe's first presented frame. */
  firstPresentMs: number;
  /** Median commit→present p50 in ms. */
  p50Ms: number;
  /** Median commit→present p95 in ms. */
  p95Ms: number;
  /** Median callback fps of the driven client. */
  fps: number;
  /** Median % CPU while animating. */
  animCpuPct: number;
  /** Median % CPU while idle (~8 s window). */
  idleCpuPct: number;
  /** Median RssAnon in MB at end of animation. */
  rssMb: number;
  /** Median ms from SIGTERM to exit. */
  shutdownMs: number;
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
  findings: { where: string; what: string; evidence: string; status: "verified" | "unconfirmed"; severity: "low" | "medium" | "high" }[];
  /** What the run checked and how: the commands it ran. */
  verified: string[];
  /** What the run did not check, and why. */
  notCovered: string[];
}

const CMP = "/home/dimmus/lumen-compare";
const rounds = typeof args.rounds === "number" ? args.rounds : 5;
const includeHyprland = args.include_hyprland === true;
const targetsArg = Array.isArray(args.targets) ? args.targets.map(String) : ["lumen", "weston", "sway"];
const targets = (includeHyprland && !targetsArg.includes("hyprland") ? [...targetsArg, "hyprland"] : targetsArg)
  .filter((t) => ["lumen", "weston", "sway", "hyprland"].includes(t));
if (targets.length === 0) {
  const empty: WorkflowReport = {
    conclusion: "No valid comparator targets were given; nothing ran.",
    findings: [],
    verified: [],
    notCovered: ["everything — targets list was empty after validation"],
  };
  return empty;
}

artifact.table("comparison", {
  title: "Compositor comparison (medians)",
  columns: [
    { field: "name", label: "compositor" },
    { field: "socketMs", label: "socket ms" },
    { field: "firstPresentMs", label: "first frame ms" },
    { field: "p50Ms", label: "p50 ms", unit: "ms" },
    { field: "p95Ms", label: "p95 ms", unit: "ms" },
    { field: "fps", label: "fps" },
    { field: "animCpuPct", label: "anim CPU %" },
    { field: "idleCpuPct", label: "idle CPU %" },
    { field: "rssMb", label: "RSS MB", unit: "MB" },
    { field: "shutdownMs", label: "kill ms" },
  ],
  key: "name",
});

phase("Check the harness, the comparators and a quiet seat");
const harness = await world.run(
  "bash",
  ["-c", `test -x ${CMP}/pres-probe && test -f ${CMP}/compare.sh && test -x /home/dimmus/lumen/build/release/lumen-compositor && echo harness-ok`],
  { output: { tail: 2048 } },
);
if (harness.exitCode !== 0 || !harness.stdout.includes("harness-ok")) {
  const blocked: WorkflowReport = {
    conclusion: "The comparison harness is not ready (pres-probe, compare.sh or build/release/lumen-compositor missing); nothing ran.",
    findings: [],
    verified: ["preflight: harness presence check failed"],
    notCovered: ["everything — build lumen (release preset) and keep ~/lumen-compare built first"],
  };
  return blocked;
}
const missing: string[] = [];
for (const t of targets.filter((x) => x !== "lumen")) {
  const which = await world.run("bash", ["-c", `command -v ${t === "hyprland" ? "start-hyprland" : t} >/dev/null && echo ok || echo missing`], { output: { tail: 1024 } });
  if (!which.stdout.includes("ok")) missing.push(t);
}
const quiet = await world.run(
  "bash",
  ["-c", 'pgrep -ax lumen-compositor; pgrep -af "^bash .*-smoke\\.sh"; cat /proc/loadavg; true'],
  { output: { tail: 4096 } },
);
const busyLines = quiet.stdout.trim().split("\n").filter((l) => l.trim() !== "" && !/^\d+\.\d+ \d+\.\d+ \d+\.\d+/.test(l.trim()));
if (missing.length > 0 || busyLines.length > 0) {
  const blocked: WorkflowReport = {
    conclusion: `Preflight blocked: ${missing.length > 0 ? `not installed: ${missing.join(", ")}` : ""}${busyLines.length > 0 ? `${missing.length > 0 ? "; " : ""}seat not quiet (a compositor/smoke session is running)` : ""}.`,
    findings: busyLines.length > 0
      ? [{ where: "seat", what: "another compositor or smoke session holds the seat", evidence: busyLines.join(" | "), status: "verified", severity: "medium" }]
      : [],
    verified: ["preflight checks ran (harness present, comparators installed, seat quiet)"],
    notCovered: ["the comparison — rerun on a quiet host"],
  };
  return blocked;
}
const load1 = Number((quiet.stdout.trim().split("\n").pop() ?? "99").split(" ")[0]);
log(`Preflight ok (load ${load1}); targets: ${targets.join(", ")}`);

phase("Run the interleaved comparison rounds on the seat");
const compare = await world.run(
  "bash",
  ["-c", `ROUNDS=${rounds} NAMES=${targets.join(",")} exec ${CMP}/compare.sh`],
  { timeoutMs: 3_600_000, output: { tail: 65_536 } },
);
if (compare.exitCode !== 0) {
  const failed: WorkflowReport = {
    conclusion: "compare.sh exited nonzero — the comparison did not complete; its tail is in the evidence.",
    findings: [{ where: "compare.sh", what: `exit ${compare.exitCode}`, evidence: (compare.stderr + compare.stdout).slice(-2000), status: "verified", severity: "high" }],
    verified: [`attempted ROUNDS=${rounds} NAMES=${targets.join(",")}`],
    notCovered: ["the summary — rerun after fixing whatever the tail shows"],
  };
  return failed;
}
const outDir = await world.run("bash", ["-c", `cd ${CMP} && ls -t results | head -1`], { output: { tail: 1024 } });
const resultsDir = `${CMP}/results/${outDir.stdout.trim()}`;
const csv = await world.run("bash", ["-c", `cat '${resultsDir}/rows.csv'`], { output: { tail: 32_768 } });
log(`Results in ${resultsDir}`);

phase("Collect the medians and verify the numbers came from the run");
const rows = csv.stdout.trim().split("\n").slice(1).map((l) => l.split(","));
const byName = new Map<string, number[][]>();
for (const r of rows) {
  const name = r[0] ?? "";
  const round = Number(r[1] ?? "0");
  if (round === 1) continue; // warm-up round excluded, as the notebook methodology does
  const nums = r.slice(2).map(Number);
  if (!byName.has(name)) byName.set(name, []);
  byName.get(name)?.push(nums);
}
const median = (xs: number[]): number => {
  const s = [...xs].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  return s.length % 2 === 1 ? (s[mid] ?? 0) : ((s[mid - 1] ?? 0) + (s[mid] ?? 0)) / 2;
};
const comps: CompRow[] = [];
for (const name of targets) {
  const rs = byName.get(name);
  if (rs === undefined || rs.length === 0) continue;
  const col = (i: number): number => median(rs.map((r) => r[i] ?? 0).filter((v) => v > -1));
  const socketMs = col(0);
  const firstPresentMs = col(3);
  const p50Ms = col(4);
  const p95Ms = col(5);
  const fps = col(6);
  const animTicks = col(11);
  const idleTicks = col(12);
  const rssKb = col(13);
  const animCpuPct = fps > 0 ? (animTicks / 100 / (200 / fps)) * 100 : 0;
  const idleCpuPct = idleTicks / 100 / 8 * 100;
  const row: CompRow = {
    name, socketMs, firstPresentMs, p50Ms, p95Ms, fps,
    animCpuPct: Math.round(animCpuPct * 100) / 100,
    idleCpuPct: Math.round(idleCpuPct * 100) / 100,
    rssMb: Math.round((rssKb / 1024) * 10) / 10,
    shutdownMs: col(14),
  };
  comps.push(row);
  report(row, "comparison");
}
const missingRuns = targets.filter((t) => !comps.some((c) => c.name === t));
const verified = [
  `${CMP}/compare.sh ROUNDS=${rounds} NAMES=${targets.join(",")} on the real seat (exit 0)`,
  `medians over ${Math.max(rounds - 1, 1)} rounds (warm-up excluded) from ${resultsDir}/rows.csv`,
];
if (missingRuns.length > 0) verified.push(`no rows produced for: ${missingRuns.join(", ")}`);

phase("Write the report and the memory record");
const summaryTable = compare.stdout
  .split("\n")
  .filter((l) => /^(lumen|weston|sway|hyprland)\s+\d/.test(l.trim()) || /socket\s+config/.test(l));
const fastest = comps.length > 0
  ? comps.reduce((a, b) => (a.p50Ms <= b.p50Ms ? a : b))
  : undefined;
const conclusion = comps.length === 0
  ? "The comparison ran but produced no comparable rows; see the evidence."
  : `Compared ${comps.map((c) => c.name).join(", ")} on the real seat over ${rounds} interleaved rounds. ` +
    (fastest !== undefined
      ? `Lowest commit→present p50: ${fastest.name} at ${fastest.p50Ms} ms; full medians in the comparison table.`
      : "");
const memory = await agent("Memory writer", {
  system:
    "You write Lumen's project memory records. Work in the MAIN checkout at /home/dimmus/lumen. " +
    "Read .zcode/skills/lumen-memory-writer/SKILL.md if present and 2-3 files in .zcode/memories/ to match " +
    "the archive's voice. Write .zcode/memories/lumen-<kebab-topic>.md: YAML frontmatter (name, dense " +
    "description with dates/numbers), dated verified facts with inline evidence (commands, artifact paths, " +
    "measured numbers), then **Why:** and **How to apply:**. Add the MEMORY.md row. Commit both files on the " +
    "current branch with subject 'memory: <topic>' (git add the two files only; never push).",
  model: "GLM-5.3-Flash",
}).ask<MemoryRecord>(
  `Record this cross-compositor comparison. Host: the vmware Arch VM (vmwgfx, GL path, no hardware Vulkan; ` +
    `2 cores). Harness: ${CMP}/compare.sh with pres-probe, ${rounds} interleaved rounds, medians exclude round 1. ` +
    `Raw CSV: ${resultsDir}/rows.csv. Medians:\n${JSON.stringify(comps, null, 2)}\n` +
    `compare.sh summary lines:\n${summaryTable.join("\n")}\n` +
    `Note honestly what the VM cannot establish (hardware Vulkan numbers, multi-core hosts).`,
);
const markdown = [
  `# Lumen vs ${targets.filter((t) => t !== "lumen").join("/")} — ${rounds} interleaved rounds on the seat`,
  "",
  `Host: vmware Arch VM (vmwgfx, GL path, no hardware Vulkan), 2 cores. Harness: ~/lumen-compare`,
  `(pres-probe client, CPU/RSS sampled every 200 ms; medians over rounds 2–${rounds}).`,
  "",
  "| compositor | socket ms | first frame ms | p50 ms | p95 ms | fps | anim CPU % | idle CPU % | RSS MB | kill ms |",
  "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
  ...comps.map((c) =>
    `| ${c.name} | ${Math.round(c.socketMs)} | ${Math.round(c.firstPresentMs)} | ${c.p50Ms.toFixed(2)} | ${c.p95Ms.toFixed(2)} | ${c.fps.toFixed(2)} | ${c.animCpuPct.toFixed(2)} | ${c.idleCpuPct.toFixed(2)} | ${c.rssMb.toFixed(1)} | ${Math.round(c.shutdownMs)} |`,
  ),
  "",
  "Raw per-round rows: `" + resultsDir + "/rows.csv`; logs beside them.",
  "",
  !targets.includes("hyprland")
    ? "Hyprland not included: it wedges when launched from SSH (needs a console launch); add with include_hyprland only where that is handled."
    : "Hyprland was included via start-hyprland as the harness does.",
  "What this host cannot establish: hardware-Vulkan numbers (this VM has none) and multi-core behavior.",
  "",
].join("\n");
await artifact.markdown("bench-report", markdown, {
  title: "Cross-compositor benchmark report",
  description: `Startup, present latency, CPU and memory medians for ${targets.join(", ")} on the real seat.`,
  primary: true,
});
const done: WorkflowReport = {
  conclusion,
  findings: missingRuns.map((m) => ({
    where: `comparison ${m}`,
    what: `no rows were produced for ${m} despite being targeted`,
    evidence: `${resultsDir}/rows.csv has no ${m} rows`,
    status: "verified",
    severity: "medium",
  })),
  verified,
  notCovered: [
    "hardware Vulkan: this VM has none — the GL path is measured; hardware comparisons need the notebook",
    "memory beyond RssAnon (no heaptrack profile of each compositor)",
    ...(targets.includes("hyprland") ? [] : ["Hyprland (needs a console launch)"]),
  ],
};
void memory;
return done;
