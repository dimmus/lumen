---
name: lumen-portal-appearance-settings
description: "2026-11-16 notebook task (worktree portal-appearance-settings, 90b64bd+729fbc8, fixes 8cd7e7a/a03eb9a/fedbba1, HEAD fedbba1, unmerged, gate NOT_READY on unattributed seat reds): xdg-desktop-portal-lumen serves org.freedesktop.impl.portal.Settings v1 — [ui] appearance → org.freedesktop.appearance color-scheme (dark→1/light→2) + contrast 0, ReadAll trailing-glob filter, rename-safe config_watch flip → SettingChanged per key; Zed 1.28.1 (binary is zeditor!) followed light→dark→light live (window means 0.925/0.222/0.925); suite 135/135; disproven: hyprland has no Settings backend, 'no zed on this host'; seat reds = drag 9/10 FAIL + app-smoke 101 FAIL rows (Z2/Z4 portal black capture) with no baseline A/B — remediation owed, TODO-tracked"
metadata:
  node_type: memory
  type: project
summary: "xdg-desktop-portal-lumen serves impl.portal.Settings v1 → org.freedesktop.appearance; Zed followed light/dark/live; Nov-16 seat reds were the concurrent-session family, closed by redo"
modules: lx.portal
topics: portal, settings, dark-mode

---

Verified 2026-11-16 on the **notebook** (iritur i915+nouveau, [[lumen-notebook-hw]]; the
memory-writer session re-ran the key checks — each claim below names the session that
produced it). Worktree `/home/dimmus/lumen-wt/portal-appearance-settings`, branch
`zcode/task/portal-appearance-settings` off main 39fe3ac, HEAD **fedbba1** (2026-11-16
19:35), **unmerged** (ran: `git merge-base --is-ancestor HEAD main` → not an ancestor),
worktree clean.

**What shipped** — 90b64bd (7 files +446/−9), 729fbc8 (deploy, 2 files); review-fix round
8cd7e7a, a03eb9a, fedbba1:

- xdg-desktop-portal-lumen serves **org.freedesktop.impl.portal.Settings** v1: a second
  `sd_bus_add_object_vtable` at the existing object path (ReadAll `a{sa{sv}}`, Read `v`,
  SettingChanged `ssv`, version `1u`). Served surface is exactly org.freedesktop.appearance:
  **color-scheme** from `[ui] appearance` (dark→1, light→2; 0 is never served) and
  **contrast** (constant 0); accent-color/reduced-motion and every unknown namespace/key
  answer `org.freedesktop.portal.Error.NotFound` / "Requested setting not found" — gtk's
  exact error and message.
- New pure `:settings` partition (settings.cppm/.cpp, 84+75 lines) holds the mapping,
  ReadAll's trailing-glob namespace filter (empty list/pattern = all; mirrors gtk's
  namespace_matches) and the reload/flip state machine over lx.config's rename-safe
  `config_watch`; values re-read through the settings app's own truth (locate_config →
  read_config → parse_config, env_lookup threaded for tests). The service loop's pollfd
  grew **4→5** with the inotify fd appended *last*, so the hard indices 0–3
  (bus/signalfd/wl/ui) keep their positions; SettingChanged emits **per key only on an
  actual value flip**. `LUMEN_CONFIG=0` degrades explicitly: defaults served, no watch.
- Routing: `deploy/portal/lumen.portal` Interfaces += Settings; `deploy/portal/lumen-portals.conf`
  moves Settings gtk→lumen (header comment states the move); CMakeLists.txt:487 adds
  lx.config to lx.portal DEPENDS; the test registers at CMakeLists.txt:950-955 beside
  test_portal_compose. color-scheme derives from `[ui] appearance` **only**, not a named
  theme — the ask's contract, deliberately not "improved". gtk never activates under
  XDG_CURRENT_DESKTOP=Lumen, so this takes over nothing that worked.

**Evidence:**

- Tests (ran this session): `./build/test/test_portal_settings` → **4/4 PASS** (mapping;
  namespace-filter glob; rename flip / same-value no-flip / flip-back; disabled-config);
  full tests-enabled suite `ctest --test-dir build/test --output-on-failure -LE bench` →
  **"100% tests passed out of 135", 77.29 s** (`LUMEN_BUILD_TESTS:BOOL=ON` in the cache;
  pre-change baseline 134 — this test is the +1).
