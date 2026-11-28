# ADR 015 — Lumen display manager

**Status:** Accepted — real-seat and boot verification pending.
**Date:** 2026-11-26
**Scope:** `executables/lumen-dm/`, system display-manager deployment, and the login handoff.
**Deciders:** project owner
**Related:** [ADR 011](011-session-and-window-management.md), [ADR 010](010-gpu-ui-engine.md)

## Context

`lumen-session` starts the desktop after authentication. It already works from a console
login or as an external display manager's Wayland session. Lumen needs its own graphical
login at boot, without duplicating PAM or making the desktop compositor accept a greeter
session or a new privileged client.

## Decision

- `lumen-dm.service` is a system unit enabled through `display-manager.service`, not a
  user unit. It runs the packaged greetd daemon on seat0/VT1 and leaves other gettys
  alone. Installing it neither enables it nor modifies another display manager.
- greetd and the distro PAM stack own authentication, account policy, credentials and
  logind sessions. The greeter connects only to greetd under its dedicated, non-login
  UID. The service's `UMask=0077` restricts greetd's socket when it is bound, before
  greetd assigns it to that UID. The authenticated session's file mask remains a PAM
  policy and is part of the deployment check.
- `lumen-dm` is the only display process before login. It opens its own local
  `greeter` logind session on `seat0` with non-forced control, takes input and DRM
  devices through logind only, and renders one form to an atomic KMS output. The
  ordinary desktop still rejects greeter sessions. The form reuses `lx.ui` and
  the existing CPU primitive renderer, with bounded cached pixels and two KMS
  buffers on the active output. No Wayland display, desktop shell, clipboard,
  IME service, or GPU context runs before login. Prompt secrets never enter UI
  text; only the masked representation does.
- Only a successful PAM conversation may schedule a session. The only command sent is
  the installed `lumen-session`, shell-quoted because greetd launches it via `/bin/sh`.
  `XDG_SESSION_TYPE=wayland` is supplied before PAM opens that session. The greeter
  stops input and presents, clears its conversation, and releases DRM and logind
  control before exiting. greetd starts the user session only after the greeter
  exits and reopens the greeter when the user session ends. No greeter environment
  or device descriptors cross the handoff. The existing launcher still owns the
  user's login-shell setup and user-manager environment.

### Experimental split-VT handoff

The separately packaged `lumen-dm-split-vt.service` uses an opt-in greetd build.
The prototype reserves VT1 for the greeter, VT7 for the authenticated desktop,
and leaves VT2 available for recovery. The broker keeps the greeter visible while
PAM opens the user session and Lumen prepares its inactive KMS path. It asks
logind to activate the desktop after verifying `KD_GRAPHICS` and readiness on a
private Unix socket. The greeter exits after the broker receives a fresh response
from a physical content flip with working input.

On orderly logout the compositor keeps its display until the broker confirms
that the replacement greeter owns the foreground graphics VT. Startup and return
have bounded failure paths. The broker checks the foreground before each switch
and abandons recovery after observing a third VT. Logind offers no atomic
foreground check and switch, so concurrent manual VT changes remain a race to
test. The authenticated desktop and the greeter retain separate UIDs and logind
controllers, and no DRM or input descriptors cross that boundary.

Keep the installed boot workaround until real-seat login and logout tests pass
with `fbcon=vc:2-63` omitted. Display a recovery login on VT2 and check the
physical screen alongside KMS and VT traces. The normal `lumen-dm.service`
remains available for rollback.

## Consequences

The desktop stays Vulkan-first. The login screen is a single local UI with no
client composition; it uses the existing CPU renderer into cached form-sized
memory and writes the changed pixels to KMS without reading the scanout mapping.
Two buffers avoid drawing into the active scanout, and idle does not schedule
frames. There is no fallback to another compositor. The earlier Weston-backed
candidate is retained as the performance and functional reference; savings must
be measured with matching display settings on the same host.

Acceptance requires native boot, failed-login recovery, repeated login/logout without
stale processes or sockets, VT recovery, and bounded crash recovery on the real seat.
Compare time to interactive greeter and idle greeter-stack PSS/wakeups on the same host;
compare login-to-first-frame against the existing console startup. `READY=1` without
visible scanout and working input is not a successful handoff.
