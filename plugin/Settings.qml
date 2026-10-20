import QtQuick
import Quickshell
import Quickshell.Hyprland
import Quickshell.Io
import Quickshell.Wayland
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// omaFace settings. Created on demand by Service.qml (omarchy-shell omaface
// settings) and destroyed again when closed, so it holds no memory at rest.
// Every change goes through `pkexec omaface ...`: the shell itself never
// writes the root-owned configuration.
Item {
  id: root

  signal closed()

  property string view: "main"
  property var status: ({})
  property bool loaded: false
  property string message: ""
  property bool messageIsError: false

  property var enrollFilled: []
  property var enrollPreview: []
  property string enrollHint: ""
  property string enrollPhase: "idle"

  property string testState: "idle"
  property string testText: ""
  // Set while the window waits for the camera to be released before it goes.
  property bool closing: false
  // Index of the appearance whose name is being edited, or -1.
  property int renaming: -1

  property string fontFamily: Style.font.menuFamily
  property color background: Commons.Color.menu.background
  property color foreground: Commons.Color.menu.text
  property color accent: Commons.Color.accent
  property color errorColor: Commons.Color.urgent
  property var borderSpec: Border.surfaceSpec("menu", "border", Commons.Color.menu.border, Math.max(1, Style.space(2)))
  readonly property int pad: Style.spacing.panelPadding
  readonly property int cardWidth: Math.min(Style.space(380), panel.width - Style.gapsOut * 2)
  // Opened from the keyboard or the launcher, so it belongs on the monitor
  // Hyprland has focused (as Omarchy's own panels do); picked once, so it
  // does not follow focus around while it is open.
  property var targetScreen: focusedScreen()

  readonly property var appearances: Array.isArray(status.appearances) ? status.appearances : []
  readonly property int maxAppearances: status.maxAppearances || 3
  // No IR camera found: the regular webcam, which also checks every scan for
  // a photo or a screen (and, if asked for, random head turns).
  readonly property bool webcam: status.cameraKind === "webcam"
  readonly property var places: status.places || ({})
  readonly property bool authPending: actionProc.running || (enrollProc.running && enrollPhase === "auth")
  // Login is only offered where the installer found SDDM as the login manager.
  readonly property var visiblePlaceRows: placeRows.filter(function(row) {
    return row.key !== "login" || status.loginSupported !== false
  })
  // Each place, and what using it is like with the IR camera or a webcam.
  readonly property var placeRows: [
    { key: "lock", label: "Lock screen", ir: "When you wake the screen", webcam: "Wake the screen, then a few head turns" },
    { key: "login", label: "Login screen", ir: "At boot; Enter on the empty field scans again", webcam: "At boot, with a few head turns" },
    { key: "sudo", label: "sudo in terminals", ir: "Press Enter at the prompt to use your face", webcam: "Press Enter, then a few head turns" },
    { key: "polkit", label: "System prompts", ir: "Click or press Enter to use your face", webcam: "Click or press Enter, then a few head turns" }
  ]

  function focusedScreen() {
    var monitor = Hyprland.focusedMonitor
    var name = monitor ? String(monitor.name || "") : ""
    for (var i = 0; i < Quickshell.screens.length; i++)
      if (Quickshell.screens[i].name === name) return Quickshell.screens[i]
    return Quickshell.screens[0]
  }

  function placeDetail(row) {
    if (row.key === "login" && status.autologin === true) return "Skipped: SDDM signs you in automatically"
    return webcam && status.headTurns === true ? row.webcam : row.ir
  }

  function refresh() {
    if (!statusProc.running) statusProc.running = true
  }

  function say(text, isError) {
    message = text
    messageIsError = !!isError
    messageTimer.restart()
  }

  function run(args, okText) {
    if (actionProc.running || enrollProc.running) return
    actionProc.okText = okText || ""
    actionProc.command = ["pkexec", "@PREFIX@/bin/omaface"].concat(args)
    actionProc.running = true
  }

  function startEnroll() {
    if (actionProc.running || enrollProc.running) return
    enrollFilled = []
    enrollPreview = []
    enrollHint = "Waiting for authorization…"
    enrollPhase = "auth"
    view = "enroll"
    var args = ["pkexec", "@PREFIX@/bin/omaface", "enroll", "--ui"]
    if (appearances.length > 0) args = args.concat(["--name", "Appearance " + (appearances.length + 1)])
    enrollProc.command = args
    enrollProc.stdinEnabled = true
    enrollProc.running = true
  }

  // Enrollment runs as root (pkexec), so a signal from the shell cannot stop
  // it; closing its stdin can, and the engine turns the camera off at once.
  function stopEnroll() {
    if (!enrollProc.running) return
    enrollProc.stdinEnabled = false
    enrollProc.running = false
  }

  function startRename(index) {
    if (actionProc.running) return
    renaming = index
  }

  function finishRename(index, text) {
    var name = String(text || "").trim()
    var old = appearances[index] ? String(appearances[index].name || "") : ""
    renaming = -1
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    if (name === "" || name === old) return
    run(["rename", String(index + 1), name], "Renamed to " + name)
  }

  function cancelRename() {
    renaming = -1
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }

  function startTest() {
    if (testProc.running) return
    testState = "scanning"
    testText = "Look at the screen…"
    view = "test"
    testProc.running = true
  }

  function back() {
    if (view === "enroll" && enrollProc.running) {
      stopEnroll()
      enrollPhase = "idle"
    }
    if (view === "test" && testProc.running) testProc.running = false
    if (view !== "main") {
      view = "main"
      refresh()
      Qt.callLater(function() { keyCatcher.forceActiveFocus() })
    } else {
      dismiss()
    }
  }

  function dismiss() {
    if (testProc.running) testProc.running = false
    if (enrollProc.running) {
      // Hide now, but stay alive until the engine has let go of the camera.
      closing = true
      enrollPhase = "idle"
      stopEnroll()
      closeFallback.restart()
      return
    }
    closed()
  }

  function handleEnrollLine(line) {
    var ev
    try { ev = JSON.parse(line) } catch (e) { return }
    if (ev.type === "start") {
      enrollPhase = "running"
      enrollHint = "Look at the screen, then slowly move your head in a circle"
    } else if (ev.type === "frame") {
      enrollPhase = "running"
      enrollFilled = ev.filled || []
      enrollPreview = ev.preview || []
      enrollHint = ev.hint || ""
    } else if (ev.type === "done") {
      enrollPhase = "done"
      enrollHint = ev.eyes === false
        ? "omaFace is set up. Your eyes were hard to see (glare?); if it will not unlock, turn off Require attention"
        : "omaFace is set up"
      refresh()
    } else if (ev.type === "error") {
      enrollPhase = "error"
      enrollHint = ev.message || "Enrollment failed"
    }
  }

  function handleTestLine(line) {
    var ev
    try { ev = JSON.parse(line) } catch (e) { return }
    if (ev.type === "status") {
      if (ev.state === "nomatch") testText = "Not recognized yet…"
      else if (ev.state === "noface") testText = "Looking for a face…"
      else if (ev.state === "face") testText = "Recognizing…"
      else if (ev.state === "turn-left") testText = "\u2190  Turn your head left"
      else if (ev.state === "turn-right") testText = "Turn your head right  \u2192"
      else if (ev.state === "turn-back") testText = "Face the camera again"
    } else if (ev.type === "result") {
      if (ev.result === "ok") {
        testState = "success"
        testText = "Recognized" + (ev.score ? "  ·  " + Number(ev.score).toFixed(2) : "")
      } else {
        testState = ev.result === "fail" ? "failure" : "idle"
        testText = ev.message || "Not recognized"
      }
    }
  }

  Component.onCompleted: {
    targetScreen = targetScreen  // keep the monitor it opened on
    refresh()
    Qt.callLater(function() { keyCatcher.forceActiveFocus() })
  }

  Timer {
    id: messageTimer
    interval: 4000
    onTriggered: root.message = ""
  }

  Process {
    id: statusProc
    command: ["@PREFIX@/bin/omaface", "status", "--json"]
    stdout: StdioCollector { id: statusOut; waitForEnd: true }
    onExited: {
      try {
        root.status = JSON.parse(String(statusOut.text || "{}"))
        root.loaded = true
      } catch (e) {
        root.status = ({})
        root.say("omaFace is not installed", true)
      }
    }
  }

  Process {
    id: actionProc
    property string okText: ""
    stderr: StdioCollector { id: actionErr; waitForEnd: true }
    onExited: function(exitCode) {
      // 126/127: the authorization dialog was dismissed.
      if (exitCode === 0) { if (okText) root.say(okText, false) }
      else if (exitCode === 126 || exitCode === 127) root.say("Not changed", false)
      else root.say(String(actionErr.text || "").trim().split("\n").pop() || "Failed", true)
      root.refresh()
    }
  }

  Timer {
    id: closeFallback
    interval: 3000
    onTriggered: if (root.closing) root.closed()
  }

  Process {
    id: enrollProc
    stdinEnabled: true
    stdout: SplitParser { onRead: function(line) { root.handleEnrollLine(line) } }
    onExited: function(exitCode) {
      if (root.closing) {
        closeFallback.stop()
        root.closed()
        return
      }
      if (root.enrollPhase === "auth") {
        root.enrollPhase = "error"
        root.enrollHint = exitCode === 126 || exitCode === 127 ? "Not authorized" : "Enrollment failed"
      } else if (root.enrollPhase === "running") {
        root.enrollPhase = "error"
        root.enrollHint = "Enrollment stopped"
      }
      root.refresh()
    }
  }

  Process {
    id: testProc
    command: ["@PREFIX@/bin/omaface", "test", "--json"]
    stdout: SplitParser { onRead: function(line) { root.handleTestLine(line) } }
    onExited: function(exitCode) {
      if (root.testState === "scanning") {
        root.testState = "idle"
        root.testText = "Test ended"
      }
    }
  }

  PanelWindow {
    id: panel
    screen: root.targetScreen
    visible: !root.closing
    anchors { top: true; bottom: true; left: true; right: true }
    color: "transparent"
    WlrLayershell.namespace: "omaface"
    WlrLayershell.layer: WlrLayer.Overlay
    // Hand the keyboard to the polkit dialog while it asks for a password.
    WlrLayershell.keyboardFocus: root.authPending ? WlrKeyboardFocus.None : WlrKeyboardFocus.Exclusive
    exclusionMode: ExclusionMode.Ignore

    Rectangle {
      anchors.fill: parent
      color: Commons.Color.menu.scrim
    }

    MouseArea {
      anchors.fill: parent
      onClicked: if (root.view === "main") root.dismiss()
    }

    BorderSurface {
      id: card
      width: root.cardWidth
      height: content.implicitHeight + root.pad * 2
      radius: Style.cornerRadius
      anchors.centerIn: parent
      color: root.background
      borderSpec: root.borderSpec
      padding: root.pad

      Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

      MouseArea { anchors.fill: parent; onClicked: keyCatcher.forceActiveFocus() }

      Item {
        id: keyCatcher
        anchors.fill: parent
        focus: true
        Keys.onPressed: function(event) {
          if (event.key === Qt.Key_Escape) {
            root.back()
            event.accepted = true
          }
        }
      }

      Item {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: root.pad
        implicitHeight: root.view === "main" ? mainView.implicitHeight
          : (root.view === "enroll" ? enrollView.implicitHeight : testView.implicitHeight)

        // ---------------------------------------------------------- main
        Column {
          id: mainView
          visible: root.view === "main"
          width: parent.width
          spacing: Style.spacing.panelGap

          // The mark, the name, and the state in one line.
          Row {
            width: parent.width
            spacing: Style.space(14)

            FaceGlyph {
              id: headerMark
              width: Style.space(40)
              height: width
              color: root.appearances.length > 0 ? root.accent : root.foreground
              opacity: root.appearances.length > 0 ? 1 : 0.5
            }

            Column {
              width: parent.width - headerMark.width - parent.spacing
              anchors.verticalCenter: parent.verticalCenter
              spacing: Style.space(3)

              Text {
                text: "omaFace"
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.heading
                font.bold: true
              }

              Text {
                width: parent.width
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: root.foreground
                opacity: 0.6
                font.family: root.fontFamily
                font.pixelSize: Style.font.bodySmall
                text: !root.loaded ? "Checking…"
                  : root.status.installed !== true ? "Not installed"
                  : root.status.lockedOut === true ? "Paused: use your password once to resume"
                  : (root.appearances.length === 0 ? "Not set up"
                    : root.appearances.length === 1 ? "1 appearance" : root.appearances.length + " appearances")
                    + (root.webcam ? "  ·  webcam" : "  ·  IR camera")
                    + (root.status.service === true ? "  ·  ready" : "  ·  service stopped")
              }
            }
          }

          PanelSeparator { foreground: root.foreground }

          Column {
            width: parent.width
            spacing: Style.space(12)

            PanelSectionHeader {
              width: parent.width
              text: "USE FACE UNLOCK FOR"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Repeater {
              model: root.visiblePlaceRows
              delegate: SettingRow {
                required property var modelData
                width: parent.width
                title: modelData.label
                detail: root.placeDetail(modelData)
                checked: root.places[modelData.key] === true
                busy: actionProc.running
                interactive: root.status.installed === true && !actionProc.running
                foreground: root.foreground
                accent: root.accent
                fontFamily: root.fontFamily
                onToggled: root.run([checked ? "disable" : "enable", modelData.key],
                  modelData.label + (checked ? " off" : " on"))
              }
            }
          }

          PanelSeparator { foreground: root.foreground }

          Column {
            width: parent.width
            spacing: Style.space(12)

            PanelSectionHeader {
              width: parent.width
              text: "SECURITY"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            SettingRow {
              width: parent.width
              title: "Require attention"
              detail: "Eyes open and on the screen, not on a phone or elsewhere"
              checked: root.status.attention !== false
              busy: actionProc.running
              interactive: root.status.installed === true && !actionProc.running
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              onToggled: root.run(["set", "attention", checked ? "off" : "on"],
                "Require attention " + (checked ? "off" : "on"))
            }

            SettingRow {
              width: parent.width
              title: "Ask first"
              detail: root.webcam ? "Always on with a webcam: sudo and prompts wait for Enter or a click"
                : "sudo and prompts wait for Enter or a click"
              checked: root.webcam || root.status.confirm !== false
              busy: actionProc.running
              interactive: !root.webcam && root.status.installed === true && !actionProc.running
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              onToggled: root.run(["set", "confirm", checked ? "off" : "on"],
                "Ask first " + (checked ? "off" : "on"))
            }

            SettingRow {
              width: parent.width
              title: "Webcam fallback"
              detail: root.webcam ? "In use: this laptop has no IR camera"
                : "Only when no IR camera is found; checks every scan for photos and screens"
              checked: root.status.rgbFallback !== false
              busy: actionProc.running
              interactive: root.status.installed === true && !actionProc.running
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              onToggled: root.run(["set", "rgb_fallback", checked ? "off" : "on"],
                "Webcam fallback " + (checked ? "off" : "on"))
            }

            SettingRow {
              visible: root.webcam
              width: parent.width
              title: "Head turns"
              detail: "Strictest: also a few random head turns on every scan"
              checked: root.status.headTurns === true
              busy: actionProc.running
              interactive: root.status.installed === true && !actionProc.running
              foreground: root.foreground
              accent: root.accent
              fontFamily: root.fontFamily
              onToggled: root.run(["set", "head_turns", checked ? "off" : "on"],
                "Head turns " + (checked ? "off" : "on"))
            }
          }

          PanelSeparator { foreground: root.foreground }

          Column {
            width: parent.width
            spacing: Style.space(10)

            PanelSectionHeader {
              width: parent.width
              text: "APPEARANCES"
              foreground: root.foreground
              fontFamily: root.fontFamily
            }

            Text {
              visible: root.loaded && root.appearances.length === 0
              text: "No face set up yet."
              color: root.foreground
              opacity: 0.6
              font.family: root.fontFamily
              font.pixelSize: Style.font.bodySmall
            }

            Repeater {
              model: root.appearances
              delegate: AppearanceRow {
                required property var modelData
                required property int index
                width: parent.width
                name: String(modelData.name || "")
                created: String(modelData.created || "")
                kind: String(modelData.kind || "ir")
                editing: root.renaming === index
                interactive: !actionProc.running
                foreground: root.foreground
                accent: root.accent
                fontFamily: root.fontFamily
                onRenameStarted: root.startRename(index)
                onRenamed: function(newName) { root.finishRename(index, newName) }
                onRenameCancelled: root.cancelRename()
                onRemoveRequested: root.run(["remove", String(index + 1)], "Removed " + (modelData.name || "appearance"))
              }
            }

            Button {
              visible: root.status.installed === true && root.appearances.length < root.maxAppearances
              text: root.appearances.length === 0 ? "Set up omaFace" : "Add another appearance"
              bordered: true
              foreground: root.accent
              fontFamily: root.fontFamily
              onClicked: root.startEnroll()
            }

            Text {
              visible: root.loaded && root.appearances.length >= root.maxAppearances
              width: parent.width
              wrapMode: Text.WordWrap
              text: "Up to " + root.maxAppearances + " appearances. Remove one to add another."
              color: root.foreground
              opacity: 0.5
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }

          PanelSeparator { foreground: root.foreground }

          // Test on the left; messages, or the Esc hint, on the right.
          Item {
            width: parent.width
            height: Math.max(testButton.implicitHeight, footerText.implicitHeight)

            Button {
              id: testButton
              anchors.left: parent.left
              anchors.verticalCenter: parent.verticalCenter
              text: "Test"
              bordered: true
              visible: root.appearances.length > 0
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.startTest()
            }

            Text {
              id: footerText
              anchors.left: testButton.visible ? testButton.right : parent.left
              anchors.leftMargin: testButton.visible ? Style.space(14) : 0
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              horizontalAlignment: Text.AlignRight
              elide: Text.ElideLeft
              textFormat: Text.PlainText
              text: root.message || "Esc to close"
              color: root.messageIsError ? root.errorColor : root.foreground
              opacity: root.message ? 0.9 : 0.4
              font.family: root.fontFamily
              font.pixelSize: Style.font.caption
            }
          }
        }

        // -------------------------------------------------------- enroll
        Column {
          id: enrollView
          visible: root.view === "enroll"
          width: parent.width
          spacing: Style.space(16)

          EnrollRing {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, Style.space(250))
            height: width
            filled: root.enrollFilled
            preview: root.enrollPhase === "running" ? root.enrollPreview : []
            done: root.enrollPhase === "done"
            color: root.foreground
            accent: root.accent
            fontFamily: Style.font.family
          }

          Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            text: root.enrollHint
            color: root.enrollPhase === "error" ? root.errorColor : root.foreground
            font.family: root.fontFamily
            font.pixelSize: Style.font.body
          }

          Button {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.enrollPhase === "done" ? "Done" : (root.enrollPhase === "error" ? "Back" : "Cancel")
            bordered: true
            foreground: root.enrollPhase === "done" ? root.accent : root.foreground
            fontFamily: root.fontFamily
            onClicked: root.back()
          }
        }

        // ---------------------------------------------------------- test
        Column {
          id: testView
          visible: root.view === "test"
          width: parent.width
          spacing: Style.space(16)

          FaceGlyph {
            anchors.horizontalCenter: parent.horizontalCenter
            width: Style.space(110)
            height: width
            mode: root.testState
            color: root.foreground
            successColor: root.accent
            errorColor: root.errorColor
          }

          Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            textFormat: Text.PlainText
            text: root.testText
            color: root.foreground
            font.family: root.fontFamily
            font.pixelSize: Style.font.body
          }

          Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Style.space(10)
            Button {
              text: "Again"
              bordered: true
              visible: root.testState !== "scanning"
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.startTest()
            }
            Button {
              text: "Back"
              bordered: true
              foreground: root.foreground
              fontFamily: root.fontFamily
              onClicked: root.back()
            }
          }
        }
      }
    }
  }
}
