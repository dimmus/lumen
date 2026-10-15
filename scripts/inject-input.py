#!/usr/bin/env python3
"""Inject pointer and keyboard input into existing evdev nodes, for unattended tests.

Writes `struct input_event` records straight into /dev/input/eventN. The kernel feeds a
write to an evdev node through the same path as the device's own events, so libinput — and
therefore the compositor — cannot tell them apart. This needs no uinput module (some
kernels ship without it), but it does need root, and it needs a device that already has
the capabilities being injected: an absolute pointer for `move`/`drag`, a keyboard for
`key`.

Positions are given in OUTPUT PIXELS. The compositor scales an absolute device's
normalised position into the output (libinput: (value - min) / (max - min + 1)), so the
injector needs the output size — from --output WxH, or read from the compositor's
`output 0 mode=WxH` log line with --compositor-log — and the device's own axis range,
which it reads with EVIOCGABS. Each position is aimed at the centre of the pixel, so
rounding on either side cannot land it on a neighbour.

Every write is recorded as one JSON line on stdout (nothing else goes there), with the
CLOCK_MONOTONIC time read just before the write. The kernel stamps the event when the
SYN_REPORT is processed, so the recorded time is never later than the timestamp libinput
reports. That is what lets a test measure input-to-present latency against the
compositor's debug log, which uses the same clock.

    sudo python3 -I scripts/inject-input.py --compositor-log comp.log move 400 300
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 700 520 --button left
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 700 520 --button left --super
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 40 20 1800 20      # hover only
    sudo python3 -I scripts/inject-input.py button right click
    sudo python3 -I scripts/inject-input.py key KEY_ESC tap
    sudo python3 -I scripts/inject-input.py watch > seen.jsonl     # until SIGTERM

`watch` records every input frame the kernel delivers on every /dev/input/event* node,
injected or not, as one JSON line each (kind "seen"; kernel timestamps on
CLOCK_MONOTONIC). A writer's own events reach every reader of the node, so comparing this
with the injector's records — which carry the raw device values written — shows exactly
which input came from somewhere else: a hand on the mouse, or a VM console forwarding the
host pointer through the very device the injector writes to.

Dependency-free python3. Run with -I so nothing from the working directory is imported
into a root process.
"""

import argparse
import fcntl
import glob
import json
import os
import re
import select
import signal
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
ABS_X, ABS_Y = 0x00, 0x01

BUTTONS = {"left": 0x110, "right": 0x111, "middle": 0x112}
KEYS = {"KEY_ESC": 1, "KEY_LEFTMETA": 125, "KEY_LEFTSHIFT": 42, "KEY_LEFTCTRL": 29,
        "KEY_LEFTALT": 56, "KEY_ENTER": 28, "KEY_SPACE": 57, "KEY_A": 30}

# struct input_event on 64-bit Linux: struct timeval (two longs), __u16 type, __u16 code,
# __s32 value. The kernel ignores the time of a written event and stamps its own.
EVENT = struct.Struct("llHHi")
# struct input_absinfo: value, minimum, maximum, fuzz, flat, resolution.
ABSINFO = struct.Struct("iiiiii")


def eviocgabs(axis):
    # _IOR('E', 0x40 + axis, struct input_absinfo)
    return (2 << 30) | (ABSINFO.size << 16) | (ord("E") << 8) | (0x40 + axis)


# _IOW('E', 0xa0, int): which clock stamps events read from this descriptor.
EVIOCSCLOCKID = (1 << 30) | (4 << 16) | (ord("E") << 8) | 0xA0
CLOCK_MONOTONIC = 1
SYN_DROPPED = 3


def now_us():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000


def pack(etype, code, value):
    return EVENT.pack(0, 0, etype, code, value)


