#!/usr/bin/env python3
"""omaface test: one self-test scan through the engine socket.

With key=value arguments the scan uses those settings instead of the saved
ones, for this scan only (a TEST request; nothing is saved). Without them it
is the ordinary self-test (VERIFY omaface-test). Either way the engine runs
the same scan as a real prompt, minus strikes, status and the choose step.

Text output for a terminal, or one JSON object per line with --json:
  {"type":"require", ...}   what this scan requires, once
  {"type":"status","state":...}
  {"type":"score", ...}     one per frame (with "preview" rows for a TEST)
  {"type":"result", ...}    the outcome; exit 0 when accepted
  {"type":"stopped"}        SIGTERM/SIGINT before a result, camera released
  {"type":"error","message":...}
"""

import argparse
import json
import math
import signal
import socket
import sys
import time

TESTABLE = ("threshold", "threshold_rgb", "match_frames", "match_frames_rgb",
            "liveness_threshold", "min_face_width", "timeout_test")
RELEASE_TIMEOUT = 3.0

MESSAGES = {"ok": "Recognized", "fail": "Not recognized", "timeout": "No face found", "locked-out": "Paused after too many attempts",
            "busy": "Camera busy", "disabled": "omaFace is off",
            "attention": "Recognized, but your eyes were closed or not on the screen"}
MOVES = {"turn-left": "<- turn your head LEFT", "turn-right": "turn your head RIGHT ->", "turn-back": "face the camera again"}
LIVENESS = {"spoof": "it did not look like a live face (a photo or a screen?)", "wrong": "moved the wrong way",
            "slow": "did not follow in time", "early": "moved before being asked",
            "lost": "the face left the camera or was swapped", "identity": "a different face turned back"}
EYES = {"ok": "paying attention", "eyes": "eyes closed",
        "away": "looking away from the screen", "head": "head turned or tilted away", "off": "attention off"}


def message(result, detail, candidate):
    text = MESSAGES.get(result, (result + " " + detail).strip())
    if result == "fail" and detail == "liveness spoof":
        text = "Face matched, but liveness could not be verified"
    elif result == "fail" and detail.startswith("liveness"):
        text = "Recognized, but the head-turn check failed: " + LIVENESS.get(detail.split()[-1], detail)
    elif result == "timeout" and detail in ("unsettled", "crowd"):
        text = {"unsettled": "Recognized, but you did not face the camera long enough to be asked",
                "crowd": "More than one face in view"}[detail]
    if result == "unavailable" and detail.startswith("not-enrolled"):
        text = "Camera works, but no face is enrolled yet" if "no-face-seen" not in detail \
            else "No face enrolled yet (and none seen by the camera)"
    elif result == "unavailable":
        text = {"lid-closed": "Lid is closed", "camera": "Camera unavailable",
                "no-camera": "No IR camera found"}.get(detail, text)
    elif result == "error" and detail == "bad-request" and candidate:
        text = "The installed engine cannot test unsaved settings; update omaFace"
    return text


