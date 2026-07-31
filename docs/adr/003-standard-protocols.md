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
- Docs and manifest list standards as the public surface; private XML is gated by session UID.
