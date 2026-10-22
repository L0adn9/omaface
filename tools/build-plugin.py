#!/usr/bin/env python3
"""Build the io.github.l0adn9.omaface shell plugin from Omarchy's current lock screen
and polkit dialog plus the omaFace patches.

    build-plugin.py --prefix <prefix> <plugin-src-dir> <out-dir>

<prefix> is where omaFace is installed (/usr for the package, /usr/local
for the git install); the plugin calls <prefix>/bin/omaface.

Omarchy 4.0 and 4.5 need different anchors; the file set whose anchors all
match the installed Omarchy is used. Fails without writing <out-dir> when no
set matches or a patch anchor is missing or ambiguous, so an Omarchy update
can never leave a half-patched lock screen.
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
OWN_FILES = ["manifest.json", "FaceGlyph.qml", "Settings.qml", "EnrollRing.qml", "SettingRow.qml", "AppearanceRow.qml"]


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def patch_files(files):
    """Apply one anchor set; return (problems, built files, upstream hashes)."""
    problems = []
    built = {}
    hashes = {}
    for upstream_rel, target, file_patches in files:
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
    return problems, built, hashes


def main():
    args = sys.argv[1:]
    if len(args) != 4 or args[0] != "--prefix" or not os.path.isabs(args[1]):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    prefix, src, out = args[1].rstrip("/") or "/", args[2], args[3]

    own = {}
    own_text = {}
    problems = []
    for name in OWN_FILES:
        path = os.path.join(src, name)
        if not os.path.isfile(path):
            problems.append(f"missing plugin source file {name}")
            continue
        own[name] = sha256(path)
        with open(path, encoding="utf-8") as f:
            own_text[name] = f.read()
    if problems:
        print("omaface: plugin build failed, nothing was changed:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    selected = None
    failures = []
    for label, files in patches.FILE_SETS:
        problems, built, hashes = patch_files(files)
        if not problems:
            selected = (label, built, hashes)
            break
        failures.append((label, problems))

    if selected is None:
        print("omaface: plugin build failed, nothing was changed:", file=sys.stderr)
        print(f"  no omaFace patch set matches the shell sources in {UPSTREAM}", file=sys.stderr)
        for label, problems in failures:
            print(f"  Omarchy {label} anchors:", file=sys.stderr)
            for p in problems:
                print(f"    - {p}", file=sys.stderr)
        return 1

    label, built, hashes = selected
    built.update(own_text)

    parent = os.path.dirname(os.path.abspath(out)) or "."
    os.makedirs(parent, exist_ok=True)
    stage = tempfile.mkdtemp(prefix=".omaface-build.", dir=parent)
    try:
        for target, text in built.items():
            with open(os.path.join(stage, target), "w", encoding="utf-8") as f:
                f.write(text.replace("@PREFIX@", prefix))
        # What it was built from: Omarchy's files, and omaFace's own (an
        # upgrade of omaFace changes these, an Omarchy update the others).
        with open(os.path.join(stage, "upstream.json"), "w", encoding="utf-8") as f:
            json.dump({"upstream": UPSTREAM, "sha256": hashes, "source": os.path.abspath(src), "own": own},
                      f, indent=2, sort_keys=True)
            f.write("\n")
        if os.path.exists(out):
            shutil.rmtree(out)
        os.chmod(stage, 0o755)
        os.rename(stage, out)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    print(f"omaface: plugin built in {out} (Omarchy {label} lock screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
