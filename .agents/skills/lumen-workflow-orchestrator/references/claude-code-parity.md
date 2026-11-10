# Extending Toward Claude Code Harness Parity

## Current status

This repository configures **agent workflow instructions** for DSH. The skills tell a coordinator how to use DSH tasks/subagents/workflows and define specialist contracts. They do not add executable hooks, new agents to runtime, permission policy, shell integration, CLI commands, lifecycle persistence, or CI enforcement. DSH currently supplies tool-level `create_goal`, shared tasks, named Agent Teams, one-off subagents and a JavaScript `workflow` fan-out API; it is not a long-lived rig daemon. User-visible model IDs/providers, permissions and context limits are host/session configured.

OpenRig (reference revision `c38d1183cb192ce8ab9d27dc302afaefda860ab2`) suggests useful patterns: stable task+attempt identity, DAG ready waves, named role seats, durable receipts, safe uncertain-state semantics, checkpoint/resume and terminal lifecycle decisions. DSH mapping is via the actual tools; do not install/pretend OpenRig commands, tmux state, YAML RigSpecs, or remote dispatch exist. Sources: [OpenRig README](https://github.com/mvschwarz/openrig/tree/c38d1183cb192ce8ab9d27dc302afaefda860ab2), [RigSpec](https://github.com/mvschwarz/openrig/blob/c38d1183cb192ce8ab9d27dc302afaefda860ab2/docs/reference/rig-spec.md), and its workflow documents. OpenRig is research, not a dependency.

## Parity matrix and candidate extensions

| Claude Code harness capability | DSH/Lumen status | Practical path / gap |
|---|---|---|
| Native role agents and slash commands | DSH skill catalog, Agent Teams and subagents exist | Project skills provide role prompts; no dedicated always-running seats unless the runtime team tool creates them. Test catalog discovery after updates. |
| Parallel agents | Available through subagents, teams, workflow `parallel`/`pipeline` | Current host is 2 physical cores: cap CPU-heavy concurrency at one by default; use read-only parallel agents within tool/provider budgets. No CPU auto-scheduler is implemented in these skills. |
| Long-lived agent tasks | DSH shared task board and goals | Task board is durable within DSH. No Lumen-specific DAG service or cross-session dispatch daemon. Ensure per-task ownership/claims/revisions and manual continuation receipts. |
| Worktree isolation | DSH worktrees tool / explicit Git worktree tools | Lead creates one workspace per active implementer; baseline/candidate dependency must be handled because uncommitted files are not copied. No automatic merge promotion. |
| Workflow hooks and lifecycle automation | Not introduced | Skills do not hook pre-tool/post-tool, branch changes, commits or prompts. To add automatic runtime hooks requires a separate DSH plugin/Host integration with cordis Inspect-driven design and user approval. |
| Permission/sandbox enforcement | Supplied by DSH session/plugin/Host configuration | Role text cannot enforce read-only, path-limited writes or block shell/network. Use actual DSH permissions/plugin controls; inspect current Host tools and policy before claiming enforcement. |
| Candidate snapshots and immutable artifacts | Process described; no dedicated store | Could add a versioned evidence bundle/manifest schema and CI artifact retention. Candidate hashes plus immutable logs are required even when manually recorded. |
| CI checks and branch protection | Repository-specific CI, if configured | CI manager skill can inspect/edit assigned workflow but does not cause a hosted run. Add required status checks and branch protections separately, owner-controlled. |
| Usage/quota awareness and checkpoint | DSH goals/jobs and user continuation | Skills instruct checkpoint at quota/compaction. No guaranteed token budget API, quota monitor, automatic wait-until-reset or run resurrection. A scheduler/plugin would be required. |
| User-visible control plane and live task board UI | DSH GUI/task tools; no Lumen-specific page in this change | A Cordis page/tool could show per-task state/evidence only after a separate design: inspect providers/Slots/services first; keep state authenticated and read-only at first. |
| Model routing | Session model/provider and supported subagent route discovery | Do not hardcode Pi/Codex/Claude CLI or brand-based role assumptions. Query `list_subagent_models`; record effective routes only when observable. External providers require setup and credentials. |
| Resilient distributed worker system | DSH-managed runtime features | No remote worker host/exactly-once dispatch guarantee is created by these skill files. Use DSH-native team/subagent tools and preserve uncertainty. |
| Autonomous CI/CD/deployment | Not authorized by role skill | Deployment/publication remains an explicit user task with environment approvals; no auto-deploy. |

## Safe roadmap to deepen parity

1. **Task evidence package (low risk):** add a lightweight schema plus a validator that checks IDs, candidate hashes, stage receipts and PASS/FAIL states; store as CI artifacts. Do not add custom long-term DB until need is demonstrated.
2. **Lumen CI contract:** map baseline/unit/sanitizer/fuzz/format/static checks into CI. Hardware seat/KMS/Vulkan gates should be labeled separate/manual or run on a verified hardware runner; never mask unavailable gates as passed.
3. **DSH runtime orchestration integration:** only if users need automatic dispatch/locks/checkpoints, design a Cordis Host service/plugin after reading current DSH plugin and Inspect provider contracts. Use provider discovery before any plugin code. Decide store, crash consistency, user approval, permissions, audit and compatibility before implementation.
4. **Read-only control surface:** optionally propose a personalized page after loading `personalized-pages` if explicitly requested; inspect live Slots/props and available Inspect methods. The page must not issue task release, branch or deployment side effects without explicit action/confirmation.
5. **Quota-aware continuation:** first use an observable provider/quota signal only if a supported API exists; otherwise keep manual checkpoint instructions. No polling/scheduling should be invented as if it reactivates a model quota.
6. **Enforcement:** evaluate built-in DSH permissions, plugins/hooks or CI protected branches. Keep skill guidance and actual technical enforcement clearly distinct; no role prompt is a security boundary.

Claude Code parity is a proposition, not a claim that DSH can be made feature-identical by adding more skills. Any runtime/UI/plugin work has a separate scope and should start with current DSH plugin-development/agent-experience instructions and an explicit user request.
