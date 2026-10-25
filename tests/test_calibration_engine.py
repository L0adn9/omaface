#!/usr/bin/env python3
"""Camera-free engine tests; all binaries, config and runtime state stay in scratch."""

import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KEYS = {
    "threshold": (0.50, 0.30, 0.95, False),
    "threshold_rgb": (0.55, 0.30, 0.95, False),
    "match_frames": (2, 1, 5, True),
    "match_frames_rgb": (8, 3, 30, True),
    "liveness_threshold": (0.85, 0.50, 0.99, False),
    "min_face_width": (70, 30, 300, True),
    "timeout_test": (8, 1, 60, False),
    "timeout_sudo": (4, 1, 15, False),
    "timeout_polkit": (5, 1, 15, False),
    "timeout_lock": (6, 1, 15, False),
    "timeout_login": (6, 1, 15, False),
    "timeout_choose": (30, 5, 120, False),
}
USER = pwd.getpwuid(os.getuid()).pw_name


class Connection:
    def __init__(self, binary, request):
        self.sock, child = socket.socketpair()
        self.sock.settimeout(4)
        self.process = subprocess.Popen(
            [str(binary), "serve"], stdin=child, stdout=child,
            stderr=subprocess.PIPE,
        )
        child.close()
        self.buffer = bytearray()
        self.sock.sendall(request.encode())

    def line(self):
        while b"\n" not in self.buffer:
            data = self.sock.recv(65536)
            if not data:
                raise AssertionError("engine closed before its response")
            self.buffer.extend(data)
        line, _, rest = self.buffer.partition(b"\n")
        self.buffer = bytearray(rest)
        return line.decode()

    def finish(self):
        try:
            return self.process.wait(timeout=4)
        finally:
            self.sock.close()
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stderr.close()


class CalibrationEngineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="omaface-calibration-engine-")
        cls.scratch = Path(cls.tmp.name)
        cls.configdir = cls.scratch / "config"
        cls.configdir.mkdir(mode=0o700)
        cls.config = cls.configdir / "config"
        cls.runtime = cls.scratch / "run"
        cls.runtime.mkdir(mode=0o700)
        cls.data = cls.scratch / "data"
        cls.data.mkdir(mode=0o700)
        cls.engine = cls.scratch / "engine"
        # An in-tree build is compiled for the real /etc and /run paths and
        # would look up to date to make; always build fresh from source.
        shutil.copytree(ROOT / "engine", cls.engine,
                        ignore=shutil.ignore_patterns("omaface-engine", "omaface-pam"))
        # The selftest needs the installed models: git install or package.
        cls.prefix = next((p for p in ("/usr/local", "/usr")
                           if Path(p, "share/omaface/models/face_detection_yunet_2023mar.onnx").exists()), None)
        cls.binary = cls.engine / "omaface-engine"
        flags = " ".join([
            f"-DOMAFACE_CONFIG='\"{cls.config}\"'",
            f"-DOMAFACE_RUN_DIR='\"{cls.runtime}\"'",
            f"-DOMAFACE_DATA_DIR='\"{cls.data}\"'",
        ])
        build = subprocess.run(
            ["make", "-C", str(cls.engine), "all", f"PREFIX={cls.prefix or '/usr/local'}", f"CPPFLAGS={flags}"],
            capture_output=True, text=True,
        )
        if build.returncode:
            raise RuntimeError("scratch engine build failed:\n" + build.stdout + build.stderr)
        cls.core = cls.scratch / "calibration-core-test"
        compile_core = subprocess.run(
            ["g++", "-O2", "-Wall", "-Wextra", "-std=c++17", "-pthread",
             "-DOMAFACE_TEST_SETTINGS_ROOT", str(ROOT / "tests/test_calibration_engine.cpp"), "-o", str(cls.core)],
            capture_output=True, text=True,
        )
        if compile_core.returncode:
            raise RuntimeError("core test build failed:\n" + compile_core.stdout + compile_core.stderr)
        print(f"Scratch engine/PAM and core builds passed: {cls.scratch}", flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.original = (
            "# scratch only\nlock=off\nattention=on\nhead_turns=on\n"
            "device=/nonexistent/omaface-calibration-test\nthreshold=0.50 # keep\n"
        )
        self.config.write_text(self.original)
        config_lock = self.configdir / ".config.lock"
        if config_lock.exists():
            config_lock.unlink()
        self.sentinels = {}
        for name in ("status", f"choice-{os.getuid()}", f"fail-{os.getuid()}"):
            path = self.runtime / name
            path.write_text("sentinel " + name + "\n")
            self.sentinels[path] = path.read_bytes()
        lock = self.runtime / "camera.lock"
        if lock.exists():
            lock.unlink()

    def command(self, *args):
        return subprocess.run([str(self.binary), *args], capture_output=True, text=True, timeout=5)

    def unchanged(self):
        self.assertEqual(self.config.read_text(), self.original)
        for path, contents in self.sentinels.items():
            self.assertEqual(path.read_bytes(), contents)

    def connect(self, request):
        return Connection(self.binary, request)

    def test_metadata_defaults_and_bounds(self):
        result = self.command("config")
        self.assertEqual(result.returncode, 0, result.stderr)
        metadata = json.loads(result.stdout)
        self.assertEqual(set(metadata), {"version", "values", "defaults", "bounds", "revision", "attention", "headTurns"})
        self.assertEqual(metadata["version"], 1)
        self.assertRegex(metadata["revision"], r"^[A-Za-z0-9._:-]+$")
        for key, (default, low, high, whole) in KEYS.items():
            self.assertEqual(metadata["values"][key], default)
            self.assertEqual(metadata["defaults"][key], default)
            self.assertEqual(metadata["bounds"][key], {"min": low, "max": high, "step": 0.01 if "threshold" in key else 1, "integer": whole})
        self.assertTrue(metadata["attention"])
        self.assertTrue(metadata["headTurns"])
        self.assertFalse((self.runtime / "camera.lock").exists())
        self.unchanged()

    def test_malformed_saved_numbers_use_defaults(self):
        for invalid in ("nan", "NaN", "inf", "-inf", "1e9999", "1e-9999", "0.5junk", "0x1p-1"):
            with self.subTest(invalid=invalid):
                self.config.write_text("\n".join(k + "=" + invalid for k in KEYS))
                result = self.command("config")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)["values"], {k: v[0] for k, v in KEYS.items()})
        self.config.write_text("threshold=0.8\nthreshold=garbage\nmatch_frames=3junk\n")
        result = json.loads(self.command("config").stdout)
        self.assertEqual(result["values"]["threshold"], 0.5)
        self.assertEqual(result["values"]["match_frames"], 2)

    def test_all_setting_bounds(self):
        for key, (default, low, high, whole) in KEYS.items():
            for value in (default, low, high):
                with self.subTest(key=key, value=value):
                    result = self.command("validate-settings", f"{key}={value}")
                    self.assertEqual(result.returncode, 0, result.stderr)
            for value in (low - 0.001, high + 0.001, "nan", "inf", "0.5junk"):
                with self.subTest(key=key, invalid=value):
                    self.assertEqual(self.command("validate-settings", f"{key}={value}").returncode, 1)
            if whole:
                self.assertEqual(self.command("validate-settings", f"{key}={low + 0.5}").returncode, 1)
        self.unchanged()
        self.assertFalse((self.configdir / ".config.lock").exists())

    def test_invalid_batches_and_privilege(self):
        for args in ((), ("threshold",), ("threshold=0.5=x",), ("threshold=0.5", "threshold=0.6"),
                     ("unknown=1",), ("attention=off",), ("threshold=0.6", "min_face_width=29")):
            with self.subTest(args=args):
                self.assertEqual(self.command("validate-settings", *args).returncode, 1)
                self.assertEqual(self.command("apply-settings", "-", *args).returncode, 1)
        result = self.command("apply-settings", "-", "threshold=0.6")
        self.assertEqual(result.returncode, 1)
        self.assertIn("must run as root", result.stderr)
        self.unchanged()
        self.assertFalse((self.configdir / ".config.lock").exists())

    def test_invalid_requests_never_scan(self):
        # Candidate values only ride on TEST, and only testable, in-bounds ones.
        for request in (f"TEST {USER} threshold=nan\n", f"TEST {USER} threshold=0.1\n",
                        f"TEST {USER} timeout_test=61\n", f"TEST {USER} timeout_sudo=4\n",
                        f"TEST {USER} attention=off\n", f"TEST {USER} threshold=.5 threshold=.6\n",
                        f"TEST {USER} match_frames_rgb=4.5\n", f"TEST {USER} omaface-test\n", "TEST\n",
                        f"TEST {USER} threshold=0.5\x00\n", f"VERIFY {USER} omaface-test threshold=0.3\n",
                        f"VERIFY {USER} sudo threshold=0.3\n", f"CALIBRATE {USER} 1\n", "RESET extra\n",
                        "CHOOSE face extra\n"):
            with self.subTest(request=request):
                c = self.connect(request)
                self.assertEqual(c.line(), "RESULT error bad-request")
                self.assertEqual(c.finish(), 1)
        self.assertFalse((self.runtime / "camera.lock").exists())
        self.unchanged()

    def test_candidate_test_is_the_self_test(self):
        for request in (f"TEST {USER} liveness_threshold=0.7 match_frames_rgb=4 timeout_test=3\n", f"TEST {USER}\n"):
            with self.subTest(request=request):
                c = self.connect(request)
                # The scratch config names a camera that does not exist; a
                # closed laptop lid is checked before it.
                self.assertIn(c.line(), ("RESULT unavailable no-camera", "RESULT unavailable lid-closed"))
                self.assertEqual(c.finish(), 1)
        self.unchanged()

    def test_candidate_test_only_for_yourself(self):
        if os.getuid() == 0:
            self.skipTest("test must run unprivileged")
        c = self.connect("TEST root threshold=0.5\n")
        self.assertEqual(c.line(), "RESULT denied peer")
        self.assertEqual(c.finish(), 1)
        self.unchanged()

    def test_incomplete_request_times_out(self):
        c = self.connect(f"TEST {USER} thresh")
        self.assertEqual(c.line(), "RESULT error bad-request")
        self.assertEqual(c.finish(), 1)
        self.assertFalse((self.runtime / "camera.lock").exists())
        self.unchanged()

    def test_basic_test_keeps_result_protocol_without_side_effects(self):
        c = self.connect(f"VERIFY {USER} omaface-test\n")
        # A closed laptop lid is checked before the (nonexistent) camera.
        self.assertIn(c.line(), ("RESULT unavailable no-camera", "RESULT unavailable lid-closed"))
        self.assertEqual(c.finish(), 1)
        self.unchanged()

    def test_models_selftest_without_camera(self):
        if not self.prefix:
            self.skipTest("face models are not installed")
        result = self.command("selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "ok")
        self.assertFalse((self.runtime / "camera.lock").exists())
        self.unchanged()

    def test_core_gates_requests_and_atomic_save(self):
        result = subprocess.run([str(self.core), str(self.configdir / "core-config")], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), "calibration core tests passed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