class Injector:
    def __init__(self, pointer, keyboard, output, tag):
        self.pointer_path = pointer
        self.keyboard_path = keyboard
        self.output = output
        self.tag = tag
        self.ptr = None
        self.kbd = None
        self.range = None

    # ── devices ─────────────────────────────────────────────────────────────

    def pointer(self):
        if self.ptr is None:
            self.ptr = os.open(self.pointer_path, os.O_WRONLY | os.O_CLOEXEC)
        return self.ptr

    def keyboard(self):
        if self.kbd is None:
            self.kbd = os.open(self.keyboard_path, os.O_WRONLY | os.O_CLOEXEC)
        return self.kbd

    def axis_ranges(self):
        if self.range is None:
            # EVIOCGABS needs a readable descriptor; open one just for the query.
            fd = os.open(self.pointer_path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            try:
                ranges = []
                for axis in (ABS_X, ABS_Y):
                    buf = bytearray(ABSINFO.size)
                    try:
                        fcntl.ioctl(fd, eviocgabs(axis), buf)
                    except OSError as e:
                        sys.exit(f"inject-input: {self.pointer_path} has no absolute axis "
                                 f"{axis} ({e}); pass --pointer with an absolute device")
                    _, lo, hi, _, _, _ = ABSINFO.unpack(buf)
                    if hi <= lo:
                        sys.exit(f"inject-input: {self.pointer_path} axis {axis} has an "
                                 f"empty range {lo}..{hi}")
                    ranges.append((lo, hi))
                self.range = ranges
            finally:
                os.close(fd)
        return self.range

    def to_device(self, px, extent, axis):
        lo, hi = self.axis_ranges()[axis]
        span = hi - lo + 1
        px = min(max(px, 0.0), extent - 1)
        # Aim at the pixel centre: libinput's (v - lo) / span * extent then floors to px.
        return min(hi, lo + int(round((px + 0.5) * span / extent)))

    # ── writes ──────────────────────────────────────────────────────────────

    def record(self, kind, t_us, **fields):
        fields.update(kind=kind, t_us=t_us)
        if self.tag:
            fields["tag"] = self.tag
        sys.stdout.write(json.dumps(fields, separators=(",", ":")) + "\n")
        sys.stdout.flush()

    def move(self, x, y, phase="move"):
        if self.output is None:
            sys.exit("inject-input: the output size is unknown; pass --output WxH or "
                     "--compositor-log")
        w, h = self.output
        ax, ay = self.to_device(x, w, 0), self.to_device(y, h, 1)
        data = (pack(EV_ABS, ABS_X, ax) + pack(EV_ABS, ABS_Y, ay) +
                pack(EV_SYN, SYN_REPORT, 0))
        t = now_us()
        os.write(self.pointer(), data)
        # The raw values written, so a watcher can tell these frames from foreign ones.
        self.record("motion", t, x=round(x, 2), y=round(y, 2), phase=phase, ax=ax, ay=ay)

    def button(self, name, pressed):
        t = now_us()
        os.write(self.pointer(), pack(EV_KEY, BUTTONS[name], 1 if pressed else 0) +
                 pack(EV_SYN, SYN_REPORT, 0))
        self.record("button", t, button=name, pressed=pressed)

    def key(self, code, pressed):
        t = now_us()
        os.write(self.keyboard(), pack(EV_KEY, code, 1 if pressed else 0) +
                 pack(EV_SYN, SYN_REPORT, 0))
        self.record("key", t, code=code, pressed=pressed)

    def close(self):
        for fd in (self.ptr, self.kbd):
            if fd is not None:
                os.close(fd)


def sleep_until(deadline_ns):
    left = deadline_ns - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    if left > 0:
        time.sleep(left / 1e9)


def parse_size(text):
    m = re.fullmatch(r"(\d+)x(\d+)", text)
    if not m or int(m.group(1)) <= 0 or int(m.group(2)) <= 0:
        raise argparse.ArgumentTypeError(f"expected WxH, got {text!r}")
    return int(m.group(1)), int(m.group(2))


def output_from_log(path):
    pattern = re.compile(r"output 0 mode=(\d+)x(\d+)")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = pattern.search(line)
                if m:
                    return int(m.group(1)), int(m.group(2))
    except OSError as e:
        sys.exit(f"inject-input: cannot read {path}: {e}")
    sys.exit(f"inject-input: no 'output 0 mode=WxH' line in {path} yet")


def key_code(text):
    if text.isdigit():
        return int(text)
    if text.upper() in KEYS:
        return KEYS[text.upper()]
    raise argparse.ArgumentTypeError(f"unknown key {text!r}; use a number or one of "
                                     f"{', '.join(sorted(KEYS))}")


def run_drag(inj, a):
    """Press (optionally under Super), move in even steps `interval` apart, release.

    With --button none it is a hover sweep. --esc-at N taps Escape after step N.
    """
    interval_ns = int(a.interval_ms * 1e6)
    steps = a.steps
    if steps <= 0:
        dist = max(abs(a.x1 - a.x0), abs(a.y1 - a.y0))
        steps = max(1, int(round(dist / a.step_px)))
    inj.move(a.x0, a.y0, "pre")
    time.sleep(0.05)
    super_down = button_down = False
    try:
        if a.super:
            inj.key(KEYS["KEY_LEFTMETA"], True)
            super_down = True
            time.sleep(0.03)
        if a.button != "none":
            inj.button(a.button, True)
            button_down = True
            time.sleep(0.05)
        start = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        for i in range(1, steps + 1):
            sleep_until(start + i * interval_ns)
            inj.move(a.x0 + (a.x1 - a.x0) * i / steps, a.y0 + (a.y1 - a.y0) * i / steps,
                     "drag" if a.button != "none" else "hover")
            if i == a.esc_at:
                inj.key(KEYS["KEY_ESC"], True)
                inj.key(KEYS["KEY_ESC"], False)
        # Hold still briefly before release, as a hand does: the last position is shown
        # before the grab ends, so the final geometry is the drag's end and not a racing one.
        time.sleep(a.hold_ms / 1000.0)
    finally:
        # Released even when interrupted: a button or Super left down on a real device
        # stays down for the whole session, and every later click becomes a grab.
        if button_down:
            inj.button(a.button, False)
            time.sleep(0.03)
        if super_down:
            inj.key(KEYS["KEY_LEFTMETA"], False)


def run_watch():
    """Record every frame on every event node until SIGTERM/SIGINT."""
    fds = {}
    for path in sorted(glob.glob("/dev/input/event*")):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError:
            continue
        try:
            fcntl.ioctl(fd, EVIOCSCLOCKID, struct.pack("i", CLOCK_MONOTONIC))
        except OSError:
            pass  # stamps stay on the realtime clock; recorded order is still valid
        fds[fd] = (path, [])
    if not fds:
        sys.exit("inject-input: watch: no readable /dev/input/event* nodes")
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
    signal.signal(signal.SIGINT, lambda *_: stop.append(1))
    out = sys.stdout
    out.write(json.dumps({"kind": "watch", "devices": [v[0] for v in fds.values()],
                          "t_us": now_us()}) + "\n")
    out.flush()
    while not stop:
        try:
            ready, _, _ = select.select(list(fds), [], [], 0.2)
        except InterruptedError:
            continue
        for fd in ready:
            path, frame = fds[fd]
            try:
                data = os.read(fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            except OSError:
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                sec, usec, etype, code, value = EVENT.unpack_from(data, off)
                if etype != EV_SYN:
                    frame.append([etype, code, value])
                    continue
                if code == SYN_DROPPED:
                    out.write(json.dumps({"kind": "dropped", "dev": path,
                                          "t_us": sec * 1_000_000 + usec}) + "\n")
                    frame.clear()
                elif code == SYN_REPORT and frame:
                    out.write(json.dumps({"kind": "seen", "dev": path,
                                          "t_us": sec * 1_000_000 + usec, "ev": frame},
                                         separators=(",", ":")) + "\n")
                    fds[fd] = (path, [])
                    frame = fds[fd][1]
        out.flush()
    for fd in fds:
        os.close(fd)


def main():
    p = argparse.ArgumentParser(
        description="Inject pointer/keyboard input into evdev nodes (root only).",
        epilog="Positions are output pixels. Writes one JSON record per event to stdout.")
    p.add_argument("--pointer", default="/dev/input/event2",
                   help="absolute pointer node (default: %(default)s, the VMware mouse)")
    p.add_argument("--keyboard", default="/dev/input/event1",
                   help="keyboard node (default: %(default)s)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--output", type=parse_size, help="output size WxH in pixels")
    g.add_argument("--compositor-log", help="read the size from 'output 0 mode=WxH' in this log")
    p.add_argument("--tag", default="", help="label copied into every record")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("move", help="absolute pointer move")
    s.add_argument("x", type=float)
    s.add_argument("y", type=float)

    s = sub.add_parser("drag", help="press, move in steps, release (or hover with --button none)")
    for name in ("x0", "y0", "x1", "y1"):
        s.add_argument(name, type=float)
    s.add_argument("--button", choices=sorted(BUTTONS) + ["none"], default="left")
    s.add_argument("--super", action="store_true", help="hold KEY_LEFTMETA around the drag")
    s.add_argument("--steps", type=int, default=0,
                   help="motion events (default: one per --step-px of distance)")
    s.add_argument("--step-px", type=float, default=4.0)
    s.add_argument("--interval-ms", type=float, default=10.0, help="time between motions")
    s.add_argument("--hold-ms", type=float, default=120.0, help="pause before release")
    s.add_argument("--esc-at", type=int, default=-1, help="tap Escape after this step")

    s = sub.add_parser("button", help="pointer button")
    s.add_argument("name", choices=sorted(BUTTONS))
    s.add_argument("action", choices=("press", "release", "click"))

    s = sub.add_parser("key", help="keyboard key (evdev code or KEY_* name)")
    s.add_argument("code", type=key_code)
    s.add_argument("action", choices=("press", "release", "tap"))

    sub.add_parser("watch", help="record every frame on every event node until SIGTERM")

    a = p.parse_args()
    # SIGTERM runs the finally blocks too, so a killed drag still releases what it held.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if os.geteuid() != 0:
        sys.exit("inject-input: needs root to write to evdev nodes (run under sudo)")

    if a.cmd == "watch":
        run_watch()
        return

    output = a.output
    if output is None and a.compositor_log:
        output = output_from_log(a.compositor_log)
    inj = Injector(a.pointer, a.keyboard, output, a.tag)
    try:
        if a.cmd == "move":
            inj.move(a.x, a.y)
        elif a.cmd == "drag":
            run_drag(inj, a)
        elif a.cmd == "button":
            if a.action in ("press", "click"):
                inj.button(a.name, True)
            if a.action == "click":
                time.sleep(0.05)
            if a.action in ("release", "click"):
                inj.button(a.name, False)
        elif a.cmd == "key":
            if a.action in ("press", "tap"):
                inj.key(a.code, True)
            if a.action == "tap":
                time.sleep(0.03)
            if a.action in ("release", "tap"):
                inj.key(a.code, False)
    except OSError as e:
        sys.exit(f"inject-input: {e}")
    finally:
        inj.close()


if __name__ == "__main__":
    main()