- Live Zed end-to-end (review round 2, 19:38–19:42, artifacts /tmp/zcheck-shots/):
  shell.log:33 `app_id=dev.zed.Zed`; portal-lumen.log startup
  `color-scheme=2 contrast=0 (watched: 1)` then `color-scheme 2 -> 1` and `1 -> 2`; the
  three 3200x1800 screenshots' window region measures mean **0.925 / 0.222 / 0.925** —
  light→dark→light in sync with the portal flips (gate re-measured 0.929/0.201/0.929);
  `~/.config/zed` holds no settings.json, so theme=System was in force — the acceptance's
  named client check is genuinely covered.
- Host finding (ran this session): **Zed on this host is `/usr/bin/zeditor`** —
  `zeditor --version` → "Zed 1.28.1 – /usr/lib/zed/zed-editor" (pacman `zed 1.28.1-1`);
  `command -v zed` is empty.

**Fuckups and their fixes:**

- **The "not runnable" trap (review finding 2, medium):** round 1 skipped the Zed
  acceptance because `command -v zed` found nothing — the binary is renamed `zeditor`.
  Fix: round 2 ran the real check on this host and it passed (above). Before declaring a
  named client not-runnable, check the owning package and alternate binary names
  (`pacman -Qo`, `pacman -Qs`).
- **Reply leak (finding 4, low):** `on_read_all` returned negative errno at five
  intermediate append-failure sites without unreffing the manually built sd_bus_message —
  8cd7e7a unrefs at all five (service.cpp:362-401); the success path still hands the ref
  to sd_bus_send.
- **Scratch leak (finding 3, low):** the new test leaked one mkdtemp dir per run (6
  /tmp/lumen-portal-settings-test-* dirs predate the fix) — a03eb9a `rm -rf` at the tail,
  matching test_config's cleanup convention.
- **Live-session fragility:** the first run-desktop run hit its own 420 s safety limit
  before the flip/strace probes, and between runs an external actor (most likely the user,
  active on this host) rewrote `~/.config/lumen/config.toml` back to its original bytes
  (hash-verified) — it still reads `appearance = "light"` (:146; dark before the task).
  Every probe therefore verified on-disk content at probe time. The idle-strace acceptance
  was done by **sudo strace attach** (ptrace_scope=1) rather than launch-under-strace,
  because portal-session.sh launches the backend itself.
- **Probe artifacts lost:** the round-1 busctl ReadOne u1/u2 / SettingChanged-monitor /
  unknown-key probes were asserted in review but no log survived — the gate flagged this
  as secondary missing evidence. Preserve probe output under evidence/ or the claim is
  gate-invisible.

**Gate verdict NOT_READY (seat gate red, unattributed):** both seat smokes ran on the
final binaries and failed — drag-smoke run.log (19:51): line-2 WARN "another session is
running (WAYLAND_DISPLAY='lumen-0'", :30 `VERDICT: FAIL`, 9 of 10 scenario rows FAIL at
+0,+0 deltas; app-smoke run.log (20:05–20:14): `VERDICT: FAIL` with 101 FAIL lines across
groups A–Z, including **Z2 interactive Screenshot `colours [(0,0,0)]`** and **Z4 PickColor
`color 0,0,0`** (run.log:181,183) — the portal backend this diff touches answering
successfully but capturing black. The gate's read also noted a client segfault at
scripts/app-smoke.sh:3284 and mixed output geometry (E1 usable 1600x900 vs Z5 format
3200x1800). **No baseline smoke exists for this task anywhere** (worktree//tmp/main
searched by the gate), while [[lumen-notebook-full-green-audit]] proved 177/0/0 green on
this same host the day before — per §1.2 an unattributed red cannot be accepted, and
"environmental" is never convertible to success by assertion
([[lumen-app-smoke-known-noise]]). Remediation owed (tracked at .zcode/TODO.md): re-run
both smokes on a quiet serialized host and record green, or produce the baseline A/B
attributing every FAIL row to a named mechanism, plus preserve the busctl/strace probe
artifacts.

**Review noise (cross-run false-positive tracking):** raised **5**, confirmed **4** (doc
rows, zeditor, test scratch leak, reply leak — all fixed by the commits above), refuted
**1**, preexisting **0**.

**Disproven (do not re-chase):** "xdg-desktop-portal-hyprland's Settings backend is the
reference" — it implements **no** Settings backend (shallow clone, tags v1.2.2/v1.3.3 +
main: GlobalShortcuts and Screencopy only); the error name was grounded in
xdg-desktop-portal-gtk's settings.c/utils.c instead. "No zed binary on this host" —
zeditor. The plan objective's part B (compositor LUMEN_THEME export, lumen-settings
startup appearance) is a different card's scope and was not touched.

