---
name: lumen-agent-screenshoter
description: "2026-11-17 notebook: harness gained agent-screenshoter (role + skill) for seat screenshots — grim/slurp over Lumen's wlr-screencopy v3 (wire_install.cpp:779-784) or the xdg-desktop-portal Screenshot wire via lumen-portal-probe; notebook inventory verified, VM extras unverified; xdg-desktop-portal-wlr is ScreenCast-only and never installed."
metadata:
  node_type: memory
  type: project
---

Verified 2026-11-17 on the iritur notebook (nouveau+i915 per `ls /sys/class/drm/card*/device/driver`):
harness task `screenshoter-agent-skill` (branch `zcode/task/screenshoter-agent-skill`, cut from main
1355959) added the dispatchable screenshot role — docs-only, zero C++.

- New: `.zcode/agents/agent-screenshoter.md` (tools Read/Bash/Grep/Glob, Edit/Write disallowed —
  shots are command artifacts) and `.zcode/skills/agent-screenshoter/SKILL.md`; dispatch wired into
  `.zcode/skills/lumen-studio/SKILL.md` (Map + one sentence) and the AGENTS.md harness bullet.
- Notebook inventory re-run this session (`command -v` per tool): present grim, slurp, import,
  convert, magick, wl-copy, wl-paste, busctl, gdbus, wlr-randr, wf-recorder; absent wayshot, scrot,
  maim, spectacle, flameshot, xwd, wayland-info. VM extras (magick/wlr-randr/wf-recorder there)
  stay unverified — the skill carries the re-verify one-liner instead of asserting them.
- Provenance re-verified in the worktree: `zwlr_screencopy_manager_v1` v3 at
  `modules/lx.compositor/wire_install.cpp:779-784`, cursor painting `wire_capture.cpp:1022`, portal
  wire (frontend `org.freedesktop.portal.Desktop` → `Screenshot(s, a{sv})` with
  handle_token/interactive/target, result on `org.freedesktop.portal.Request.Response`'s `uri` key)
  at `tests/clients/lumen-portal-probe.cpp:60-62,166-200,253-307`; target semantics
  `modules/lx.portal/service.cpp:453,511`; naming `lx.portal/files.cpp:100-113`.
- xdg-desktop-portal-wlr clarification (owner's question): portal-session.sh does start it
  (:144-155) but only as the **ScreenCast** backend (ADR-009 D25); Screenshot is
  xdg-desktop-portal-lumen's (D26; lumen-portals.conf: Screenshot lumen, ScreenCast wlr, D28).
  "Lumen does not use or need the wlr one" is exactly true for screenshots. Never install it.
- No ctest run: diff ⊆ `.zcode/**` + AGENTS.md ⇒ no compiled input changed ⇒ the tests-enabled
  baseline cannot be affected (no rebuild made or claimed).

**Why:** screenshot evidence was being improvised per task (wrong tool guesses on the two
same-named hosts, portal wire re-derived each time); a role with a verified-facts skill makes the
next session start at the commands instead of the discovery.

**How to apply:** dispatch `agent-screenshoter` when pixels from the live seat are needed; read the
skill's inventory one-liner output before trusting any tool on either host; keep the xdpw
screenshot clarification in PR text when this harness change is described. Extend the skill (not a
new memory) when a host inventory changes or a compositor split is newly verified.
