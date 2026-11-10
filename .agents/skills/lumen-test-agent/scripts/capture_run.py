#!/usr/bin/env python3
"""Capture one bounded non-seat command and its evidence manifest without a shell."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="New directory; existing paths are refused")
    parser.add_argument("--cwd", type=Path, default=Path.cwd())
    parser.add_argument("--timeout", type=float, default=300, help="Seconds before TERM, then KILL after 5 seconds")
    parser.add_argument("--question", required=True)
    parser.add_argument("--revision", required=True, help="Exact revision/dirty-state description supplied by coordinator")
    parser.add_argument("--kind", choices=("syntax", "helper", "generator", "unit", "integration", "benchmark", "diagnostic"), required=True)
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="Recorded override; never pass secrets")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="Command argv after --; no shell interpretation")
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("A command and finite positive timeout are required")
    overrides = {}
    for value in args.env:
        name, separator, text = value.partition("=")
        if not separator or not name or "\x00" in value:
            parser.error("--env needs a valid NAME=VALUE")
        overrides[name] = text
    cwd = args.cwd.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = {
        "schema_version": 1, "question": args.question, "revision": args.revision,
        "kind": args.kind, "argv": command, "cwd": str(cwd), "environment_overrides": overrides,
        "host": platform.node(), "platform": platform.platform(), "started_at": utc_now(),
        "timeout_seconds": args.timeout, "execution_status": "running", "exit_code": None,
        "assessment": "UNASSESSED", "capability_gaps": [], "artifacts": [],
        "cleanup": "No seat/service restoration is provided; use only bounded non-seat commands",
    }
    manifest_path = output / "result.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    start = time.monotonic()
    process = None
    result = 2

    def stop_owned():
        if process is None:
            return
        # The new session owns this process group, including descendants on Linux.
        if os.name == "posix":
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                return
        elif process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            else:
                process.kill()
            process.wait()
        # Even if the group leader exited, descendants must not survive a timeout.
        if os.name == "posix":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    try:
        with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, cwd=cwd, env={**os.environ, **overrides},
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                       start_new_session=os.name == "posix")
            try:
                code = process.wait(timeout=args.timeout)
                manifest["execution_status"] = "completed"
                manifest["exit_code"] = code
                result = code if code >= 0 else 1
            except subprocess.TimeoutExpired:
                manifest["execution_status"] = "timed_out"
                stop_owned()
                manifest["exit_code"] = process.returncode
                result = 124
    except KeyboardInterrupt:
        manifest["execution_status"] = "interrupted"
        stop_owned()
        manifest["exit_code"] = process.returncode if process else None
        result = 130
    except OSError as error:
        manifest["execution_status"] = "could_not_run"
        manifest["error"] = str(error)
        stop_owned()
    finally:
        manifest["ended_at"] = utc_now()
        manifest["duration_seconds"] = round(time.monotonic() - start, 6)
        for name in ("stdout.log", "stderr.log"):
            path = output / name
            if path.exists():
                hasher = hashlib.sha256()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        hasher.update(chunk)
                manifest["artifacts"].append({"path": name, "bytes": path.stat().st_size, "sha256": hasher.hexdigest()})
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(str(manifest_path))
    return result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("ERROR: " + str(error), file=sys.stderr)
        sys.exit(2)
