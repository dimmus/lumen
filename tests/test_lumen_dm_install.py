"""Verify display-manager packaging without installing or enabling anything on the host."""

import configparser
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib


def install_script(source, build, cmake, destdir, prefix):
    env = dict(os.environ, DESTDIR=str(destdir))
    script = destdir / "stage.cmake"
    lines = {
        "CMAKE_INSTALL_PREFIX": prefix,
        "LUMEN_SOURCE_DIR": source,
        "LUMEN_BINARY_DIR": build,
        "LUMEN_STAGE_DIR": destdir / "templates",
        "LUMEN_BINDIR_REL": "bin",
        "LUMEN_LIBEXECDIR_REL": "libexec",
        "LUMEN_DATADIR_REL": "share",
        "LUMEN_DM_BUILT": "ON",
        "LUMEN_SYSTEMD_SYSTEM_UNIT_DIR_USR": "/usr/lib/systemd/system",
        "LUMEN_SYSTEMD_SYSUSERS_DIR_USR": "/usr/lib/sysusers.d",
    }
    script.write_text(
        "".join(f"set({key} [==[{value}]==])\n" for key, value in lines.items())
        + f"include([==[{source / 'cmake/LumenInstallSession.cmake'}]==])\n"
    )
    subprocess.run([cmake, "-P", str(script)], env=env, check=True)


def check(source, build, cmake, prefix):
    with tempfile.TemporaryDirectory(prefix="lumen-dm-install-") as scratch:
        destdir = Path(scratch)
        install_script(source, build, cmake, destdir, prefix)
        sysconf = "/etc" if prefix == "/usr" else f"{prefix}/etc"
        config = f"{sysconf}/lumen-dm"
        system_unit = destdir / f"{prefix[1:]}/lib/systemd/system/lumen-dm.service"
        sysusers = destdir / f"{prefix[1:]}/lib/sysusers.d/lumen-dm.conf"
        etc = destdir / config[1:]

        unit = configparser.ConfigParser(interpolation=None)
        assert unit.read(system_unit)
        assert unit["Install"]["Alias"] == "display-manager.service"
        assert unit["Unit"]["Conflicts"] == "getty@tty1.service"
        assert unit["Unit"]["StartLimitBurst"] == "5"
        assert unit["Service"]["UMask"] == "0077"
        assert unit["Service"]["ExecStart"] == f"greetd --config {config}/greetd.toml"
        assert not list(destdir.rglob("display-manager.service")), "install enabled the manager"
        assert sysusers.read_text().splitlines()[-1].startswith("u lumen-dm ")

        greetd = tomllib.loads((etc / "greetd.toml").read_text())
        assert greetd["terminal"] == {"vt": 1, "switch": True}
        assert greetd["general"]["source_profile"] is False
        assert "initial_session" not in greetd, "install must not configure autologin"
        assert greetd["default_session"]["user"] == "lumen-dm"
        assert greetd["default_session"]["command"] == (
            f"{prefix}/libexec/lumen-dm {prefix}/bin/lumen-session"
        )
        assert not (etc / "weston.ini").exists()


if __name__ == "__main__":
    source_dir, build_dir, cmake_path = map(Path, sys.argv[1:])
    check(source_dir, build_dir, str(cmake_path), "/usr")
    check(source_dir, build_dir, str(cmake_path), "/opt/lumen")
