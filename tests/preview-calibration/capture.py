#!/usr/bin/env python3
"""Capture the actual native calibration view in labelled camera-free scenarios."""

import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "tests/preview-calibration/preview.py"
CASES = (
    ("ready", "tokyo-night", "ready", 740, 900),
    ("testing", "tokyo-night", "running", 740, 900),
    ("building-streak", "tokyo-night", "streak", 740, 900),
    ("accepted", "tokyo-night", "pass", 740, 900),
    ("not-accepted-liveness", "tokyo-night", "reject", 740, 900),
    ("not-accepted-identity", "tokyo-night", "nomatch", 740, 900),
    ("head-turn", "tokyo-night", "turn", 740, 900),
    ("ir-testing", "tokyo-night", "ir", 740, 900),
    ("ir-accepted", "tokyo-night", "ir-pass", 740, 900),
    ("no-face", "tokyo-night", "no-face", 740, 900),
    ("camera-busy", "tokyo-night", "busy", 740, 900),
    ("service-error", "tokyo-night", "error", 740, 900),
    ("weaker-draft", "tokyo-night", "weaker", 740, 900),
    ("save-conflict", "tokyo-night", "conflict", 740, 900),
    ("light-accepted", "catppuccin-latte", "pass", 740, 900),
    ("compact-dark", "tokyo-night", "weaker", 400, 640),
    ("compact-light", "catppuccin-latte", "reject", 400, 640),
)


def main():
    destination = Path(sys.argv[1]).resolve()
    if not destination.is_dir():
        raise SystemExit("Create a new evidence directory first")
    records = []
    for name, theme, scenario, width, height in CASES:
        image = destination / (name + ".png")
        if image.exists():
            raise SystemExit("Refusing to overwrite evidence: " + str(image))
        command = [sys.executable, "-I", str(LAUNCHER), "--offscreen", "--theme", theme,
                   "--scenario", scenario, "--width", str(width),
                   "--height", str(height), "--capture", str(image)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        log = result.stdout + result.stderr
        (destination / (name + ".log")).write_text(log)
        if result.returncode or not image.is_file() or image.stat().st_size < 1000 or "Fixture capture saved:" not in log:
            raise SystemExit("Native capture failed for " + name + "\n" + log)
        records.append({"name": name, "path": str(image), "theme": theme, "scenario": scenario,
                        "width": width, "height": height,
                        "nativeQml": True, "simulatedMeasurements": True,
                        "camera": False, "authentication": False})
        print("Captured " + name, flush=True)
    (destination / "manifest.json").write_text(json.dumps(records, indent=2) + "\n")
    print(f"Captured {len(records)} labelled native states in {destination}")


if __name__ == "__main__":
    main()
