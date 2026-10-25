import QtQuick
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// Presentation only. Settings owns configuration, authorization and the test
// process; the fixture harness uses this same component with inert actions.
//
// Edit the draft, pick where it should work (each place has its own scan
// time), press Test: one real scan with the draft values decides Accepted or
// Not accepted, exactly as that prompt would. Nothing is saved until Apply.
FocusScope {
  id: root

  property var draft: ({})
  property var saved: ({})
  property var metadata: ({})
  // idle | running | stopping | done
  property string testState: "idle"
  property string testPlace: "lock"
  property var require: ({})
  property var frame: ({})
  property var result: ({})
  property string testStatus: ""
  property string cameraKind: ""
  property bool dirty: false
  property bool conflicted: false
  property bool applying: false
  property bool loading: false
  property bool canStart: false
  property bool canApply: false
  property string error: ""
  property string notice: ""
  property string fixtureLabel: ""
  property color foreground: Commons.Color.menu.text
  property color background: Commons.Color.menu.background
  property color accent: Commons.Color.accent
  property string fontFamily: Style.font.menuFamily

  signal valueEdited(string key, real value)
  signal testRequested()
  signal stopRequested()
  signal placeSelected(string place)
  signal applyRequested()
  signal defaultsRequested()
  signal revertRequested()
  signal refreshRequested()
  signal backRequested()

  implicitHeight: layout.implicitHeight

  readonly property bool running: testState === "running" || testState === "stopping"
  readonly property bool hasResult: result.type === "result"
  readonly property bool accepted: hasResult && result.accepted === true
  readonly property bool hasFrame: frame.type === "score"
  readonly property string activeCameraKind: String(require.cameraKind || result.cameraKind || cameraKind || "")
  readonly property bool infrared: activeCameraKind === "ir"
  readonly property string cameraLabel: infrared ? "IR camera" : activeCameraKind === "webcam" ? "RGB webcam" : "Camera"
  readonly property var defaults: metadata.defaults || ({})
  readonly property var bounds: metadata.bounds || ({})
  readonly property var places: [
    { key: "lock", title: "Lock" },
    { key: "sudo", title: "sudo" },
    { key: "polkit", title: "Prompts" },
    { key: "login", title: "Login" }
  ]
  readonly property var visibleRequirementKeys: infrared
    ? ["threshold", "match_frames", "min_face_width"]
    : ["threshold_rgb", "liveness_threshold", "match_frames_rgb", "min_face_width"]
  readonly property var securityKeys: ["threshold", "threshold_rgb", "match_frames", "match_frames_rgb", "liveness_threshold", "min_face_width"]
  readonly property var requirementRows: [
    { key: "threshold", title: "Identity" },
    { key: "threshold_rgb", title: "Identity" },
    { key: "liveness_threshold", title: "Liveness" },
    { key: "match_frames", title: "Frames" },
    { key: "match_frames_rgb", title: "Frames" },
    { key: "min_face_width", title: "Min face" }
  ]
  readonly property var timeoutRows: [
    { key: "timeout_sudo", title: "sudo" },
    { key: "timeout_polkit", title: "Prompts" },
    { key: "timeout_lock", title: "Lock" },
    { key: "timeout_login", title: "Login" },
    { key: "timeout_choose", title: "Choose" }
  ]
  // Only the rows on screen can be fixed, so only they raise the warning.
  readonly property var weakerKeys: visibleRequirementKeys.filter(function(key) { return root.lowered(key) })
  readonly property bool reduced: weakerKeys.length > 0

  // Seconds since this scan started looking (the engine's REQUIRE line).
  property real elapsed: 0
  property real scanStarted: 0
  onRequireChanged: { scanStarted = require.type === "require" ? Date.now() : 0; elapsed = 0 }
  Timer {
    interval: 100
    repeat: true
    running: root.testState === "running" && root.scanStarted > 0
    onTriggered: root.elapsed = (Date.now() - root.scanStarted) / 1000
  }

  // A theme colour marks a state only where it stands out from the page. In a
  // muted theme accent and urgent can be darker than the text and would make
  // the important line look disabled, so the text keeps full contrast there
  // and the symbols (✓ ✗ ⚠) carry the state.
  function luminance(c) {
    function channel(v) { return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4) }
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b)
  }
  function contrast(a, b) {
    var x = luminance(a), y = luminance(b)
    return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
  }
  function emphasis(c) { return contrast(c, background) >= 6 ? c : foreground }
  readonly property color goodColor: emphasis(accent)
  readonly property color badColor: emphasis(Commons.Color.urgent)

  function value(key) { return typeof draft[key] === "number" ? draft[key] : 0 }
  function known(next) { return typeof next === "number" && isFinite(next) }
  function seconds(next) { return known(next) ? next.toFixed(1) + " s" : "" }
  function placeTitle(key) {
    for (var i = 0; i < places.length; i++) if (places[i].key === key) return places[i].title
    return key
  }
  function placeDuration(key) { return value("timeout_" + key) }
  // With attention on, an IR scan needs at least three frames whatever
  // match_frames says (the engine's floor), so compare what scans will use.
  function attentionFloor(key) { return key === "match_frames" && metadata.attention === true ? 3 : 0 }
  function effective(key, next) { return Math.max(next, attentionFloor(key)) }
  function lowered(key) {
    return securityKeys.indexOf(key) >= 0 && typeof draft[key] === "number" && typeof defaults[key] === "number" &&
      effective(key, draft[key]) < effective(key, defaults[key])
  }
  function titleOf(key) {
    for (var i = 0; i < requirementRows.length; i++) if (requirementRows[i].key === key) return requirementRows[i].title
    return key
  }

  // During a scan the checks show the current frame; afterwards, the best
  // reached, against what this scan required.
  readonly property var checks: {
    var during = running
    var rows = [{ title: "Identity", current: during ? frame.score : result.score, required: require.threshold, digits: 3 }]
    if (!infrared)
      rows.push({ title: "Liveness", current: during ? frame.live : result.live, required: require.liveThreshold, digits: 3 })
    // A streak still building is progress, not a failure.
    rows.push({ title: "Streak", current: during ? frame.frames : result.longestStreak, required: require.requiredFrames, digits: 0, progress: during })
    if (during) rows.push({ title: "Face size", current: frame.width, required: require.minWidth, digits: 0 })
    return rows
  }

  function formatted(next, digits) {
    if (!known(next)) return "—"
    return digits ? next.toFixed(digits) : String(Math.round(next))
  }
  function checkText(row) {
    var current = formatted(row.current, row.digits), required = formatted(row.required, row.digits)
    if (!known(row.current) || !known(row.required) || (row.progress && row.current < row.required)) return current + " / " + required
    return current + (row.current >= row.required ? " ≥ " : " < ") + required
  }
  function checkMark(row) {
    if (!known(row.current) || !known(row.required)) return ""
    if (row.current >= row.required) return "✓"
    return row.progress ? "" : "✗"
  }

  function gateText() {
    var moves = {
      "turn-left": "← Turn your head left",
      "turn-right": "Turn your head right →",
      "turn-back": "Face the camera again"
    }
    if (moves[testStatus]) return moves[testStatus]
    if (!hasFrame) return testStatus === "scanning" ? "Look at the screen" : "Starting the camera…"
    var reasons = {
      "noface": "No face in view",
      "small": "Move closer",
      "nomatch": "Identity below requirement",
      "attention": "Eyes closed or not on the screen",
      "notlive": "Liveness below requirement",
      "models": "Models unavailable",
      "not-enrolled": "Set up omaFace first",
      "pass": "Requirements met · building the streak"
    }
    return reasons[frame.gate] || "Look at the screen"
  }

  readonly property string bannerTitle: testState === "stopping" ? "Stopping…"
    : running ? "Testing · " + elapsed.toFixed(1) + " / " + placeDuration(testPlace) + " s"
    : accepted ? "✓ Accepted"
    : hasResult ? "✗ Not accepted"
    : "Test the draft"
  readonly property string bannerDetail: running ? gateText()
    : accepted ? "As " + placeTitle(testPlace) + " would" + (known(result.elapsed) ? " · in " + seconds(result.elapsed) : "")
    : hasResult ? String(result.message || "Not recognized")
    : "Pick where it should work, then Test. Nothing is saved."
  readonly property color bannerColor: accepted ? goodColor : hasResult ? badColor : foreground

  // The ring fills with the streak: all 24 segments at the required frames.
  function ringTicks() {
    var required = known(require.requiredFrames) ? Math.max(1, require.requiredFrames) : 0
    var frames = running ? frame.frames : result.longestStreak
    var lit = required && known(frames) ? Math.round(Math.min(1, frames / required) * 24) : 0
    var ticks = []
    for (var i = 0; i < 24; i++) ticks.push(i < lit ? 1 : 0)
    return ticks
  }

  // Kept for the fixture harness and keyboard tests; the compact page has no
  // scroll area.
  function ensureVisible(item) { return }
  function scrollTo(section) { return }

  Keys.onEscapePressed: function(event) { backRequested(); event.accepted = true }

  Column {
    id: layout
    width: parent.width
    spacing: Style.space(6)

    Item {
      width: parent.width
      height: Math.max(backButton.implicitHeight, titleColumn.implicitHeight)

      Button {
        id: backButton
        objectName: "calibration_back"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        text: "Back"
        bordered: true
        focusable: true
        foreground: root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        enabled: !root.applying
        onClicked: root.backRequested()
      }

      Column {
        id: titleColumn
        anchors.left: backButton.right
        anchors.leftMargin: Style.space(12)
        anchors.right: testButton.left
        anchors.rightMargin: Style.space(12)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Style.space(1)

        Text {
          width: parent.width
          text: "Advanced calibration"
          textFormat: Text.PlainText
          elide: Text.ElideRight
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.heading
          font.bold: true
        }
        Text {
          width: parent.width
          text: root.cameraLabel + (root.running ? " · testing" : "")
          textFormat: Text.PlainText
          elide: Text.ElideRight
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.caption
        }
      }

      Button {
        id: testButton
        objectName: "calibration_test"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: root.running ? "Stop" : "Test"
        bordered: true
        focusable: true
        foreground: root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        enabled: root.testState !== "stopping" && !root.applying && (root.running || root.canStart)
        onClicked: root.running ? root.stopRequested() : root.testRequested()
      }
    }

    Item {
      width: parent.width
      height: (fixtureLabel.visible || draftLabel.visible) ? Math.max(fixtureLabel.implicitHeight, draftLabel.implicitHeight) : 0

      Text {
        id: fixtureLabel
        objectName: "calibration_fixture_label"
        anchors.left: parent.left
        anchors.right: draftLabel.left
        anchors.rightMargin: Style.space(8)
        visible: root.fixtureLabel !== ""
        text: root.fixtureLabel
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        font.bold: true
      }

      Text {
        id: draftLabel
        objectName: "calibration_draft_label"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        visible: root.fixtureLabel !== "" || root.dirty || root.conflicted
        text: root.dirty ? "Draft" : "Saved"
        textFormat: Text.PlainText
        color: root.foreground
        font.family: root.fontFamily
        font.pixelSize: Style.font.caption
        font.bold: true
      }
    }

    PanelSeparator { foreground: root.foreground }

    Row {
      id: bodyRow
      width: parent.width
      spacing: Style.space(14)

      Column {
        id: livePanel
        width: root.width >= Style.space(620) ? Style.space(200) : Style.space(150)
        spacing: Style.space(6)

        EnrollRing {
          width: parent.width * 0.86
          height: width
          anchors.horizontalCenter: parent.horizontalCenter
          filled: root.ringTicks()
          preview: Array.isArray(root.frame.preview) ? root.frame.preview : []
          done: root.accepted
          color: root.foreground
          accent: root.accent
          fontFamily: root.fontFamily
        }

        // The answer: as large as the page allows, coloured by outcome.
        BorderSurface {
          objectName: "calibration_result"
          width: parent.width
          height: bannerColumn.implicitHeight + Style.space(8) * 2
          radius: Style.cornerRadius
          color: Qt.rgba(root.bannerColor.r, root.bannerColor.g, root.bannerColor.b, root.hasResult ? 0.16 : 0.06)
          borderSpec: Border.flat(Qt.rgba(root.bannerColor.r, root.bannerColor.g, root.bannerColor.b, root.hasResult ? 0.9 : 0.3),
                                  Math.max(1, Style.space(root.hasResult ? 2 : 1)))

          Column {
            id: bannerColumn
            x: Style.space(8)
            y: Style.space(8)
            width: parent.width - x * 2
            spacing: Style.space(2)

            Text {
              objectName: "calibration_result_title"
              width: parent.width
              text: root.bannerTitle
              textFormat: Text.PlainText
              wrapMode: Text.WordWrap
              color: root.bannerColor
              font.family: root.fontFamily
              font.pixelSize: root.hasResult ? Style.font.heading * 1.25 : Style.font.body
              font.bold: true
            }
            Text {
              objectName: "calibration_result_detail"
              width: parent.width
              text: root.bannerDetail
              textFormat: Text.PlainText
              wrapMode: Text.WordWrap
              color: root.foreground
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }
        }

        Column {
          objectName: "calibration_checks"
          width: parent.width
          spacing: Style.space(2)
          visible: root.running || root.hasResult

          Text {
            width: parent.width
            visible: root.hasResult && !root.running
            text: "Best reached"
            textFormat: Text.PlainText
            color: root.foreground
            opacity: 0.7
            font.family: root.fontFamily
            font.pixelSize: Style.font.caption
          }

          Repeater {
            model: root.checks
            Item {
              required property var modelData
              width: parent.width
              height: Math.max(checkLabel.implicitHeight, checkValue.implicitHeight)
              readonly property string mark: root.checkMark(modelData)
              readonly property color tone: mark === "✗" ? root.badColor : mark === "✓" ? root.goodColor : root.foreground

              Text {
                id: checkLabel
                anchors.left: parent.left
                text: (parent.mark || "·") + " " + parent.modelData.title
                textFormat: Text.PlainText
                color: parent.tone
                font.family: root.fontFamily
                font.pixelSize: Style.font.bodySmall
                font.bold: parent.mark === "✗"
              }
              Text {
                id: checkValue
                anchors.right: parent.right
                text: root.checkText(parent.modelData)
                textFormat: Text.PlainText
                color: parent.tone
                font.family: root.fontFamily
                font.pixelSize: Style.font.bodySmall
              }
            }
          }
        }
      }

      Column {
        width: bodyRow.width - livePanel.width - bodyRow.spacing
        spacing: Style.space(4)

        Text {
          width: parent.width
          text: "Test as"
          textFormat: Text.PlainText
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          font.bold: true
        }

        Row {
          objectName: "calibration_places"
          spacing: Style.space(4)
          Repeater {
            model: root.places
            Button {
              required property var modelData
              objectName: "calibration_place_" + modelData.key
              text: modelData.title + " " + root.placeDuration(modelData.key) + " s"
              bordered: true
              focusable: true
              selected: root.testPlace === modelData.key
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              fontSize: Style.font.caption
              enabled: !root.running && !root.applying
              onClicked: root.placeSelected(modelData.key)
            }
          }
        }

        Item { width: 1; height: Style.space(2) }

        Text {
          width: parent.width
          text: "Requirements"
          textFormat: Text.PlainText
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          font.bold: true
        }

        Grid {
          id: requirementsGrid
          width: parent.width
          columns: 2
          columnSpacing: Style.space(10)
          rowSpacing: Style.space(2)
          height: childrenRect.height
          property real cellWidth: (width - columnSpacing) / 2

          Repeater {
            model: root.requirementRows.filter(function(row) {
              return root.visibleRequirementKeys.indexOf(row.key) >= 0 && root.bounds[row.key] !== undefined
            })
            CalibrationNumeric {
              required property var modelData
              width: requirementsGrid.cellWidth
              settingKey: modelData.key
              title: modelData.title
              value: root.value(modelData.key)
              defaultValue: Number(root.defaults[modelData.key])
              bounds: root.bounds[modelData.key]
              reducedSecurity: root.lowered(modelData.key)
              warnColor: root.badColor
              note: root.attentionFloor(modelData.key) > value ? "Attention on: scans use at least " + root.attentionFloor(modelData.key) : ""
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              enabled: !root.applying && !root.loading && !root.running
              onEdited: function(next) { root.valueEdited(modelData.key, next) }
              onFocused: function(item) { root.ensureVisible(item) }
            }
          }
        }

        Text {
          width: parent.width
          text: "Scan time (s)"
          textFormat: Text.PlainText
          color: root.foreground
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          font.bold: true
        }

        Grid {
          id: timeoutsGrid
          width: parent.width
          columns: 2
          columnSpacing: Style.space(10)
          rowSpacing: Style.space(2)
          height: childrenRect.height
          property real cellWidth: (width - columnSpacing) / 2

          Repeater {
            model: root.timeoutRows.filter(function(row) { return root.bounds[row.key] !== undefined })
            CalibrationNumeric {
              required property var modelData
              width: timeoutsGrid.cellWidth
              settingKey: modelData.key
              title: modelData.title
              value: root.value(modelData.key)
              defaultValue: Number(root.defaults[modelData.key])
              bounds: root.bounds[modelData.key]
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              enabled: !root.applying && !root.loading && !root.running
              onEdited: function(next) { root.valueEdited(modelData.key, next) }
              onFocused: function(item) { root.ensureVisible(item) }
            }
          }
        }
      }
    }

    PanelSeparator { foreground: root.foreground }

    // Lower requirements are allowed, but never quietly.
    BorderSurface {
      objectName: "calibration_weaker_warning"
      width: parent.width
      visible: root.reduced
      height: visible ? warningText.implicitHeight + Style.space(6) * 2 : 0
      radius: Style.cornerRadius
      color: Qt.rgba(root.badColor.r, root.badColor.g, root.badColor.b, 0.12)
      borderSpec: Border.flat(root.badColor, Math.max(1, Style.space(2)))

      Text {
        id: warningText
        x: Style.space(8)
        y: Style.space(6)
        width: parent.width - x * 2
        text: "⚠ Weaker than the defaults (" + root.weakerKeys.map(root.titleOf).join(", ") + "). "
          + "Another person, or a photo or screen of you, may be accepted. "
          + "A test that accepts you does not show these settings are safe."
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: root.badColor
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
        font.bold: true
      }
    }

    Text {
      objectName: "calibration_notice"
      width: parent.width
      visible: text !== ""
      text: root.error !== "" ? root.error
        : root.applying ? "Applying…"
        : root.conflicted ? "Saved settings changed elsewhere. Revert to load them; Apply is off until then."
        : root.notice !== "" ? root.notice
        : root.dirty ? "Draft not applied · Authentication is unchanged"
        : ""
      textFormat: Text.PlainText
      wrapMode: Text.WordWrap
      color: root.error !== "" ? root.badColor : root.foreground
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
      font.bold: root.error !== "" || root.conflicted
    }

    Row {
      width: parent.width
      spacing: Style.space(6)

      Button {
        objectName: "calibration_defaults"
        text: "Reset"
        bordered: true
        focusable: true
        foreground: root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        fontSize: Style.font.bodySmall
        enabled: !root.applying && !root.loading && !root.running && root.metadata.version === 1
        onClicked: root.defaultsRequested()
      }
      Button {
        objectName: "calibration_revert"
        text: "Revert"
        bordered: true
        focusable: true
        foreground: root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        fontSize: Style.font.bodySmall
        enabled: !root.applying && !root.running && (root.dirty || root.conflicted)
        onClicked: root.revertRequested()
      }
      Button {
        objectName: "calibration_refresh"
        text: "Refresh"
        bordered: true
        focusable: true
        foreground: root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        fontSize: Style.font.bodySmall
        enabled: !root.applying && !root.loading && !root.running
        onClicked: root.refreshRequested()
      }
      Button {
        objectName: "calibration_apply"
        text: root.applying ? "Applying…" : (root.reduced ? "Apply weaker settings" : "Apply")
        bordered: true
        focusable: true
        foreground: root.reduced ? root.badColor : root.foreground
        accent: root.accent
        fontFamily: root.fontFamily
        fontSize: Style.font.bodySmall
        enabled: root.canApply && root.dirty && !root.applying && !root.conflicted && !root.loading
        onClicked: root.applyRequested()
      }
    }
  }
}
