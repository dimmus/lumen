#!/usr/bin/env python3
"""Create a virtual absolute-pointer mouse on /dev/uinput and hold it open.

Notebook hosts have no absolute pointer to inject into (the touchpad is a multitouch
device: no hover, and libinput ignores bare ABS_X/Y on it), and some kernels ship
without the uinput module — where it exists, a virtual mouse is the clean target: a
separate node the smoke's watcher can tell apart from real hardware, an axis range in
output pixels (a 1:1 map), and buttons plus a wheel on the same device.

    sudo python3 scripts/uinput-mouse.py --output 3200x1800 [--name 'Lumen Inject']

Prints one JSON line {"node": "/dev/input/eventN", "pid": N} once the device exists and
holds it open (closing the uinput fd destroys the device) until SIGTERM/SIGINT. Root:
the device is created with the caller's ownership, and logind ACLs it to the active
seat like any evdev node. Pass the node to inject-input.py as both --pointer and
--relative (REL_WHEEL rides on it).
"""
import argparse
import fcntl
import json
import os
import signal
import struct
import sys
import time

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
REL_WHEEL = 0x08
ABS_X, ABS_Y = 0x00, 0x01
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112

BUS_USB = 0x03


def _iow(nr: int) -> int:
    return (1 << 30) | (4 << 16) | (ord("U") << 8) | nr


def _ior(nr: int, size: int) -> int:
    return (2 << 30) | (size << 16) | (ord("U") << 8) | nr


UI_DEV_CREATE = 0x5501
UI_SET_EVBIT = _iow(100)
UI_SET_KEYBIT = _iow(101)
UI_SET_RELBIT = _iow(102)
UI_SET_ABSBIT = _iow(103)
UI_GET_SYSNAME = _ior(60, 64)

# struct uinput_user_dev: char name[80]; uinput_id {u16 bustype, vendor, product, version};
# u32 ff_effects_max; s32 absmax[64], absmin[64], absfuzz[64], absflat[64].
UINPUT_USER_DEV = struct.Struct("80sHHHHi" + "64i" * 4)
UINPUT_SETUP = struct.Struct("80sHHHHi" + "64i" * 4)  # same layout via UI_DEV_SETUP's struct


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True, metavar="WxH",
                    help="the output size in pixels; the axes span exactly it (a 1:1 map)")
    ap.add_argument("--name", default="Lumen Inject Mouse")
    args = ap.parse_args()
    try:
        w, h = (int(v) for v in args.output.lower().split("x", 1))
    except ValueError:
        sys.exit("uinput-mouse: --output wants WxH")
    if w <= 0 or h <= 0:
        sys.exit("uinput-mouse: a positive output size")

    try:
        fd = os.open("/dev/uinput", os.O_RDWR | os.O_CLOEXEC)
    except OSError as e:
        sys.exit(f"uinput-mouse: cannot open /dev/uinput ({e}); the kernel needs the "
                 f"uinput module (modprobe uinput)")

    for evtype in (EV_SYN, EV_KEY, EV_REL, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, evtype)
    for btn in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):
        fcntl.ioctl(fd, UI_SET_KEYBIT, btn)
    fcntl.ioctl(fd, UI_SET_RELBIT, REL_WHEEL)
    for axis in (ABS_X, ABS_Y):
        fcntl.ioctl(fd, UI_SET_ABSBIT, axis)

    name = args.name.encode()[:79]
    axes_max = [0] * 64
    axes_min = [0] * 64
    axes_max[ABS_X], axes_max[ABS_Y] = w - 1, h - 1
    dev = UINPUT_USER_DEV.pack(
        name.ljust(80, b"\0"), BUS_USB, 0x6E75, 0x1FAB, 1, 0,
        *axes_max, *axes_min, *[0] * 64, *[0] * 64)
    os.write(fd, dev)
    fcntl.ioctl(fd, UI_DEV_CREATE)

    # The node name (UI_GET_SYSNAME where available, else the newest matching name).
    sysname = ""
    buf = bytearray(64)
    try:
        fcntl.ioctl(fd, UI_GET_SYSNAME, buf, True)
        sysname = buf.split(b"\0", 1)[0].decode()
    except OSError:
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            matches = []
            for d in os.listdir("/sys/class/input"):
                try:
                    with open(f"/sys/class/input/{d}/name") as f:
                        if f.read().strip() == args.name:
                            matches.append(d)
                except OSError:
                    continue
            events = []
            for m in matches:
                for e in os.listdir(f"/sys/class/input/{m}"):
                    if e.startswith("event"):
                        events.append(e)
            if events:
                sysname = sorted(events, key=lambda s: int(s[5:]))[-1]
                break
            time.sleep(0.05)
    if not sysname:
        sys.exit("uinput-mouse: the device was created but its node did not appear")

    sys.stdout.write(json.dumps({"node": f"/dev/input/{sysname}", "pid": os.getpid()}) + "\n")
    sys.stdout.flush()

    stop = {"go": True}

    def quit(sig, frame):
        stop["go"] = False

    signal.signal(signal.SIGTERM, quit)
    signal.signal(signal.SIGINT, quit)
    while stop["go"]:
        time.sleep(0.2)
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
