#!/usr/bin/env python3
"""Exercise the installed-shape CLI against a disposable stub engine, without root or cameras."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VALUES = {
    "threshold": 0.5, "threshold_rgb": 0.55, "match_frames": 2,
    "match_frames_rgb": 8, "liveness_threshold": 0.85, "min_face_width": 70,
    "timeout_test": 8, "timeout_sudo": 4, "timeout_polkit": 5,
    "timeout_lock": 6, "timeout_login": 6, "timeout_choose": 30,
}
BOUNDS = {
    key: {"min": 0.30 if key.startswith("threshold") else 1, "max": 0.95 if key.startswith("threshold") else 60, "step": 0.01 if key.startswith("threshold") else 1, "integer": not key.startswith("threshold")}
    for key in VALUES
}
METADATA = {"version": 1, "revision": "test-revision", "values": VALUES, "defaults": VALUES, "bounds": BOUNDS, "attention": True, "headTurns": False}


class Cli(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="omaface-cli-test-")
        self.addCleanup(self.scratch.cleanup)
        self.prefix = Path(self.scratch.name)
        (self.prefix / "bin").mkdir()
        (self.prefix / "lib/omaface").mkdir(parents=True)
        (self.prefix / "share/omaface/tools").mkdir(parents=True)
        self.command = self.prefix / "bin/omaface"
        self.command.write_text((ROOT / "bin/omaface").read_text().replace("@PREFIX@", str(self.prefix)))
        self.command.chmod(0o755)
        engine = self.prefix / "lib/omaface/omaface-engine"
        engine.write_text("#!/usr/bin/python3 -I\nimport json,sys\n" +
                          "metadata=" + repr(METADATA) + "\n" +
                          "if sys.argv[1]=='config': print(json.dumps(metadata))\n" +
                          "elif sys.argv[1]=='validate-settings':\n    if len(sys.argv)<3 or any('=' not in arg or arg.endswith('=nan') for arg in sys.argv[2:]): sys.exit(1)\n" +
                          "else: sys.exit(90)\n")
        engine.chmod(0o755)
        self.env = {**os.environ, "PATH": str(self.prefix / "bin") + ":" + os.environ["PATH"]}
        for name in ("sudo", "pkexec"):
            tool = self.prefix / "bin" / name
            tool.write_text("#!/bin/bash\nprintf '%s\\n' \"$@\"\nexit 77\n")
            tool.chmod(0o755)
        # socket_active asks systemd; answer yes without touching the real one.
        tool = self.prefix / "bin/systemctl"
        tool.write_text("#!/bin/bash\nexit 0\n")
        tool.chmod(0o755)
        # A stand-in client that records its arguments: the real one would
        # connect to /run/omaface.sock and turn a camera on.
        helper = self.prefix / "share/omaface/tools/test-client.py"
        helper.write_text("import json,sys; print(json.dumps({'args':sys.argv[1:]}))\n")

    def run_cli(self, *args):
        return subprocess.run([str(self.command), *args], env=self.env, capture_output=True, text=True, timeout=3)

    def test_help(self):
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("test [key=value...]", result.stdout)
        self.assertNotIn("calibrate", result.stdout)
        self.assertIn("apply <revision>", result.stdout)

    def test_camera_free_metadata(self):
        result = self.run_cli("advanced", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), METADATA)

    def test_human_metadata(self):
        result = self.run_cli("advanced")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Advanced calibration (system-wide)", result.stdout)
        self.assertIn("liveness_threshold", result.stdout)
        self.assertIn("omaface test liveness_threshold", result.stdout)

    def test_argument_validation(self):
        for args in (("advanced", "--bad"), ("test", "--bad"), ("test", "threshold=nan"), ("set", "threshold"), ("set", "threshold", "0.5", "extra"), ("apply", "-", "threshold=0.5"), ("apply", "bad revision", "threshold=0.5")):
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn("pkexec", result.stdout)

    def test_invalid_numeric_setting_does_not_authorize(self):
        result = self.run_cli("set", "threshold", "nan")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")

    def test_valid_batch_goes_through_authorization(self):
        if os.geteuid() == 0:
            self.skipTest("test must run unprivileged")
        result = self.run_cli("apply", "test-revision", "threshold=0.5")
        self.assertEqual(result.returncode, 77)
        self.assertIn(str(self.command), result.stdout)
        self.assertIn("test-revision", result.stdout)
        self.assertIn("threshold=0.5", result.stdout)

    def test_test_values_go_to_the_client_without_authorization(self):
        result = self.run_cli("test", "--json", "threshold=0.6", "match_frames_rgb=4")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = json.loads(result.stdout)["args"]
        self.assertEqual(args[0], "--user")
        self.assertEqual(args[2], "--duration")
        self.assertEqual(args[4:], ["--json", "threshold=0.6", "match_frames_rgb=4"])
        self.assertNotIn("pkexec", result.stdout)

    def test_ordinary_test_has_no_values(self):
        result = self.run_cli("test", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["args"][4:], ["--json"])

    def test_calibrate_is_gone(self):
        self.assertEqual(self.run_cli("calibrate", "--json").returncode, 2)

if __name__ == "__main__":
    unittest.main()
