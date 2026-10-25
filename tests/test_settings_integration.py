#!/usr/bin/env python3
"""Exercise production Settings process IO in a camera-free native test host."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NATIVE = Path("/usr/share/omarchy/shell")
VALUES = {
    "threshold": 0.50, "threshold_rgb": 0.55, "match_frames": 2,
    "match_frames_rgb": 8, "liveness_threshold": 0.85, "min_face_width": 70,
    "timeout_test": 8, "timeout_sudo": 4, "timeout_polkit": 5,
    "timeout_lock": 6, "timeout_login": 6, "timeout_choose": 30,
}

STUB = r'''#!/usr/bin/python3 -I
import json, os, pathlib, sys
base = pathlib.Path(__file__).resolve().parent.parent
meta = base / "metadata.json"
mode = (base / "mode").read_text().strip()
with (base / "commands.log").open("a") as stream:
    stream.write(" ".join(sys.argv[1:]) + "\n")
args = sys.argv[1:]
def emit(value):
    print(json.dumps(value), flush=True)
if args[0] == "status":
    emit({"installed":True,"service":True,"cameraKind":"webcam","attention":True,"headTurns":False,"appearances":[{"name":"Fixture","kind":"rgb"}],"maxAppearances":3,"places":{}})
elif args[0] == "advanced":
    print(meta.read_text(), flush=True)
elif args[0] == "apply":
    if mode == "cancel":
        sys.exit(126)
    if mode == "write-failure":
        print("cannot write config", file=sys.stderr)
        sys.exit(1)
    if mode == "conflict":
        obj = json.loads(meta.read_text())
        obj["revision"] = "fixture-changed"
        obj["values"]["threshold_rgb"] = 0.60
        meta.write_text(json.dumps(obj))
        print("stale config revision; reload settings", file=sys.stderr)
        sys.exit(1)
    obj = json.loads(meta.read_text())
    if args[1] != obj["revision"]:
        print("stale config revision; reload settings", file=sys.stderr)
        sys.exit(1)
    for item in args[2:]:
        key,value=item.split("=",1)
        obj["values"][key] = float(value)
    obj["revision"] = "fixture-saved"
    meta.write_text(json.dumps(obj))
    emit(obj)
elif args[0] == "test":
    import signal, time
    values = dict(item.split("=", 1) for item in args[2:])
    def stopped(*_):
        emit({"type": "stopped"})
        sys.exit(0)
    signal.signal(signal.SIGTERM, signal.SIG_IGN if mode == "unclean" else stopped)
    emit({"type": "status", "state": "scanning"})
    emit({"type": "require", "cameraKind": "webcam", "threshold": float(values["threshold_rgb"]),
          "liveThreshold": float(values["liveness_threshold"]), "minWidth": 70,
          "requiredFrames": int(values["match_frames_rgb"]), "timeout": float(values["timeout_test"])})
    emit({"type": "score", "score": 0.72, "live": 0.91, "frames": 1, "requiredFrames": int(values["match_frames_rgb"]), "gate": "pass", "preview": ["x"]})
    if mode in ("hang", "unclean"):
        while True:
            time.sleep(0.1)
    accepted = mode != "reject"
    emit({"type": "result", "result": "ok" if accepted else "fail", "accepted": accepted, "elapsed": 0.5,
          "score": 0.72, "live": 0.91, "longestStreak": 1, "requiredFrames": int(values["match_frames_rgb"]),
          "message": "Recognized" if accepted else "Not recognized"})
    sys.exit(0 if accepted else 1)
else:
    print("Fixture refuses production command", file=sys.stderr)
    sys.exit(90)
'''

QML = r'''import QtQuick
import Quickshell
import Quickshell.Io
import QtTest
import "plugin"

ShellRoot {
  id: harness
  Settings { id: settings }
  Process {
    id: modeProcess
    command: ["@ROOT@/bin/mode", "success"]
  }
  FileView { id: commands; path: "@ROOT@/commands.log"; blockLoading: true }
  TestCase {
    id: tests
    name: "SettingsCalibrationIo"
    property bool ready: false
    Timer { interval: 500; running: true; onTriggered: tests.ready = true }
    when: ready

    function mode(value) {
      modeProcess.command = ["@ROOT@/bin/mode", value]
      modeProcess.running = true
      tryCompare(modeProcess, "running", false, 3000)
    }
    function init() {
      if (settings.draftTestState === "running" || settings.draftTestState === "stopping") {
        settings.stopDraftTest("")
        tryCompare(settings, "draftTestState", "idle", 8000)
      }
      mode("reset")
      settings.advancedLoaded = false
      settings.advancedConflict = false
      settings.advancedApplying = false
      settings.advancedError = ""
      settings.advancedNotice = ""
      settings.draftTestPlace = "lock"
      settings.openAdvanced()
      tryCompare(settings, "advancedLoaded", true, 3000)
      compare(settings.advancedRevision, "fixture-base")
    }
    function cleanup() {
      console.log("Settings IO case " + qtest_results.functionName + ": " + JSON.stringify({test:settings.draftTestState,revision:settings.advancedRevision,notice:settings.advancedNotice,error:settings.advancedError,applying:settings.advancedApplying,conflict:settings.advancedConflict,failures:qtest_results.failCount}))
    }
    function lastCommand() {
      var log = commands.text().trim().split("\n")
      return log[log.length - 1]
    }
    function test_draft_values_are_tested_for_the_chosen_place() {
      settings.editAdvanced("liveness_threshold", 0.70)
      settings.draftTestPlace = "sudo"
      settings.startDraftTest()
      tryCompare(settings, "draftTestState", "done", 3000)
      commands.reload()
      var command = lastCommand()
      verify(command.indexOf("test --json ") === 0, command)
      verify(command.indexOf("liveness_threshold=0.7") > 0, command)
      verify(command.indexOf("threshold_rgb=0.55") > 0, command)
      // The scan lasts as long as sudo's.
      verify(command.indexOf("timeout_test=4") > 0, command)
      verify(command.indexOf("timeout_sudo") < 0, command)
      compare(settings.draftTestRequire.liveThreshold, 0.70)
      compare(settings.draftTestResult.accepted, true)
      compare(settings.advancedSaved.liveness_threshold, 0.85)
      compare(settings.advancedError, "")
    }
    function test_not_accepted_is_a_result_not_an_error() {
      mode("reject")
      settings.startDraftTest()
      tryCompare(settings, "draftTestState", "done", 3000)
      compare(settings.draftTestResult.accepted, false)
      compare(settings.advancedError, "")
    }
    function test_editing_clears_the_result() {
      settings.startDraftTest()
      tryCompare(settings, "draftTestState", "done", 3000)
      settings.editAdvanced("threshold_rgb", 0.6)
      compare(settings.draftTestState, "idle")
      compare(settings.draftTestResult.type, undefined)
    }
    function test_draft_is_locked_while_testing_and_stop_releases() {
      mode("hang")
      settings.startDraftTest()
      tryVerify(function() { return settings.draftTestFrame.type === "score" }, 3000)
      settings.editAdvanced("threshold_rgb", 0.6)
      compare(settings.advancedDraft.threshold_rgb, 0.55)
      settings.stopDraftTest("")
      tryCompare(settings, "draftTestState", "idle", 3000)
      compare(settings.advancedError, "")
    }
    function test_apply_stops_the_test_first() {
      settings.editAdvanced("threshold_rgb", 0.58)
      mode("hang")
      settings.startDraftTest()
      tryVerify(function() { return settings.draftTestFrame.type === "score" }, 3000)
      settings.applyAdvanced()
      tryCompare(settings, "advancedApplying", false, 4000)
      compare(settings.draftTestState, "idle")
      compare(settings.advancedRevision, "fixture-saved")
      compare(settings.advancedSaved.threshold_rgb, 0.58)
      compare(settings.advancedDirty, false)
    }
    function test_unconfirmed_stop_never_applies() {
      settings.editAdvanced("threshold_rgb", 0.58)
      mode("unclean")
      settings.startDraftTest()
      tryVerify(function() { return settings.draftTestFrame.type === "score" }, 3000)
      settings.applyAdvanced()
      tryCompare(settings, "advancedApplying", false, 8000)
      compare(settings.advancedSaved.threshold_rgb, 0.55)
      compare(settings.advancedRevision, "fixture-base")
      verify(settings.advancedError.indexOf("not confirmed") >= 0, settings.advancedError)
    }
    function test_authorization_cancel_retains_draft() {
      settings.editAdvanced("threshold_rgb", 0.58)
      mode("cancel")
      settings.applyAdvanced()
      tryCompare(settings, "advancedApplying", false, 3000)
      compare(settings.advancedDraft.threshold_rgb, 0.58)
      compare(settings.advancedSaved.threshold_rgb, 0.55)
      verify(settings.advancedNotice.indexOf("cancelled") >= 0)
    }
    function test_conflict_retains_draft_until_revert() {
      settings.editAdvanced("threshold_rgb", 0.58)
      mode("conflict")
      settings.applyAdvanced()
      tryCompare(settings, "advancedConflict", true, 3000)
      compare(settings.advancedDraft.threshold_rgb, 0.58)
      compare(settings.advancedSaved.threshold_rgb, 0.60)
      settings.revertAdvancedDraft()
      compare(settings.advancedDraft.threshold_rgb, 0.60)
      compare(settings.advancedConflict, false)
    }
    function test_failed_apply_keeps_error_after_readback() {
      settings.editAdvanced("threshold_rgb", 0.58)
      mode("write-failure")
      settings.applyAdvanced()
      tryCompare(settings, "advancedApplying", false, 3000)
      tryCompare(settings, "advancedLoading", false, 3000)
      compare(settings.advancedError, "cannot write config")
      compare(settings.advancedDraft.threshold_rgb, 0.58)
      compare(settings.advancedSaved.threshold_rgb, 0.55)
    }
    function cleanupTestCase() {
      console.log("Settings process IO tests: " + qtest_results.passCount + " passed, " + qtest_results.failCount + " failed, " + qtest_results.skipCount + " skipped")
    }
  }
}
'''


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("Settings host anchor changed: " + old[:60])
    return source.replace(old, new, 1)


def main():
    with tempfile.TemporaryDirectory(prefix="omaface-settings-io-") as directory:
        scratch = Path(directory)
        plugin = scratch / "plugin"
        shutil.copytree(ROOT / "plugin", plugin)
        for module in ("Commons", "Ui"):
            (scratch / module).symlink_to(NATIVE / module, target_is_directory=True)
        # Only the native window host and authorization launcher are test doubles.
        # All production metadata/stream/draft/Apply controller functions remain intact.
        source = (plugin / "Settings.qml").read_text().replace("@PREFIX@", directory)
        source = source.replace('import Quickshell.Hyprland\n', '')
        source = replace_once(source, '  property var targetScreen: focusedScreen()', '  property var targetScreen: null')
        first = source.index('  function focusedScreen() {')
        last = source.index('\n  function placeDetail', first)
        source = source[:first] + '  function focusedScreen() { return null }\n' + source[last:]
        source = replace_once(source, '  PanelWindow {\n    id: panel\n    screen: root.targetScreen', '  FloatingWindow {\n    id: panel\n    implicitWidth: 1280\n    implicitHeight: 1000')
        for line in (
            '    anchors { top: true; bottom: true; left: true; right: true }\n',
            '    WlrLayershell.namespace: "omaface"\n',
            '    WlrLayershell.layer: WlrLayer.Overlay\n',
            '    WlrLayershell.keyboardFocus: root.authPending ? WlrKeyboardFocus.None : WlrKeyboardFocus.Exclusive\n',
            '    exclusionMode: ExclusionMode.Ignore\n',
        ):
            source = replace_once(source, line, '')
        source = source.replace('["pkexec", "' + directory + '/bin/omaface"]', '["' + directory + '/bin/fixture-auth", "' + directory + '/bin/omaface"]')
        source = source.replace('["pkexec", "' + directory + '/bin/omaface", "enroll", "--ui"]', '["' + directory + '/bin/fixture-auth", "' + directory + '/bin/omaface", "enroll", "--ui"]')
        if '"pkexec"' in source:
            raise RuntimeError("Test host still contains a production authorization launcher")
        (plugin / "Settings.qml").write_text(source)
        bin_dir = scratch / "bin"
        bin_dir.mkdir()
        command = bin_dir / "omaface"
        command.write_text(STUB)
        command.chmod(0o755)
        auth = bin_dir / "fixture-auth"
        auth.write_text('#!/bin/bash\nexec "$@"\n')
        auth.chmod(0o755)
        bounds = {
            key: {"min": low, "max": high, "step": step, "integer": whole}
            for key, low, high, step, whole in (
                ("threshold", .30, .95, .01, False), ("threshold_rgb", .30, .95, .01, False),
                ("match_frames", 1, 5, 1, True), ("match_frames_rgb", 3, 30, 1, True),
                ("liveness_threshold", .50, .99, .01, False), ("min_face_width", 30, 300, 1, True),
                ("timeout_test", 1, 60, 1, False), ("timeout_sudo", 1, 15, 1, False),
                ("timeout_polkit", 1, 15, 1, False), ("timeout_lock", 1, 15, 1, False),
                ("timeout_login", 1, 15, 1, False), ("timeout_choose", 5, 120, 1, False),
            )
        }
        metadata = {"version": 1, "values": VALUES, "defaults": VALUES, "bounds": bounds,
                    "revision": "fixture-base", "attention": True, "headTurns": False}
        (scratch / "baseline.json").write_text(json.dumps(metadata))
        (scratch / "metadata.json").write_text(json.dumps(metadata))
        (scratch / "mode").write_text("success")
        mode = bin_dir / "mode"
        mode.write_text('''#!/usr/bin/python3 -I
import pathlib,sys
root=pathlib.Path(__file__).resolve().parent.parent
if sys.argv[1]=="reset":
    (root/"metadata.json").write_text((root/"baseline.json").read_text())
    (root/"mode").write_text("success")
else: (root/"mode").write_text(sys.argv[1])
''')
        mode.chmod(0o755)
        (scratch / "shell.qml").write_text(QML.replace("@ROOT@", directory))
        home = scratch / "home"
        theme = home / ".local/state/omarchy/current/theme"
        theme.mkdir(parents=True)
        shutil.copy2(Path("/usr/share/omarchy/themes/tokyo-night/colors.toml"), theme / "colors.toml")
        env = {**os.environ, "HOME": str(home), "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_BACKEND": "software",
               "QS_DISABLE_FILE_WATCHER": "1", "QS_NO_RELOAD_POPUP": "1"}
        result = subprocess.run(["qs", "--no-color", "-n", "-p", directory], env=env,
                                capture_output=True, text=True, timeout=90)
        output = result.stdout + result.stderr
        print(output, end="")
        report = re.search(r"Settings process IO tests: (\d+) passed, (\d+) failed, (\d+) skipped", output)
        if result.returncode or not report or int(report[1]) < 9 or int(report[2]) or int(report[3]):
            raise SystemExit(1)
        print("Production Settings controller tested; native host and authorization are isolated fixtures.")


if __name__ == "__main__":
    main()
