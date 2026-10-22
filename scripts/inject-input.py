#!/usr/bin/env python3
"""Inject pointer and keyboard input into existing evdev nodes, for unattended tests.

Writes `struct input_event` records straight into /dev/input/eventN. The kernel feeds a
write to an evdev node through the same path as the device's own events, so libinput — and
therefore the compositor — cannot tell them apart. This needs no uinput module (some
kernels ship without it), but it does need root, and it needs a device that already has
the capabilities being injected: an absolute pointer for `move`/`drag`, a keyboard for
`key`, a relative pointer (REL_X/REL_Y; the VMware mouse's second node) for `rel`.

Positions are given in OUTPUT PIXELS. The compositor scales an absolute device's
normalised position into the output (libinput: (value - min) / (max - min + 1)), so the
injector needs the output size — from --output WxH, or read from the compositor's
`output 0 mode=WxH` log line with --compositor-log — and the device's own axis range,
which it reads with EVIOCGABS. Each position is aimed at the centre of the pixel, so
rounding on either side cannot land it on a neighbour.

With several outputs the compositor maps an absolute device onto the bounding box of the
layout (ADR 008 M9). `--layout LOG` reads every `output N name=… logical=WxH+X+Y` line of
the compositor's log, and positions are then LAYOUT (logical) coordinates: the point a
move names is where the pointer lands, on whichever output holds it.

Every write is recorded as one JSON line on stdout (nothing else goes there), with the
CLOCK_MONOTONIC time read just before the write. The kernel stamps the event when the
SYN_REPORT is processed, so the recorded time is never later than the timestamp libinput
reports. That is what lets a test measure input-to-present latency against the
compositor's debug log, which uses the same clock.

    sudo python3 -I scripts/inject-input.py --compositor-log comp.log move 400 300
    sudo python3 -I scripts/inject-input.py --layout comp.log move 2400 300   # on output 1
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 700 520 --button left
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 700 520 --button left --super
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 40 20 1800 20      # hover only
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 900 400 --mods ctrl
    sudo python3 -I scripts/inject-input.py --output 1918x928 drag 500 400 900 400 --mods-at 20:shift
    sudo python3 -I scripts/inject-input.py button right click
    sudo python3 -I scripts/inject-input.py key KEY_ESC tap
    sudo python3 -I scripts/inject-input.py key KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C tap
    sudo python3 -I scripts/inject-input.py type lumen-1
    sudo python3 -I scripts/inject-input.py rel 10 -5 --count 20 --interval-ms 8   # mouse deltas
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

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
SYN_REPORT = 0
ABS_X, ABS_Y = 0x00, 0x01
REL_X, REL_Y = 0x00, 0x01

BUTTONS = {"left": 0x110, "right": 0x111, "middle": 0x112}
KEYS = {"KEY_ESC": 1, "KEY_LEFTMETA": 125, "KEY_LEFTSHIFT": 42, "KEY_RIGHTSHIFT": 54,
        "KEY_LEFTCTRL": 29, "KEY_LEFTALT": 56, "KEY_ENTER": 28, "KEY_SPACE": 57,
        "KEY_F10": 68, "KEY_UP": 103, "KEY_LEFT": 105, "KEY_RIGHT": 106, "KEY_DOWN": 108,
        "KEY_MINUS": 12, "KEY_INSERT": 110, "KEY_BACKSPACE": 14, "KEY_TAB": 15,
        "KEY_RIGHTMETA": 126, "KEY_RIGHTCTRL": 97, "KEY_RIGHTALT": 100,
        "KEY_HOME": 102, "KEY_END": 107, "KEY_PAGEUP": 104, "KEY_PAGEDOWN": 109,
        "KEY_KPENTER": 96}
# F1–F10 are consecutive; F11 and F12 are not (VT switching is Ctrl+Alt+F<n>).
for _i in range(10):
    KEYS[f"KEY_F{_i + 1}"] = 59 + _i
KEYS["KEY_F11"] = 87
KEYS["KEY_F12"] = 88
# Letters and digits by their evdev codes (US layout rows).
for _row, _first in (("qwertyuiop", 16), ("asdfghjkl", 30), ("zxcvbnm", 44)):
    for _i, _ch in enumerate(_row):
        KEYS[f"KEY_{_ch.upper()}"] = _first + _i
for _i, _ch in enumerate("1234567890"):
    KEYS[f"KEY_{_ch}"] = 2 + _i
# `type`: characters reachable without a modifier on a US layout.
TYPEABLE = {ch: KEYS[f"KEY_{ch.upper()}"] for ch in "abcdefghijklmnopqrstuvwxyz0123456789"}
TYPEABLE.update({"-": KEYS["KEY_MINUS"], " ": KEYS["KEY_SPACE"]})

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
    def __init__(self, pointer, keyboard, output, tag, origin=(0, 0), relative=None):
        self.pointer_path = pointer
        self.keyboard_path = keyboard
        self.relative_path = relative
        self.rel_fd = None
        self.output = output
        # The top-left of the area the device spans (the layout's bounding box).
        self.origin = origin
        self.tag = tag
        self.ptr = None
        self.kbd = None
        self.range = None
        # What this run has pressed and not yet released, in press order: a real evdev node
        # (not uinput) releases nothing by itself when the writer dies.
        self.down_keys = []
        self.down_buttons = []

    # ── devices ─────────────────────────────────────────────────────────────

    def pointer(self):
        if self.ptr is None:
            self.ptr = os.open(self.pointer_path, os.O_WRONLY | os.O_CLOEXEC)
        return self.ptr

    def relative(self):
        if self.rel_fd is None:
            self.rel_fd = os.open(self.relative_path, os.O_WRONLY | os.O_CLOEXEC)
        return self.rel_fd

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
        ax = self.to_device(x - self.origin[0], w, 0)
        ay = self.to_device(y - self.origin[1], h, 1)
        data = (pack(EV_ABS, ABS_X, ax) + pack(EV_ABS, ABS_Y, ay) +
                pack(EV_SYN, SYN_REPORT, 0))
        t = now_us()
        os.write(self.pointer(), data)
        # The raw values written, so a watcher can tell these frames from foreign ones.
        self.record("motion", t, x=round(x, 2), y=round(y, 2), phase=phase, ax=ax, ay=ay)

    def rel(self, dx, dy):
        """One relative motion frame (REL_X/REL_Y): what libinput reports as a mouse's motion,
        accelerated by its profile; the unaccelerated delta is these device units."""
        data = b""
        if dx:
            data += pack(EV_REL, REL_X, dx)
        if dy:
            data += pack(EV_REL, REL_Y, dy)
        if not data:
            return
        t = now_us()
        os.write(self.relative(), data + pack(EV_SYN, SYN_REPORT, 0))
        self.record("rel", t, dx=dx, dy=dy)

    def button(self, name, pressed):
        t = now_us()
        os.write(self.pointer(), pack(EV_KEY, BUTTONS[name], 1 if pressed else 0) +
                 pack(EV_SYN, SYN_REPORT, 0))
        self.track(self.down_buttons, name, pressed)
        self.record("button", t, button=name, pressed=pressed)

    def key(self, code, pressed):
        t = now_us()
        os.write(self.keyboard(), pack(EV_KEY, code, 1 if pressed else 0) +
                 pack(EV_SYN, SYN_REPORT, 0))
        self.track(self.down_keys, code, pressed)
        self.record("key", t, code=code, pressed=pressed)

    @staticmethod
    def track(down, what, pressed):
        if pressed and what not in down:
            down.append(what)
        elif not pressed and what in down:
            down.remove(what)

    def release_held(self):
        """Releases every button and key this run left down, buttons first, keys in reverse
        press order. Best effort: one failed write does not keep the rest down."""
        for name in reversed(list(self.down_buttons)):
            try:
                self.button(name, False)
            except OSError:
                pass
        for code in reversed(list(self.down_keys)):
            try:
                self.key(code, False)
            except OSError:
                pass

    def close(self):
        for fd in (self.ptr, self.kbd, self.rel_fd):
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


def layout_from_log(path):
    """The bounding box of every output's logical rect, from the compositor's
    `output N name=… logical=WxH+X+Y` lines (the newest line per output wins; an
    `output N removed` line takes output N out until it is logged again — hotplug)."""
    pattern = re.compile(r"output (\d+) name=\S+ .*logical=(\d+)x(\d+)\+(-?\d+)\+(-?\d+)")
    removed = re.compile(r"output (\d+) removed ")
    rects = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = pattern.search(line)
                if m:
                    rects[int(m.group(1))] = tuple(int(m.group(i)) for i in range(2, 6))
                    continue
                m = removed.search(line)
                if m:
                    rects.pop(int(m.group(1)), None)
    except OSError as e:
        sys.exit(f"inject-input: cannot read {path}: {e}")
    if not rects:
        sys.exit(f"inject-input: no 'output N name=… logical=WxH+X+Y' line in {path} yet")
    x0 = min(r[2] for r in rects.values())
    y0 = min(r[3] for r in rects.values())
    x1 = max(r[2] + r[0] for r in rects.values())
    y1 = max(r[3] + r[1] for r in rects.values())
    return (x1 - x0, y1 - y0), (x0, y0)


def key_code(text):
    if text.isdigit():
        return int(text)
    if text.upper() in KEYS:
        return KEYS[text.upper()]
    raise argparse.ArgumentTypeError(f"unknown key {text!r}; use a number or one of "
                                     f"{', '.join(sorted(KEYS))}")


def key_combo(text):
    """KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C: pressed in order, released in reverse."""
    return [key_code(part) for part in text.split("+")]


def run_type(inj, text):
    for ch in text:
        if ch not in TYPEABLE:
            sys.exit(f"inject-input: type: {ch!r} needs a modifier or is not mapped; "
                     f"use [a-z0-9 -]")
        inj.key(TYPEABLE[ch], True)
        time.sleep(0.02)
        inj.key(TYPEABLE[ch], False)
        time.sleep(0.03)


MODIFIERS = {"super": "KEY_LEFTMETA", "shift": "KEY_LEFTSHIFT", "ctrl": "KEY_LEFTCTRL",
             "alt": "KEY_LEFTALT"}


def mod_list(text):
    """ctrl,shift: modifier names in press order ("" or "none" for none)."""
    names = [] if text in ("", "none") else text.split(",")
    for n in names:
        if n not in MODIFIERS:
            raise argparse.ArgumentTypeError(f"unknown modifier {n!r}; use {', '.join(MODIFIERS)}")
    return names


def mods_at(text):
    """STEP:LIST — after motion STEP the held modifiers become LIST."""
    step, _, names = text.partition(":")
    if not step.isdigit():
        raise argparse.ArgumentTypeError(f"--mods-at wants STEP:LIST, got {text!r}")
    return int(step), mod_list(names)


def run_drag(inj, a):
    """Press (optionally under modifiers), move in even steps `interval` apart, release.

    With --button none it is a hover sweep. --esc-at N taps Escape after step N. --mods holds
    modifiers around the whole drag (--super is --mods super); --mods-at N:LIST changes the
    held set after step N (released ones first).
    """
    interval_ns = int(a.interval_ms * 1e6)
    steps = a.steps
    if steps <= 0:
        dist = max(abs(a.x1 - a.x0), abs(a.y1 - a.y0))
        steps = max(1, int(round(dist / a.step_px)))
    inj.move(a.x0, a.y0, "pre")
    time.sleep(0.05)
    held = []  # modifier names currently down, in press order
    button_down = False
    wanted = list(a.mods) + (["super"] if a.super and "super" not in a.mods else [])
    changes = dict(a.mods_at)
    try:
        for name in wanted:
            inj.key(KEYS[MODIFIERS[name]], True)
            held.append(name)
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
            if i in changes:
                for name in [n for n in held if n not in changes[i]]:
                    inj.key(KEYS[MODIFIERS[name]], False)
                    held.remove(name)
                for name in [n for n in changes[i] if n not in held]:
                    inj.key(KEYS[MODIFIERS[name]], True)
                    held.append(name)
        # Hold still briefly before release, as a hand does: the last position is shown
        # before the grab ends, so the final geometry is the drag's end and not a racing one.
        time.sleep(a.hold_ms / 1000.0)
    finally:
        # Released even when interrupted: a button or Super left down on a real device
        # stays down for the whole session, and every later click becomes a grab.
        if button_down:
            inj.button(a.button, False)
            time.sleep(0.03)
        for name in reversed(held):
            inj.key(KEYS[MODIFIERS[name]], False)


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
    p.add_argument("--relative", default="/dev/input/event3",
                   help="relative pointer node for `rel` (default: %(default)s, the VMware "
                        "mouse's REL_X/REL_Y node)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--output", type=parse_size, help="output size WxH in pixels")
    g.add_argument("--compositor-log", help="read the size from 'output 0 mode=WxH' in this log")
    g.add_argument("--layout", help="read every output from this compositor log: positions "
                                    "are layout coordinates over the outputs' bounding box")
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
    s.add_argument("--mods", type=mod_list, default=[],
                   help="modifiers held around the drag: comma list of super,shift,ctrl,alt")
    s.add_argument("--mods-at", type=mods_at, action="append", default=[],
                   help="STEP:LIST — after motion STEP the held modifiers become LIST")
    s.add_argument("--steps", type=int, default=0,
                   help="motion events (default: one per --step-px of distance)")
    s.add_argument("--step-px", type=float, default=4.0)
    s.add_argument("--interval-ms", type=float, default=10.0, help="time between motions")
    s.add_argument("--hold-ms", type=float, default=120.0, help="pause before release")
    s.add_argument("--esc-at", type=int, default=-1, help="tap Escape after this step")

    s = sub.add_parser("button", help="pointer button")
    s.add_argument("name", choices=sorted(BUTTONS))
    s.add_argument("action", choices=("press", "release", "click", "dblclick"))

    s = sub.add_parser("key", help="keyboard key or chord (evdev code or KEY_* name; "
                                   "KEY_LEFTCTRL+KEY_LEFTSHIFT+KEY_C presses in order and "
                                   "releases in reverse)")
    s.add_argument("code", type=key_combo)
    s.add_argument("action", choices=("press", "release", "tap"))

    s = sub.add_parser("type", help="tap each character of TEXT ([a-z0-9 -], US layout)")
    s.add_argument("text")

    s = sub.add_parser("rel", help="relative pointer motion (device units) on --relative")
    s.add_argument("dx", type=int)
    s.add_argument("dy", type=int)
    s.add_argument("--count", type=int, default=1, help="frames of this motion")
    s.add_argument("--interval-ms", type=float, default=8.0, help="time between frames")

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
    origin = (0, 0)
    if output is None and a.compositor_log:
        output = output_from_log(a.compositor_log)
    if output is None and a.layout:
        output, origin = layout_from_log(a.layout)
    inj = Injector(a.pointer, a.keyboard, output, a.tag, origin, a.relative)
    completed = False
    try:
        if a.cmd == "move":
            inj.move(a.x, a.y)
        elif a.cmd == "drag":
            run_drag(inj, a)
        elif a.cmd == "button":
            if a.action == "dblclick":
                # Two clicks 170 ms apart: inside every toolkit's double-click time (GTK and
                # the compositor's frame both use 400 ms), which two separate injector runs
                # cannot promise.
                inj.button(a.name, True)
                time.sleep(0.05)
                inj.button(a.name, False)
                time.sleep(0.12)
            if a.action in ("press", "click", "dblclick"):
                inj.button(a.name, True)
            if a.action in ("click", "dblclick"):
                time.sleep(0.05)
            if a.action in ("release", "click", "dblclick"):
                inj.button(a.name, False)
        elif a.cmd == "key":
            if a.action in ("press", "tap"):
                for code in a.code:
                    inj.key(code, True)
                    if len(a.code) > 1:
                        time.sleep(0.02)
            if a.action == "tap":
                time.sleep(0.03)
            if a.action in ("release", "tap"):
                for code in reversed(a.code):
                    inj.key(code, False)
                    if len(a.code) > 1:
                        time.sleep(0.02)
        elif a.cmd == "type":
            run_type(inj, a.text)
        elif a.cmd == "rel":
            step_ns = int(a.interval_ms * 1e6)
            deadline = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            for i in range(max(1, a.count)):
                if i:
                    deadline += step_ns
                    sleep_until(deadline)
                inj.rel(a.dx, a.dy)
        completed = True
    except OSError as e:
        sys.exit(f"inject-input: {e}")
    finally:
        # Interrupted (Ctrl+C, SIGTERM, a failed write) between a press and its release: let
        # go of what this run pressed, or the session keeps a modifier or a letter held until
        # someone taps it. Only a completed `press` means to leave something down.
        if not (completed and getattr(a, "action", None) == "press"):
            inj.release_held()
        inj.close()


if __name__ == "__main__":
    main()
