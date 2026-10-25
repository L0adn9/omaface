# Calibration verification

All unattended checks use disposable directories and temporary Unix sockets. They
never authorize a system save or turn on a camera.

## Engine, client, CLI and Settings

```bash
python3 -I tests/test_calibration_engine.py -v
python3 -I tests/test_test_client.py -v
python3 -I tests/test_calibration_cli.py -v
python3 -I tests/test_settings_integration.py
```

The engine suite builds the engine and PAM helper from source in scratch (never
the in-tree binaries, which point at the real `/etc` and `/run`), compiles a
camera-free C++ core driver, and runs the synthetic `selftest` against installed
models (skipped when there are none). It covers default and boundary gates, strict
finite values, `TEST` request parsing (only testable keys, only on `TEST`, only for
yourself), stale config revisions, comment preservation and atomic saves against
temporary files. Only the core driver uses `OMAFACE_TEST_SETTINGS_ROOT`; neither
production nor `OMAFACE_DEV` builds bypass Apply's root check.

The client suite runs `tools/test-client.py` against a scripted engine socket: the
request it sends, the JSON it reports, the result summary, refusing invalid values
before connecting, and Stop waiting for the engine to let go of the camera. The CLI
suite renders an installed-shape `omaface` in a temporary prefix with an inert
engine, client and authorization stubs. The Settings suite runs the production
controller in a temporary native window host with a fixture CLI: the draft values
and scan time sent to `omaface test`, results, locking the draft during a test,
Apply stopping the test first, refusing Apply when the camera release is not
confirmed, cancelled authorization, save conflicts and readback.

## Native UI

The preview harness uses the same `plugin/Calibration.qml` as Settings, with inert
actions and a permanent fixture label. It cannot authenticate or save settings.

```bash
python3 -I tests/preview-calibration/preview.py --offscreen --theme tokyo-night --tests
python3 -I tests/preview-calibration/preview.py --offscreen --theme catppuccin-latte \
  --scenario reject --width 400 --height 640 --capture /tmp/calibration-compact.png
```

The fixture launcher prints its isolated Quickshell config and HOME. Its IPC target
is `calibration`; select states and capture with `qs ipc -p <config> call calibration
scenario <name>` and `... screenshot /absolute/path.png`. Never use `omarchy-shell`
for fixture control: it targets the live shell. `capture.py <new directory>` saves
every labelled state.

A fixture screenshot proves presentation and interaction, not real-camera
acceptance or photo/video rejection. No reduced setting is certified safe by a test
that accepts its owner.

## Integration checks

Build a candidate plugin into a **new** scratch output directory:

```bash
python3 -I tools/build-plugin.py --prefix /tmp/candidate-prefix plugin /tmp/new-plugin-output
omarchy plugin validate /tmp/new-plugin-output
bash -n bin/omaface system/install-files.sh system/install-root.sh
```

Manifest validation is not a QML typecheck; the native harness must also load and
be exercised. Do not use `install.sh`, `omaface sync` or root-owned settings for
unattended verification. Keep normal PAM/service integration untouched.
