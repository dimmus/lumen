# Executing the Workflow with DSH

DSH has tool-level orchestration primitives, not an implicit OpenRig service. Select the smallest mechanism that preserves task identity and role independence.

## Primitive Mapping

| Need | DSH mechanism | Notes |
|---|---|---|
| Persist whole-session objective across automatic continuation | `create_goal` / `get_goal` / `update_goal` | Use only for long-running objectives; resume the disarmed goal after a user asks to continue. |
| Maintain visible work steps | `todo_write` | For Lead's own sequence; not a replacement for per-task shared work records. |
| Durable independently owned tasks | `team_task_create/list/get/update` | Claim with current revision; use blockers only for real prerequisites. |
| Run a selected independent specialist now | `subagent`, `subagent_claude_code`, `subagent_codex`, or TeamMate | Separate worker identity; only tools/catalog routes actually available. State exact task/worktree/scope. |
| Persistent team members / direct followups | `spawn_teammate`, `list_agents`, `send_message`, `wait_agent`, `interrupt_agent` | Inactive does not mean complete. Lead waits for required members; query results before final answer. |
| Purpose-built broad independent fan-out | `workflow` | Use only when user requests workflow/large multi-agent orchestration. It offers `pipeline` with independent per-item stages and `parallel` barrier; agent code receives no filesystem/network/timer APIs, so workers must do the work. |
| Background command | `pwsh` with `run_in_background` | Track every job ID, continue independent work, collect all results. A completed shell command is not a test pass absent inspected evidence. |
| Separate source tree | `create_worktree` | Lead first checks dirty status and base. Keep working directories distinct; task-specific worktree created from HEAD omits dirty dependencies. |
| Host/editor file edits | `read/edit/write` | Prefer guarded text edits. Never rewrite user files via shell scripts. |
| Independent code review | `subagent` with exact candidate and read-only role prompt | Do not make independent reviewers co-authors; enumerate all relevant untracked files. |

## Shared Tasks for Concurrent Lanes

1. Create tasks for independent outcomes before dispatch. Every task gets a task card from `references/task-card.md`. If discovery/research or architecture is a true prerequisite, create that as an upstream task; otherwise permit read-only context gathering in parallel.
2. Add dependencies with `blocked_by` only when downstream acceptance literally requires upstream output. A single global “baseline complete” lock is usually wasteful: parallelize baseline checks for independent candidate/build environments, but serialize accesses to the same build directory/container or CPU-heavy compiler. After a shared base baseline receipt exists, link it into all consumers.
3. Create shared tasks unowned, then Lead `get` + `claim` and assign worker identity, or Lead-only reassign. Teammates are not auto-started by readiness. A claimed task doesn't prove completion; ask for receipt/evidence, inspect files/diff and task state before completing.
4. Launch only the current ready wave subject to CPU/memory, model/provider budget, write scopes and locks. On this host, 2 physical cores/4 logical processors: one CPU-heavy builder/test process at a time, plus coordination; short read-only reviewer/research tasks can overlap if token budget permits. Recompute on another host. Never run full CTest, a fuzz campaign, two builds, real-seat smoke, or A/B measurement concurrently just because there are free agent slots.
5. Do not wait for unrelated task lanes. If task A has frozen candidate+review available, A may run its review/verification while B is still implementing. Gating depends on A's prerequisites, not global progress. Parallel review/verification of the **same frozen candidate** may be allowed when they inspect independent evidence and do not contend for resources; candidate mutation invalidates either branch's result.

## Workflow Tool Patterns

Use `workflow` for broad explicitly requested fan-out, such as N disjoint modules each requiring an independent inventory. Prefer `pipeline(items, stages...)` if each item independently progresses through inspect→edit→review. A phase should not impose a barrier unless later work consumes all prior results. Example conceptual pattern:

```js
phase("Independent task lanes");
const lanes = await pipeline(
  args.tasks,
  async (task) => await agent(`Inspect task ${task.id}; return bounded design/acceptance`, { label: `plan-${task.id}` }),
  async (plan, task) => await agent(`Implement ${task.id} only in ${task.work_scope}; plan=${plan}`, { label: `impl-${task.id}` }),
  async (impl, task) => await agent(`Read-only review exact candidate for ${task.id}; result=${impl}`, { label: `review-${task.id}` })
);
return lanes;
```

This is an example of API shape, not deployable code: real workers need self-contained prompts and filesystem access through their own agent tools. The script cannot read source files. Never pass only task IDs without included objective, scope, base, acceptance, constraints and available paths.

`pipeline()` creates a serial chain within each item even though items have no cross-item barrier. It is suitable for independent mini-workflows, but must not auto-review mutable work and then continue editing after a gate. Freeze/hash the candidate before review/verification; any edit becomes a new candidate and invalidates downstream receipts. For arbitrary partial dependencies use DSH shared tasks, with `blocked_by` representing actual prerequisites. `parallel()` is a barrier and returns in input order; use for confirmations over a known finding set after reviewers have completed. Filter `null` failures explicitly. Do not call `parallel` with dozens of heavy build/test agents. Child failures must stay visible as unresolved, not be dropped from the final lane summary. Use schema-validated child output when reliable structured records are required.

## Checkpoints and Pause

Before context/usage pressure, emit a concise checkpoint with workflow ID and per-task card IDs/states, worker targets, worktrees/candidates, current job IDs and locks, evidence artifacts, open findings, user decisions needed and one next action per lane. Never state a worker is dead just because no message arrived. Continue after resumption from current shared-task/agent/job state, not from stale memory alone.
