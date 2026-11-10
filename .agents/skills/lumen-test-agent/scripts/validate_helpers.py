#!/usr/bin/env python3
"""Validate helper behavior with temporary data and mocked package transactions."""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("test_tools", HERE / "test_tools.py")
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class HelperTests(unittest.TestCase):
    def test_distro_and_package_plans(self):
        self.assertEqual(tools.detect_manager({"ID": "custom", "ID_LIKE": "debian"}), "apt")
        with self.assertRaises(ValueError):
            tools.detect_manager({"ID": "unknown"})
        for manager in tools.MANAGERS:
            packages = tools.packages_for(manager, tools.selected_tools(["core", "memory"]), {})
            self.assertEqual(len(packages), len(set(packages)))
            self.assertIn("heaptrack", packages)
            self.assertTrue(tools.install_command(manager, packages))
        with mock.patch.object(tools.platform, "release", return_value="test-kernel"):
            self.assertEqual(tools.packages_for("apt", ["perf"], {"ID": "ubuntu"}), ["linux-tools-test-kernel"])

    def test_missing_package_prevents_mutation(self):
        with mock.patch.object(tools.platform, "system", return_value="Linux"), \
             mock.patch.object(tools.platform, "freedesktop_os_release", return_value={"ID": "arch"}), \
             mock.patch.object(tools, "probe", side_effect=lambda names: {name: None for name in names}), \
             mock.patch.object(tools.shutil, "which", return_value="/bin/pacman"), \
             mock.patch.object(tools, "available", return_value=False), \
             mock.patch.object(tools.subprocess, "run") as transaction, \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(tools.main(["--install", "--groups", "cpu", "--yes"]), 2)
            transaction.assert_not_called()

    def test_package_failure_propagates(self):
        with mock.patch.object(tools.platform, "system", return_value="Linux"), \
             mock.patch.object(tools.platform, "freedesktop_os_release", return_value={"ID": "arch"}), \
             mock.patch.object(tools, "probe", side_effect=lambda names: {name: None for name in names}), \
             mock.patch.object(tools.shutil, "which", return_value="/bin/pacman"), \
             mock.patch.object(tools, "available", return_value=True), \
             mock.patch.object(tools.os, "geteuid", return_value=0, create=True), \
             mock.patch.object(tools.subprocess, "run", return_value=subprocess.CompletedProcess([], 7)), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tools.main(["--install", "--groups", "cpu", "--yes"]), 7)

    def capture(self, directory, code, timeout=10):
        return subprocess.run([sys.executable, str(HERE / "capture_run.py"),
                               "--output", str(directory), "--cwd", str(HERE),
                               "--timeout", str(timeout), "--question", "helper validation",
                               "--revision", "fixture", "--kind", "helper", "--",
                               sys.executable, "-c", code], capture_output=True, text=True, timeout=20)

    def test_capture_success_failure_and_collision(self):
        with tempfile.TemporaryDirectory() as tmp:
            success = Path(tmp) / "success"
            self.assertEqual(self.capture(success, "print('evidence')").returncode, 0)
            manifest = json.loads((success / "result.json").read_text())
            self.assertEqual(manifest["assessment"], "UNASSESSED")
            self.assertEqual(manifest["execution_status"], "completed")
            for artifact in manifest["artifacts"]:
                self.assertEqual(hashlib.sha256((success / artifact["path"]).read_bytes()).hexdigest(), artifact["sha256"])
            self.assertEqual(self.capture(success, "print('overwrite')").returncode, 2)
            failure = Path(tmp) / "failure"
            self.assertEqual(self.capture(failure, "import sys; print('bad', file=sys.stderr); sys.exit(9)").returncode, 9)
            self.assertEqual(json.loads((failure / "result.json").read_text())["exit_code"], 9)

    def test_timeout_is_not_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "timeout"
            result = self.capture(directory, "import time; time.sleep(30)", timeout=0.1)
            self.assertEqual(result.returncode, 124)
            manifest = json.loads((directory / "result.json").read_text())
            self.assertEqual(manifest["execution_status"], "timed_out")
            self.assertEqual(manifest["assessment"], "UNASSESSED")
            for bad_timeout in ("nan", "inf", "-inf", "0"):
                invalid = subprocess.run([sys.executable, str(HERE / "capture_run.py"),
                                          "--output", str(Path(tmp) / bad_timeout), "--timeout", bad_timeout,
                                          "--question", "invalid timeout", "--revision", "fixture",
                                          "--kind", "helper", "--", sys.executable, "-c", "pass"],
                                         capture_output=True, text=True, timeout=5)
                self.assertEqual(invalid.returncode, 2, bad_timeout)
                self.assertFalse((Path(tmp) / bad_timeout).exists(), bad_timeout)


if __name__ == "__main__":
    unittest.main()
