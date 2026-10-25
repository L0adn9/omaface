#!/usr/bin/env python3
"""Camera-free tests for tools/test-client.py against a scripted engine socket."""

import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLIENT = ROOT / "tools" / "test-client.py"
BRAILLE = "⣿⠤⠀"
SCAN = [
    "STATUS scanning",
    "REQUIRE camera webcam threshold 0.550 live 0.700 min_width 70 frames 4 timeout 6",
    f"PREVIEW {BRAILLE} {BRAILLE}",
    "SCORE 0.720 width 150 attention ok live 0.91 gaze 1 -2 eyes 0.40 v2 0.93 v1se 0.89 frames 1 required 4 gate pass",
    "SCORE -1 width -1 attention unavailable live -1 gaze 0 0 eyes 0 v2 -1 v1se -1 frames 0 required 4 gate noface",
    "SCORE 0.740 width 152 attention ok live 0.95 gaze 1 -2 eyes 0.40 v2 0.96 v1se 0.94 frames 4 required 4 gate pass",
]


class Engine:
    """One fake engine connection: records the request, replies with lines."""

    def __init__(self, test, lines=(), close=True, hold=False):
        self.scratch = tempfile.TemporaryDirectory(prefix="omaface-test-client-")
        test.addCleanup(self.scratch.cleanup)
        self.path = str(Path(self.scratch.name) / "socket")
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path)
        self.server.listen(1)
        self.server.settimeout(5)
        test.addCleanup(self.server.close)
        self.request = None
        self.saw_eof = threading.Event()
        self.thread = threading.Thread(target=self.serve, args=(list(lines), close, hold), daemon=True)
        self.thread.start()

    def serve(self, lines, close, hold):
        try:
            conn, _ = self.server.accept()
        except socket.timeout:
            return
        with conn:
            data = b""
            while not data.endswith(b"\n"):
                chunk = conn.recv(1024)
                if not chunk:
                    break
                data += chunk
            self.request = data.decode().strip()
            for line in lines:
                conn.sendall((line + "\n").encode())
            if close:
                return
            # Like the engine: keep scanning until the client goes away; with
            # hold, never let go (a wedged engine).
            conn.settimeout(10)
            try:
                while conn.recv(1024):
                    pass
            except socket.timeout:
                return
            self.saw_eof.set()
            if hold:
                time.sleep(6)


def run(engine, *args, json_mode=True):
    command = ["python3", "-I", str(CLIENT), "--user", "tester", "--socket", engine.path]
    if json_mode:
        command.append("--json")
    return subprocess.run(command + list(args), capture_output=True, text=True, timeout=15)


def events(result):
    return [json.loads(line) for line in result.stdout.splitlines()]


class TestClient(unittest.TestCase):
    def test_candidate_values_send_test_and_report_the_scan(self):
        engine = Engine(self, SCAN + ["RESULT ok"])
        result = run(engine, "liveness_threshold=0.70", "match_frames_rgb=4", "timeout_test=6")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(engine.request, "TEST tester liveness_threshold=0.70 match_frames_rgb=4 timeout_test=6")
        out = events(result)
        self.assertEqual([e["type"] for e in out], ["status", "require", "score", "score", "score", "result"])
        self.assertEqual(out[1], {"type": "require", "cameraKind": "webcam", "threshold": 0.55, "liveThreshold": 0.7,
                                  "minWidth": 70, "requiredFrames": 4, "timeout": 6})
        self.assertEqual(out[2]["preview"], [BRAILLE, BRAILLE])
        self.assertEqual(out[2]["score"], 0.72)
        self.assertEqual(out[2]["liveV2"], 0.93)
        self.assertNotIn("preview", out[3])
        self.assertIsNone(out[3]["score"])
        self.assertIsNone(out[3]["live"])
        final = out[-1]
        self.assertTrue(final["accepted"])
        self.assertEqual(final["score"], 0.74)
        self.assertEqual(final["live"], 0.95)
        self.assertEqual(final["longestStreak"], 4)
        self.assertEqual(final["requiredFrames"], 4)
        self.assertEqual(final["cameraKind"], "webcam")
        self.assertIsInstance(final["elapsed"], float)

    def test_without_values_it_is_the_ordinary_self_test(self):
        engine = Engine(self, ["STATUS scanning", "RESULT fail liveness spoof"])
        result = run(engine)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(engine.request, "VERIFY tester omaface-test")
        final = events(result)[-1]
        self.assertFalse(final["accepted"])
        self.assertEqual(final["message"], "Face matched, but liveness could not be verified")
        self.assertIsNone(final["score"])

    def test_old_engine_refusing_test_says_update(self):
        engine = Engine(self, ["RESULT error bad-request"])
        final = events(run(engine, "threshold=0.6"))[-1]
        self.assertEqual(final["message"], "The installed engine cannot test unsaved settings; update omaFace")

    def test_invalid_values_never_connect(self):
        for args in (("timeout_sudo=4",), ("threshold=nan",), ("threshold",), ("threshold=0.5", "threshold=0.6"), ("device=/dev/video0",)):
            with self.subTest(args=args):
                engine = Engine(self)
                result = run(engine, *args)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(events(result)[0]["type"], "error")
                self.assertIsNone(engine.request)

    def test_text_output(self):
        engine = Engine(self, SCAN + ["RESULT ok"])
        result = run(engine, "threshold_rgb=0.6", json_mode=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertEqual(lines[0], "Testing with threshold_rgb=0.6 (not saved)")
        self.assertEqual(lines[1], "Look at the screen…")
        self.assertIn("  score 0.720   paying attention   real face 0.91", lines)
        self.assertEqual(lines[-1], "Recognized  (best 0.740)")

    def stop_while_scanning(self, hold):
        engine = Engine(self, SCAN[:3], close=False, hold=hold)
        command = ["python3", "-I", str(CLIENT), "--user", "tester", "--socket", engine.path, "--json", "threshold=0.6"]
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first = proc.stdout.readline()
        self.assertEqual(json.loads(first)["type"], "status")
        proc.send_signal(signal.SIGTERM)
        started = time.monotonic()
        out, _ = proc.communicate(timeout=10)
        return proc.returncode, [json.loads(line) for line in out.splitlines()], time.monotonic() - started, engine

    def test_stop_waits_for_the_engine_to_let_go(self):
        code, out, _, engine = self.stop_while_scanning(hold=False)
        self.assertEqual(code, 0)
        self.assertEqual(out[-1], {"type": "stopped"})
        self.assertTrue(engine.saw_eof.is_set())

    def test_stop_reports_an_unconfirmed_release(self):
        code, out, took, _ = self.stop_while_scanning(hold=True)
        self.assertEqual(code, 1)
        self.assertEqual(out[-1]["type"], "error")
        self.assertLess(took, 5)

    def test_scan_without_result_is_an_error(self):
        engine = Engine(self, ["STATUS scanning"])
        result = run(engine, "threshold=0.6")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(events(result)[-1]["type"], "error")


if __name__ == "__main__":
    unittest.main()