**Why:** a D-Bus settings surface is only as good as its watch discipline — serve the
interface doc's exact values, flip-detect against the previously served value (never the
file's), and append (never shift) poll fds when a loop hard-indexes. The gate failure is
the other half of the lesson: a feature whose own consumer rows (Z2/Z4) are red with no
baseline is undecidable, and a skipped acceptance based on a wrong binary name hides a
working feature.

**How to apply:** to re-verify the live path: `scripts/run-desktop.sh` +
`scripts/portal-session.sh`, then `busctl --user call org.freedesktop.portal.Desktop
/org/freedesktop/portal/desktop org.freedesktop.portal.Settings ReadOne ss
org.freedesktop.appearance color-scheme` (u 1 dark / u 2 light), and flip
`[ui] appearance` by write-and-rename — expect `portal.settings: color-scheme X -> Y` in
portal-lumen.log; save every probe's output under evidence/. Zed theme=System on this
host follows those flips (launch `zeditor`, not `zed`). Before the seat gate can pass,
close the TODO row: quiet-host re-run of drag-smoke + app-smoke, or a baseline A/B that
names each FAIL row's mechanism. When extending the portal poll loop again, append fds at
the end and capture the new index where armed — fds[0..3] are load-bearing.

## 2026-11-17 seat redo — TODO remediation closed on merged main (branch merged as ebe7046; tester-run, analyst-recounted)

- **Quiet-host serialized re-run green** (`evidence/2026-11-17-seat-redo/`, notebook):
  drag-smoke 10/10 PASS; app-smoke **177 PASS / 0 WARN / 0 FAIL** with **Z2 interactive
  Screenshot `colours [(255,0,0)]`** and **Z4 PickColor `255,0,0`** as real-pixel PASSes —
  the Nov-16 "backend answering but capturing black" reading is **not supported**; the
  101-FAIL signature matches the concurrent-session WARN family
  ([[lumen-app-smoke-known-noise]]). ctest 136/136 at merged main 9022e74.
- **Every asserted probe artifact now preserved** (the round-1 loss is repaired):
  busctl ReadOne `v u 2` light / `v u 1` dark; SettingChanged dbus-monitor capture +
  journal `portal.settings: color-scheme 2 -> 1`; unknown key/namespace → exact
  `org.freedesktop.portal.Error.NotFound` / "Requested setting not found"; ReadAll glob
  (match / no-match → `a{sa{sv}} 0` / empty = all); **idle-strace**: backend parked in
  `poll` with timeout −1, zero syscalls and zero CPU over two 30 s windows
  (`c-portal/`, `d-supplements/strace-*`).
- Bonus path proof: an accidental injected dropdown commit changed `density` and wrote the
  config end-to-end — the app's value-control write path works on the seat (immediately
  restored). New anomaly recorded to TODO: a transient "no writable config.toml" banner on
  fresh lumen-settings start that vanished after the first write (unexplained; see TODO).
- The row's "no BTN codes" node claim was a bitmap misreading —
  see [[lumen-uinput-key-bitmap-format]].

Related: [[lumen-vulkan-capture-green]] (the Screenshot/PickColor backend this builds
beside; the Z-row family and the portal symlink trap), [[lumen-smoke-bench-harness-green]]
(portal-session LUMEN_BUILD_DIR rule), [[lumen-notebook-full-green-audit]] (the 177/0/0
green the seat reds contradict), [[lumen-app-smoke-known-noise]] (baseline-when-failing
rule), [[lumen-notebook-hw]] (this host), [[lumen-zcode-agent-harness]] (worktree/gate
discipline).
