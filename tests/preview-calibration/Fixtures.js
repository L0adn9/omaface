.pragma library

var testableKeys = ["threshold", "threshold_rgb", "match_frames", "match_frames_rgb", "liveness_threshold", "min_face_width"]
var scenarios = ["ready", "running", "streak", "pass", "reject", "nomatch", "turn", "ir", "ir-pass", "no-face", "error", "busy", "weaker", "conflict", "stopping"]

function copy(value) { return JSON.parse(JSON.stringify(value)) }

function metadata() {
  var values = {
    threshold: 0.50, threshold_rgb: 0.55, match_frames: 2, match_frames_rgb: 8,
    liveness_threshold: 0.85, min_face_width: 70, timeout_test: 8,
    timeout_sudo: 4, timeout_polkit: 5, timeout_lock: 6, timeout_login: 6, timeout_choose: 30
  }
  return {
    version: 1, values: copy(values), defaults: copy(values), revision: "fixture-revision-1",
    attention: true, headTurns: false,
    bounds: {
      threshold: { min: 0.30, max: 0.95, step: 0.01, integer: false },
      threshold_rgb: { min: 0.30, max: 0.95, step: 0.01, integer: false },
      match_frames: { min: 1, max: 5, step: 1, integer: true },
      match_frames_rgb: { min: 3, max: 30, step: 1, integer: true },
      liveness_threshold: { min: 0.50, max: 0.99, step: 0.01, integer: false },
      min_face_width: { min: 30, max: 300, step: 1, integer: true },
      timeout_test: { min: 1, max: 60, step: 1, integer: true },
      timeout_sudo: { min: 1, max: 15, step: 1, integer: true },
      timeout_polkit: { min: 1, max: 15, step: 1, integer: true },
      timeout_lock: { min: 1, max: 15, step: 1, integer: true },
      timeout_login: { min: 1, max: 15, step: 1, integer: true },
      timeout_choose: { min: 5, max: 120, step: 1, integer: true }
    }
  }
}

// A braille stand-in for the camera: an oval face inside the ring.
function art() {
  var rows = []
  for (var r = 0; r < 12; r++) {
    var row = ""
    for (var c = 0; c < 30; c++) {
      var dx = (c - 14.5) / 9, dy = (r - 5.5) / 5.5
      var d = dx * dx + dy * dy
      row += String.fromCharCode(0x2800 + (d < 0.75 ? 0xff : d < 1 ? 0x24 : 0))
    }
    rows.push(row)
  }
  return rows
}

function require(ir) {
  return ir
    ? { type: "require", cameraKind: "ir", threshold: 0.50, liveThreshold: null, minWidth: 70, requiredFrames: 3, timeout: 6 }
    : { type: "require", cameraKind: "webcam", threshold: 0.55, liveThreshold: 0.85, minWidth: 70, requiredFrames: 8, timeout: 6 }
}

function frame(fields) {
  var base = {
    type: "score", score: 0.706, attention: "ok", live: 0.641, width: 118, liveV2: 0.698, liveV1SE: 0.584,
    frames: 0, requiredFrames: 8, gate: "notlive", preview: art()
  }
  for (var key in fields) base[key] = fields[key]
  return base
}

function result(fields) {
  var base = {
    type: "result", result: "fail", detail: "liveness spoof", accepted: false, elapsed: null,
    score: 0.731, live: 0.712, longestStreak: 0, requiredFrames: 8, cameraKind: "webcam",
    message: "Face matched, but liveness could not be verified"
  }
  for (var key in fields) base[key] = fields[key]
  return base
}

function scenario(name) {
  var m = metadata()
  var out = {
    name: name, metadata: m, saved: copy(m.values), draft: copy(m.values), cameraKind: "webcam",
    testState: "idle", testPlace: "lock", require: {}, frame: {}, result: {}, testStatus: "",
    error: "", notice: "", conflicted: false
  }
  if (name === "running") {
    out.testState = "running"; out.require = require(false); out.frame = frame({}); out.testStatus = "notlive"
  } else if (name === "streak") {
    out.testState = "running"; out.require = require(false)
    out.frame = frame({ live: 0.912, liveV2: 0.923, liveV1SE: 0.901, frames: 5, gate: "pass" })
  } else if (name === "pass") {
    out.testState = "done"; out.require = require(false)
    out.frame = frame({ live: 0.928, frames: 8, gate: "pass" })
    out.result = result({ result: "ok", detail: "", accepted: true, elapsed: 1.42, score: 0.744, live: 0.951, longestStreak: 8, message: "Recognized" })
  } else if (name === "reject") {
    out.testState = "done"; out.require = require(false); out.frame = frame({})
    out.result = result({ longestStreak: 2 })
  } else if (name === "nomatch") {
    out.testState = "done"; out.require = require(false); out.frame = frame({ score: 0.41, gate: "nomatch" })
    out.result = result({ detail: "", score: 0.48, live: 0.93, message: "Not recognized" })
  } else if (name === "turn") {
    out.metadata.headTurns = true
    out.testState = "running"; out.require = require(false); out.testStatus = "turn-left"
    out.frame = frame({ live: 0.93, frames: 8, gate: "pass" })
  } else if (name === "ir") {
    out.cameraKind = "ir"; out.testState = "running"; out.require = require(true)
    out.frame = frame({ score: 0.66, live: null, liveV2: null, liveV1SE: null, frames: 2, requiredFrames: 3, gate: "pass" })
  } else if (name === "ir-pass") {
    out.cameraKind = "ir"; out.testState = "done"; out.require = require(true)
    out.result = result({ result: "ok", detail: "", accepted: true, elapsed: 0.61, score: 0.69, live: null, longestStreak: 3, requiredFrames: 3, cameraKind: "ir", message: "Recognized" })
  } else if (name === "no-face") {
    out.testState = "done"; out.require = require(false)
    out.frame = frame({ score: null, live: null, liveV2: null, liveV1SE: null, width: null, gate: "noface", attention: "unavailable" })
    out.result = result({ result: "timeout", detail: "", score: null, live: null, message: "No face found" })
  } else if (name === "error") {
    out.error = "Simulated: cannot reach the omaFace service. No camera was opened."
  } else if (name === "busy") {
    out.testState = "done"
    out.result = result({ result: "busy", detail: "", score: null, live: null, message: "Camera busy" })
  } else if (name === "weaker") {
    out.draft.liveness_threshold = 0.70; out.draft.match_frames_rgb = 4
    out.notice = "Unsaved sample draft · Authentication is unchanged"
  } else if (name === "conflict") {
    out.draft.liveness_threshold = 0.80; out.saved.threshold_rgb = 0.58
    out.metadata.values = copy(out.saved); out.metadata.revision = "fixture-revision-2"
    out.conflicted = true
  } else if (name === "stopping") {
    out.testState = "stopping"; out.require = require(false); out.frame = frame({})
  }
  return out
}
