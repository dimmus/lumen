---
name: lumen-agent-screenshoter
description: "2026-11-17 notebook: harness gained agent-screenshoter (role + skill, branch zcode/task/screenshoter-agent-skill 9f5bea4+ce58c45+ac0330d, gate READY, unmerged) for seat screenshots — grim/slurp over Lumen's wlr-screencopy v3 (wire_install.cpp:778-784) or the xdg-desktop-portal Screenshot wire via lumen-portal-probe; notebook inventory verified, VM extras unverified; grim capture flips the panel bell into DND for 2 s (k_indicator_ns, wire_capture.cpp:97); xdg-desktop-portal-wlr is ScreenCast-only and never installed."
metadata:
  node_type: memory
  type: project
summary: "Screenshot role + skill: grim over wlr-screencopy v3 or the portal path via lumen-portal-probe; xdg-desktop-portal-wlr never installed; grim flips the panel bell to DND for 2 s"
modules: harness
topics: screenshots, seat

---

Verified 2026-11-17 on the iritur notebook (nouveau+i915 per `ls /sys/class/drm/card*/device/driver`):
harness task `screenshoter-agent-skill` (branch `zcode/task/screenshoter-agent-skill`, cut from main
1355959, 4 commits, unmerged — nothing pushed, `git branch -r --contains ac0330d` empty) added the
dispatchable screenshot role — docs-only, zero C++, no ctest run or claimed.

- Commits 9f5bea4 (role + skill + wiring), 5871231 (branch's shorter copy of this record),
  ce58c45 + ac0330d (review fixes, below). Diff = 6 files, all `.zcode/**` + AGENTS.md ⇒ no
  compiled input changed ⇒ the tests-enabled ctest baseline cannot be affected.
- New: `.zcode/agents/agent-screenshoter.md` (frontmatter per the lumen-architect convention;
  name `agent-screenshoter`; tools Read/Bash/Grep/Glob, Edit/Write disallowed — shots are
  command artifacts, maxTurns 20) and `.zcode/skills/agent-screenshoter/SKILL.md` whose every
  fact carries provenance. Dispatch wired minimally: lumen-studio Map rows + one direct-dispatch
  sentence (+4/−2), AGENTS.md harness bullet stays one bullet (+5/−3).
- Notebook inventory re-run (`command -v` per tool): present grim, slurp, import, convert,
  magick, wl-copy, wl-paste, busctl, gdbus, wlr-randr, wf-recorder; absent wayshot, scrot,
  maim, spectacle, flameshot, xwd, wayland-info. VM extras (magick/wlr-randr/wf-recorder
  there) stay unverified — the skill's re-verify one-liner is the discharge path, never assert.
- Provenance re-verified this session in the worktree: `zwlr_screencopy_manager_v1` v3 at
  `modules/lx.compositor/wire_install.cpp:778-784` (grep), cursor painting
  `wire_capture.cpp:1022`, portal wire (frontend `org.freedesktop.portal.Desktop` @
  `/org/freedesktop/portal/desktop` → `Screenshot(s parent_window, a{sv} options)` with
  handle_token/interactive/target; result on `org.freedesktop.portal.Request.Response`'s
  `uri` key) at `tests/clients/lumen-portal-probe.cpp:60-62,166-200,253-307`; target
  semantics `modules/lx.portal/service.cpp:453,511` (0/1 → private `/run/user/<uid>/lumen-portal`
  file; 2 window, 4 area, interactive → `$XDG_PICTURES_DIR/Screenshots` per
  `lx.portal/files.cpp:100-113`); locked session → response 2 (ADR-009 D30); ready-made
  client `build/test/lumen-portal-probe`. grim -h/slurp -h flags verified live.
- xdg-desktop-portal-wlr clarification (owner's question): portal-session.sh does start it
  (:144-155) but only as the **ScreenCast** backend (ADR-009 D25); Screenshot is
  xdg-desktop-portal-lumen's (D26; lumen-portals.conf: Screenshot lumen, ScreenCast wlr, D28).
  "Lumen does not use or need the wlr one" is exactly true for screenshots. Never install it
  or any package.
- **grim is not side-effect-free on Lumen:** a wlr-screencopy frame counts as active capture
  for 2 s (`k_indicator_ns = 2'000'000'000ull`, `wire_capture.cpp:97` — read this session) →
  `zlm_shell_v1.capture_state` → shell DND while capturing (`shell_desktop.cpp:318-319`,
  `notifications.cppm:132,224`) → the panel bell swaps layout (~3.4 k px at the bell bbox,
  notebook x2775-3121 y17-46, probe-induced — attributed to [[lumen-animations-close-ghost]]:
  64-66,90, not re-measured). Space settled shots >2 s apart; exclude the bell bbox from
  cross-shot pixel diffs.

**Close-out 2026-11-17 (gate READY, written on main from the packet + gate/review evidence):**

- **Review noise (cross-run false-positive tracking): raised 2 / confirmed 2 / refuted 0 /
  preexisting 0.** Both findings low, both fixed on the branch, gate re-verified at tip
  ac0330d. (1) The re-verify one-liner omitted wf-recorder, so it could not discharge the
  skill's own unverified-VM-extras claim for that tool — fixed ce58c45 (+1/−1); reviewer
  re-ran the updated line live (`/usr/sbin/wf-recorder` present). (2) The skill omitted the
  recorded grim→bell-DND side effect (above) — fixed ac0330d (+14, §4 side-effect block +
  §7 judging rule); every new citation read in source by review and re-checked here.
- Gate verdict READY: diff scope 6 files verified via `git diff --name-only main...HEAD`;
  acceptances 1–5 verified (frontmatter shape, skill provenance incl. live one-liner re-run,
  minimal wiring, docs-only reasoning stated, memory record + index, nothing pushed, scripts/
  untouched). Packet inaccuracy noted by gate: implementation.commits listed 2 of the branch's
  4 commits (the fix commits live in the review section). No seat smokes / bench (off — not
  applicable to a harness-docs change). review.preexistingFindings = [] ⇒ no TODO rows opened
  by this close-out.
- Branch commit 5871231 carries this record's shorter pre-review copy; at merge this file is
  add/add — keep main's (extended) copy, same as [[lumen-settings-key-capture-seat]].

**Why:** screenshot evidence was being improvised per task (wrong tool guesses across the two
same-named hosts, portal wire re-derived each time), and a skill's self-verification loop must
cover every tool it defers to — the one-liner that omits a tool leaves that tool's claim
forever unverified. The capture tool itself perturbs the session under test (DND flip), so
evidence pipelines must record tool side effects, not just commands.

**How to apply:** dispatch `agent-screenshoter` when pixels from the live seat are needed;
run the skill's inventory one-liner before trusting any tool on either host; keep panel shots
>2 s apart and the bell bbox out of pixel diffs; extend the skill (not this memory) when a
host inventory changes or a compositor split is newly verified; when merging the branch keep
main's copy of this file; never install xdg-desktop-portal-wlr or any package. Related:
[[lumen-zcode-agent-harness]], [[lumen-animations-close-ghost]], [[lumen-notebook-hw]].