def finite(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def fields_of(parts, start):
    """key value pairs after parts[start]; gaze carries two values."""
    fields, index = {}, start
    while index + 1 < len(parts):
        key = parts[index]
        fields[key] = parts[index + 1]
        index += 3 if key == "gaze" else 2
    return fields


def parse_overrides(args):
    overrides = []
    seen = set()
    for arg in args:
        key, sep, value = arg.partition("=")
        if not sep or key not in TESTABLE:
            raise ValueError(f"{key or arg} cannot be tested (testable: {', '.join(TESTABLE)})")
        if key in seen:
            raise ValueError(f"{key} given twice")
        if finite(value) is None:
            raise ValueError(f"{key} must be a finite number")
        seen.add(key)
        overrides.append((key, value))
    return overrides


class Stop(Exception):
    pass


def main():
    parser = argparse.ArgumentParser(prog="omaface test")
    parser.add_argument("--user", required=True)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--duration", type=float, default=8)
    parser.add_argument("--socket", default="/run/omaface.sock")
    parser.add_argument("settings", nargs="*", metavar="key=value")
    args = parser.parse_args()
    as_json = args.json

    def out(obj, text):
        if as_json:
            print(json.dumps(obj, allow_nan=False), flush=True)
        elif text is not None:
            print(text, flush=True)

    try:
        overrides = parse_overrides(args.settings)
    except ValueError as error:
        out({"type": "error", "message": str(error)}, f"omaface: {error}")
        return 2
    if not args.user or any(c.isspace() or ord(c) < 32 for c in args.user):
        out({"type": "error", "message": "invalid user"}, "omaface: invalid user")
        return 2
    candidate = bool(overrides)
    duration = dict(overrides).get("timeout_test", args.duration)
    duration = finite(duration) or 8
    if not 1 <= duration <= 60:
        duration = 8

    def stop(_signum, _frame):
        raise Stop()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(max(20, duration + 20))
    try:
        s.connect(args.socket)
        if candidate:
            request = f"TEST {args.user} " + " ".join(f"{k}={v}" for k, v in overrides)
        else:
            request = f"VERIFY {args.user} omaface-test"
        s.sendall((request + "\n").encode())
        if not as_json:
            if candidate:
                print("Testing with " + " ".join(f"{k}={v}" for k, v in overrides) + " (not saved)", flush=True)
            print("Look at the screen…", flush=True)
        return scan(s, out, as_json, candidate)
    except Stop:
        # Closing our side ends the scan; the engine closing its side means
        # it has exited and let go of the camera.
        released = False
        try:
            s.shutdown(socket.SHUT_WR)
            s.settimeout(RELEASE_TIMEOUT)
            deadline = time.monotonic() + RELEASE_TIMEOUT
            while time.monotonic() < deadline:
                if not s.recv(4096):
                    released = True
                    break
        except (OSError, Stop):
            pass
        if not released:
            out({"type": "error", "message": "The camera release was not confirmed"}, "omaface: camera release not confirmed")
            return 1
        out({"type": "stopped"}, "Stopped")
        return 0
    except OSError as error:
        out({"type": "error", "message": f"cannot reach the omaFace service: {error}"}, f"omaface: cannot reach the service: {error}")
        return 1
    finally:
        s.close()


def scan(s, out, as_json, candidate):
    best = best_live = None
    longest, required, camera = 0, None, None
    preview = None
    scan_start = None
    seen_attention = set()
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            parts = line.decode(errors="replace").split()
            if not parts:
                continue
            if parts[0] == "STATUS" and len(parts) > 1:
                if parts[1] == "scanning" and scan_start is None:
                    scan_start = time.monotonic()
                out({"type": "status", "state": parts[1]}, None if as_json or parts[1] not in MOVES else f"  {MOVES[parts[1]]}")
            elif parts[0] == "REQUIRE":
                fields = fields_of(parts, 1)
                camera = fields.get("camera")
                required = finite(fields.get("frames"))
                live = finite(fields.get("live"))
                out({"type": "require", "cameraKind": camera, "threshold": finite(fields.get("threshold")),
                     "liveThreshold": live if live is not None and live >= 0 else None,
                     "minWidth": finite(fields.get("min_width")), "requiredFrames": required,
                     "timeout": finite(fields.get("timeout"))}, None)
            elif parts[0] == "PREVIEW":
                preview = parts[1:]
            elif parts[0] == "SCORE":
                score = finite(parts[1])
                attention = parts[5] if len(parts) > 5 and parts[4] == "attention" else "off"
                live = finite(parts[7]) if len(parts) > 7 and parts[6] == "live" else None
                fields = fields_of(parts, 2)
                gate = fields.get("gate")
                valid_score = score is not None and gate not in ("noface", "small", "models", "not-enrolled")
                if valid_score:
                    best = score if best is None else max(best, score)
                live = live if live is not None and 0 <= live <= 1 else None
                if live is not None:
                    best_live = live if best_live is None else max(best_live, live)
                frames = finite(fields.get("frames"))
                if frames is not None:
                    longest = max(longest, int(frames))
                seen_attention.add(attention)

                def unit_score(key):
                    value = finite(fields.get(key))
                    return value if value is not None and 0 <= value <= 1 else None

                event = {"type": "score", "score": score if valid_score else None,
                         "attention": attention, "live": live,
                         "width": finite(fields.get("width")), "liveV2": unit_score("v2"), "liveV1SE": unit_score("v1se"),
                         "frames": frames, "requiredFrames": finite(fields.get("required")), "gate": gate}
                if preview is not None:
                    event["preview"] = preview
                    preview = None
                text = None
                if valid_score:
                    real = f"   real face {live:.2f}" if live is not None else ""
                    text = f"  score {score:.3f}   {EYES.get(attention, attention)}{real}"
                out(event, text)
            elif parts[0] == "RESULT" and len(parts) > 1:
                result = parts[1]
                detail = " ".join(parts[2:])
                text = message(result, detail, candidate)
                elapsed = round(time.monotonic() - scan_start, 2) if scan_start is not None else None
                out({"type": "result", "result": result, "detail": detail, "accepted": result == "ok", "elapsed": elapsed,
                     "score": best, "live": best_live, "longestStreak": longest, "requiredFrames": required,
                     "cameraKind": camera, "message": text, "attention": sorted(seen_attention)},
                    ("Recognized" if result == "ok" else text) + (f"  (best {best:.3f})" if best is not None else ""))
                return 0 if result == "ok" else 1
    out({"type": "error", "message": "The scan ended without a result"}, "omaface: the scan ended without a result")
    return 1


if __name__ == "__main__":
    sys.exit(main())
