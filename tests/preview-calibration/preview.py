#!/usr/bin/env python3
"""Prepare or run a camera-free Quickshell preview using the production Calibration.qml."""

import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
NATIVE = Path("/usr/share/omarchy/shell")


def prepare(theme, parent=None, tests=False):
    scratch = Path(tempfile.mkdtemp(prefix="omaface-calibration-preview-", dir=parent))
    for module in ("Commons", "Ui"):
        (scratch / module).symlink_to(NATIVE / module, target_is_directory=True)
    (scratch / "plugin").symlink_to(PROJECT / "plugin", target_is_directory=True)
    shutil.copy2(HERE / "shell.qml", scratch / "shell.qml")
    shutil.copy2(HERE / "Fixtures.js", scratch / "Fixtures.js")
    if tests:
        shutil.copy2(HERE / "InteractionTests.qml", scratch / "InteractionTests.qml")
        shell = (scratch / "shell.qml").read_text()
        shell = shell.replace("      Calibration {", "      InteractionTests { harness: root; calibration: calibration }\n\n      Calibration {", 1)
        (scratch / "shell.qml").write_text(shell)
    home = scratch / "home"
    theme_target = home / ".local/state/omarchy/current/theme"
    theme_target.mkdir(parents=True)
    if theme:
        source = Path(theme).expanduser().resolve()
        if not source.is_dir():
            source = Path("/usr/share/omarchy/themes") / theme
    else:
        source = Path.home() / ".local/state/omarchy/current/theme"
        if not source.is_dir():
            source = Path.home() / ".config/omarchy/current/theme"
        if not source.is_dir():
            source = Path("/usr/share/omarchy/themes/tokyo-night")
    for name in ("colors.toml", "shell.toml"):
        if (source / name).is_file():
            shutil.copy2(source / name, theme_target / name)
    (scratch / "preview.json").write_text(json.dumps({
        "config": str(scratch), "home": str(home), "theme": str(source),
        "component": str(PROJECT / "plugin/Calibration.qml"),
        "fixtures": True, "camera": False, "authentication": False,
    }, indent=2) + "\n")
    return scratch, home


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--offscreen", action="store_true")
    parser.add_argument("--theme", help="Native theme name or directory; copied to an isolated HOME")
    parser.add_argument("--scenario", default="reject")
    parser.add_argument("--width", type=int, default=740)
    parser.add_argument("--height", type=int, default=850)
    parser.add_argument("--capture", type=Path, help="Save a labelled native PNG and exit")
    parser.add_argument("--scratch-parent", type=Path)
    parser.add_argument("--tests", action="store_true", help="Run QtTest interactions inside Quickshell")
    args = parser.parse_args()
    scratch, home = prepare(args.theme, args.scratch_parent, args.tests)
    print(f"CONFIG={scratch}", flush=True)
    print(f"HOME={home}", flush=True)
    print(f"IPC: qs ipc -p {scratch} call calibration scenario pass", flush=True)
    if args.prepare_only:
        return 0
    env = dict(os.environ, HOME=str(home), OMAFACE_PREVIEW_SCENARIO=args.scenario,
               OMAFACE_PREVIEW_WIDTH=str(args.width), OMAFACE_PREVIEW_HEIGHT=str(args.height))
    if args.offscreen:
        env.update(QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
    if args.capture:
        env.update(OMAFACE_PREVIEW_CAPTURE=str(args.capture.resolve()), OMAFACE_PREVIEW_EXIT_AFTER_CAPTURE="1")
    if args.tests:
        env["OMAFACE_PREVIEW_TESTS"] = "1"
    command = ["qs", "--no-color", "-n", "-p", str(scratch)]
    if args.tests:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)
        print(result.stdout, end="")
        report = re.search(r"Native calibration interaction tests: (\d+) passed, (\d+) failed, (\d+) skipped", result.stdout)
        if result.returncode != 0 or not report or int(report[1]) < 12 or int(report[2]) != 0 or int(report[3]) != 0:
            return 1
        return 0
    return subprocess.call(command, env=env)


if __name__ == "__main__":
    raise SystemExit(main())
