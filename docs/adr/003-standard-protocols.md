> **Status:** Current (v0.3) — see code for implementation truth

# ADR 003 — Standard Wayland Protocols as Public API

**Status:** Accepted  
**Date:** 2026-07-30

## Context

`zlm_shell_v1` duplicated layer-shell, decoration, workspace, and output-management interfaces.
Third-party clients and toolkits expect standard protocols.

## Decision

- Register **standard globals** from `protocol_managers` (wlr-layer-shell, xdg-decoration,
  ext-workspace, wlr-output-management, session-lock, text-input, color-management, …).
- Shrink `zlm_shell_v1` to a **privileged control channel** for `lumen-shell` only (policy bridge,
  workspace writes, window rules, compositor-owned toplevel handles).
- P1 promotion for `ext-foreign-toplevel-list-v1`.

## Consequences

- Compositor implements stub→real handlers per protocol in `lx.compositor:protocol_managers`.
- *Update (ADR 007):* standard globals are registered by the protocol wire units
  (`wire_install.cpp`, one `wire_*.cpp` per protocol area: xdg-decoration, wlr-layer-shell,
  xdg-output, cursor-shape, xdg-activation, viewporter, fractional-scale, presentation-time,
  idle-notify, idle-inhibit, output-power, session-lock, text-input, input-method,
  virtual-keyboard, …), each served in
  full; `protocol_managers` keeps only unadvertised placeholder state for protocols not yet
  implemented, and loses each entry when its protocol lands.
- Docs and manifest list standards as the public surface; private XML is gated by session UID.
