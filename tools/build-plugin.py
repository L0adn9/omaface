#!/usr/bin/env python3
"""Build the io.github.l0adn9.omaface shell plugin from Omarchy's current lock screen
and polkit dialog plus the omaFace patches.

    build-plugin.py <plugin-src-dir> <out-dir>

Fails without writing <out-dir> when any patch anchor is missing or
ambiguous, so an Omarchy update can never leave a half-patched lock screen.
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import patches  # noqa: E402

UPSTREAM = os.environ.get("OMAFACE_UPSTREAM", "/usr/share/omarchy/shell/plugins")
OWN_FILES = ["manifest.json", "FaceGlyph.qml", "Settings.qml", "EnrollRing.qml"]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    problems = []
    built = {}
    hashes = {}

    for upstream_rel, target, file_patches in patches.FILES:
        path = os.path.join(UPSTREAM, upstream_rel)
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except OSError as e:
            problems.append(f"{upstream_rel}: cannot read upstream ({e.strerror})")
            continue
        hashes[upstream_rel] = sha256(path)
        for i, (old, new) in enumerate(file_patches, 1):
            count = text.count(old)
            if count != 1:
                first = old.strip().splitlines()[0][:70]
                problems.append(f"{upstream_rel} patch {i}: anchor found {count}x: {first!r}")
                continue
            text = text.replace(old, new)
        built[target] = text

    for name in OWN_FILES:
        if not os.path.isfile(os.path.join(src, name)):
            problems.append(f"missing plugin source file {name}")

    if problems:
        print("omaface: plugin build failed, nothing was changed:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    parent = os.path.dirname(os.path.abspath(out)) or "."
    os.makedirs(parent, exist_ok=True)
    stage = tempfile.mkdtemp(prefix=".omaface-build.", dir=parent)
    try:
        for target, text in built.items():
            with open(os.path.join(stage, target), "w", encoding="utf-8") as f:
                f.write(text)
        for name in OWN_FILES:
            shutil.copy2(os.path.join(src, name), os.path.join(stage, name))
        with open(os.path.join(stage, "upstream.json"), "w", encoding="utf-8") as f:
            json.dump({"upstream": UPSTREAM, "sha256": hashes}, f, indent=2, sort_keys=True)
            f.write("\n")
        if os.path.exists(out):
            shutil.rmtree(out)
        os.chmod(stage, 0o755)
        os.rename(stage, out)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    print(f"omaface: plugin built in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
