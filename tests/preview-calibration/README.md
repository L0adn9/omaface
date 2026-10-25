# Native calibration fixture preview

The harness loads the production `plugin/Calibration.qml` with Omarchy's installed `qs.Commons` and `qs.Ui` modules. Every capture carries **Fixture preview · Simulated measurements · No camera or authentication**. Apply, ordinary Test, and configuration refresh are inert fixture handlers. No production `Settings.qml`, `Service.qml`, PAM, camera, or authorization process is started.

## One capture

From the repository root:

```sh
python3 tests/preview-calibration/preview.py \
  --offscreen --theme tokyo-night --scenario reject --height 900 \
  --capture /tmp/calibration-reject.png

python3 tests/preview-calibration/preview.py \
  --offscreen --theme catppuccin-latte --scenario dirty \
  --section requirements --width 400 --height 640 \
  --capture /tmp/calibration-compact-draft.png
```

Each launch creates a fresh user-owned scratch directory and copies the selected native theme into an isolated HOME. It does not change the desktop theme. `--capture` waits for the native scene, uses `grabToImage()`, saves a PNG, and exits. The destination directory must already exist.

The offscreen Qt backend prints warnings about `WAYLAND_DISPLAY` and unsupported window masks. Those are expected; QML load errors, failed captures, and test failures are not.

## Persistent run and IPC

Omit `--capture` to keep the fixture window running. The launcher prints `CONFIG=/tmp/omaface-calibration-preview-...`. Use that exact path:

```sh
qs ipc --any-display -p "$CONFIG" call calibration scenario progress
qs ipc --any-display -p "$CONFIG" call calibration state
qs ipc --any-display -p "$CONFIG" call calibration scroll requirements
qs ipc --any-display -p "$CONFIG" call calibration size 400 640
qs ipc --any-display -p "$CONFIG" call calibration screenshot /tmp/calibration-draft.png
qs ipc --any-display -p "$CONFIG" call calibration quit
```

`screenshot` returns when capture is queued. `state()` exposes `capturePending` and `lastCapture` for a bounded driver to wait for completion. `size` / `resize` resize the actual **content item and capture**, not the compositor-owned outer window. `state.width` / `height` report capture dimensions; `windowWidth` / `windowHeight` report the outer window separately. For interactive window testing at a different outer size, use a fresh launch with `--width` and `--height`.

Scenarios: `ready`, `reject`, `pass`, `progress`, `ir`, `eyes`, `no-face`, `error`, `busy`, `not-enrolled`, `dirty`, `reset`, `conflict`, `stale`, `cancelled`.

Other calls:

```sh
qs ipc --any-display -p "$CONFIG" call calibration edit liveness_threshold 0.79
qs ipc --any-display -p "$CONFIG" call calibration action defaults
qs ipc --any-display -p "$CONFIG" call calibration action revert
qs ipc --any-display -p "$CONFIG" call calibration action apply
```

`action apply` records an inert attempt and retains the draft; it never saves. Edits to preview keys expose stale measurements rather than recalculating pass/fail in JavaScript. Edits to authentication timeouts leave the sample preview generation alone. Ctrl+1 through Ctrl+9 select the first nine scenarios when the native window is focused.

## Capture the state matrix

```sh
mkdir /tmp/calibration-evidence
python3 -I tests/preview-calibration/capture.py /tmp/calibration-evidence
```

The batch driver records nineteen native states and a JSON manifest, refuses to
replace existing evidence files, and keeps simulated measurements labelled. It
covers both camera modes, passing/rejected/stale states, draft/default/conflict
states, errors, timeouts and compact/light/dark layouts. It opens no camera and
publishes nothing.

## Native interaction tests

```sh
python3 tests/preview-calibration/preview.py --offscreen --theme tokyo-night --tests
python3 tests/preview-calibration/preview.py --offscreen --theme catppuccin-latte --tests
```

`--tests` loads QtTest **inside Quickshell**, attached to the real rendered component. It sends native pointer and keyboard events and checks snapping, Home/End, Tab focus, untouched precision, fixture Apply/defaults/revert, conflicts, IR filtering, stale generations, missing/zero scores, Apply-only timeouts, and pinned score/footer positions. The Python runner requires a nonzero passing count, zero failures/skips, and a bounded completion; Quickshell's own exit code alone is not accepted as proof.

These tests validate the presentation and fixture controller, not production CLI/process IO or camera performance. Production `Settings.qml` integration needs a separate scratch CLI test. Fixture pass states do not establish authentication success, head-turn coverage, or spoof resistance.
