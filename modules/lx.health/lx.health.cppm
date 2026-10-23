module;

// lx.health — the session's health report (docs/subsystems/health.md). Runtime layer, pure:
// the compositor fills `health_inputs` from state it already keeps, at the moment someone
// asks (lumen-ctl status, the control socket's GetHealth), and `evaluate` turns them into a
// `health_report`. Nothing is sampled per frame and nothing runs on a timer: an idle
// desktop pays nothing for being observable.

export module lx.health;

export import :types;
export import :monitor;
