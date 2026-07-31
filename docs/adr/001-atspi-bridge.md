> **Status:** Current (v0.3) — see code for implementation truth

# ADR-001: AT-SPI bridge — native sd-bus vs AccessKit

**Status:** accepted  
**Date:** 2026-07-30

## Context

Lumen needs an accessibility tree exposed to Orca and other AT-SPI2 clients. AccessKit
provides a mature cross-platform model but requires a Rust toolchain and `accesskit-c`
in the build.

## Decision

Implement **native AT-SPI2 over sd-bus** in `lx.a11y` for the first release. The widget
tree publishes `lx::a11y::node` records; `atspi_bridge` maps them to D-Bus (`org.a11y.atspi`).

AccessKit remains an optional future backend behind `LUMEN_A11Y_ACCESSKIT` if we add Rust
to the build later.

## Consequences

- Pure C++/Clang toolchain preserved.
- More initial work than AccessKit C bindings.
- Full control over AT-SPI object lifecycle and compositor magnifier hooks.
