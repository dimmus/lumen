> **Status:** Current (v0.3) — see code for implementation truth

# Health Evaluation

The session's health report: which parts of the compositor work, the processes of the
session, memory, and how well each output keeps its frame pace — aggregated into one
`health_report` with one level. `lumen-ctl status` shows it; the control socket's
`GetHealth` returns it (ADR 010 S8).

**Related:** [architecture.md](../architecture.md) §10 · [memory-management.md](memory-management.md) ·
[rendering-performance.md](rendering-performance.md) · [errors-and-logging.md](errors-and-logging.md) ·
[ADR 010](../adr/010-session-and-window-management.md)

---

## 1. Design goals

| Goal | Mechanism |
|------|-----------|
| Know which parts work, which fell back, which are stubs | `part_status` with a level, a detail line and `implemented` |
| Track the multi-process session | `process_status` for the compositor and the shell (pid, connected, bind count) |
| Memory visibility | pressure, RSS, import cache, staging against its budget |
| Speed | per output: presented frames, frames that missed their vblank, misses among the last 128 |
| **No cost when nobody asks** | pulled: the report is built at request time from state the compositor keeps anyway |
| No upward deps | `lx.health` is pure (Runtime layer); the compositor fills `health_inputs` |

---

## 2. Module layout

```
lx.health
├── types    — level, part_status, process_status, memory_status, speed_status, health_report
└── monitor  — miss_ring, health_thresholds, display_path, output_input, health_inputs,
               evaluate(), read_rss_kib()
```

**Layer:** Runtime (depends on `lx.foundation` only; no libsystemd, no Wayland).
**Consumers:** `lx.compositor` (the control socket's `GetHealth`), `lumen-ctl`.

---

## 3. Evaluation model

```mermaid
flowchart LR
    REQ[GetHealth / lumen-ctl status] --> UI[UI thread: facts it owns]
    UI --> POST[post to render]
    POST --> RENDER[render thread: its figures]
    RENDER --> BACK[post back to UI]
    BACK --> IN[health_inputs]
    IN --> EVAL[evaluate]
    EVAL --> RPT[health_report]
    POST -.->|"no answer in 500 ms"| CRIT[render: critical]
    CRIT --> IN
```

### Levels

| Level | Meaning |
|-------|---------|
| `ok` | Works as it should |
| `degraded` | Works, but not as it should: a fallback path, no shell connected, no input devices, moderate memory pressure, frames missing their vblank now and then |
| `critical` | Something the session cannot do without: the Wayland socket, the render thread, memory |
| `unknown` | Nothing to judge by yet |

Overall = **worst** of parts, processes, memory and speed (`unknown` loses to anything known).

---

## 4. What is judged

### Parts

| Part | ok | degraded / critical |
|------|----|---------------------|
| wayland | socket served | critical: no socket |
| display | KMS page flips (also while the session is in the background); virtual outputs | degraded: card open without DRM master, or a headless fallback |
| graphics | a composite backend (vulkan, gl, cpu) | degraded: no composite on a KMS display |
| render | the render thread answered the request (its time in ms) | critical: no answer within 500 ms |
| input | libinput devices on the seat | degraded: none, or the seat could not be opened |
| session | logind session in control (or virtual outputs) | degraded: a session not in control, or none on a real display |
| presentation | every output paced by page flips | degraded: a KMS output paced by a timer |
| systemd | (`--session` only) READY sent, environment exported, watchdog on | degraded: not ready yet, or the export failed |

### Processes

| Role | Signals |
|------|---------|
| compositor | pid; always connected |
| shell | the policy bridge's client: pid, connected, binds since start (1 + restarts); degraded while none is connected |

### Memory

Pressure from `memory_budget_coordinator` (normal ok, moderate degraded, critical critical);
RSS from `/proc/self/statm`; staging bytes against `staging_budget_bytes`; import cache
occupancy (full: degraded).

### Speed (per output)

Each content flip is counted as presented; it **missed** when it landed half a refresh
period or more after the vblank its repaint aimed at (`repaint_scheduler::next_deadline` at
the tick that served the output). The last 128 frames are kept as bits (`miss_ring`, one bit
write per presented frame — the only per-frame cost). With at least 16 recent frames: 8
misses in the window are degraded, 32 critical (`health_thresholds`). The last UI and render
tick times and the counts of ticks over the hot-path budget (`budget_tracker`) are reported
alongside.

---

## 5. Compositor wiring

Nothing runs per frame or on a timer for health. On `GetHealth` the compositor (UI thread)
posts a task to the render thread, which reads its own figures (import cache, last render
tick, render ticks over budget) and posts them back; the UI thread adds what it owns —
readiness of every part, logind, the shell bridge, memory pressure and staging, each
output's presented/missed counts and ring — and calls `lx::health::evaluate`. The control
socket answers with the report. A render thread that does not answer within 500 ms is
reported `critical` (the request is also a liveness probe of that thread).

While `lumen-ctl monitor` is subscribed, the compositor re-evaluates every 2 s and streams
a `health` event when the overall level changes; with no subscriber it never evaluates.

---

## 6. Export

| Way | What |
|-----|------|
| `io.lumen.Compositor.GetHealth` | the report as JSON (types `Health`, `Part`, `Process`, `Memory`, `Speed`; `varlinkctl introspect $LUMEN_SOCKET`) |
| `lumen-ctl status` | a summary: version, backend, uptime, overall level, whatever is not ok, the processes, memory, each output; plus the session's systemd units (active state, restarts). Exit 0 ok, 1 degraded, 2 critical, 3 not running |
| `lumen-ctl health` | every part, with tick times and budget counters; `--json` for the raw report |
| `io.lumen.Compositor.Subscribe` | `health` events on level changes |

---

## 7. Hot-path rules

- Per frame: one bit write into an output's `miss_ring` per presented content frame, and
  one store of the aimed vblank per output a tick serves. Nothing else.
- `evaluate` is pure and allocation-free; the report is a fixed-size value.
- The request's heap use (JSON) happens only per request, on the UI thread.

---

## 8. Implementation phases

| Phase | Deliverable | Status |
|-------|-------------|--------|
| **H0** | Types and evaluation | ✅ `lx.health` |
| **H1** | Real readiness facts | ✅ from the compositor's state at request time |
| **H2** | RSS and PIDs | ✅ `/proc/self/statm`, peer credentials of the shell's connection |
| **H3** | Exporter and `lumen-ctl` | ✅ Varlink control socket (`GetHealth`, `Subscribe`) |
| **H4** | Shell self-report over the policy bridge | Future |

---

## 9. UML

| Diagram | Topic |
|---------|-------|
| [38-health-evaluation.mmd](../uml/38-health-evaluation.mmd) | Request → facts → evaluate → report |
| [43-session-startup-sequence.mmd](../uml/43-session-startup-sequence.mmd) | The session start-up, READY and the control socket |
