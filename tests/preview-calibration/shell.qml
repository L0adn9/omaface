import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "plugin"
import "Fixtures.js" as Fixtures

ShellRoot {
  id: root

  property var metadata: Fixtures.metadata()
  property var saved: Fixtures.copy(metadata.values)
  property var draft: Fixtures.copy(saved)
  property string cameraKind: "webcam"
  property string testState: "idle"
  property string testPlace: "lock"
  property var require: ({})
  property var frame: ({})
  property var result: ({})
  property string testStatus: ""
  property string scenarioName: "ready"
  property bool conflicted: false
  property string error: ""
  property string notice: ""
  property string lastCapture: ""
  property bool capturePending: false
  property int applyAttempts: 0
  property int testRequests: 0
  property int viewportWidth: 0
  property int viewportHeight: 0
  readonly property bool dirty: Object.keys(saved).some(function(key) { return saved[key] !== draft[key] })

  function selectScenario(name) {
    if (Fixtures.scenarios.indexOf(name) < 0) return "Unknown scenario: " + name
    var fixture = Fixtures.scenario(name)
    metadata = fixture.metadata; saved = fixture.saved; draft = fixture.draft
    cameraKind = fixture.cameraKind; testState = fixture.testState; testPlace = fixture.testPlace
    require = fixture.require; frame = fixture.frame; result = fixture.result; testStatus = fixture.testStatus
    error = fixture.error; notice = fixture.notice; conflicted = fixture.conflicted
    scenarioName = name; applyAttempts = 0; testRequests = 0
    return "Fixture scenario: " + name
  }

  function edit(key, next) {
    var bounds = metadata.bounds[key]
    if (!bounds || typeof next !== "number" || !isFinite(next) || next < bounds.min || next > bounds.max || (bounds.integer && Math.floor(next) !== next)) return false
    if (draft[key] === next) return true
    var candidate = Fixtures.copy(draft); candidate[key] = next; draft = candidate
    clearTest()
    notice = "Unsaved sample draft · No authentication settings were written"
    return true
  }

  // As in Settings: a result belongs to the draft it was tested with.
  function clearTest() {
    if (testState === "running" || testState === "stopping") return
    testState = "idle"; require = {}; frame = {}; result = {}; testStatus = ""
  }

  function stopTest() {
    testState = "idle"; testStatus = ""
    notice = "Fixture test stopped · No camera was opened"
  }

  function defaultsDraft() {
    draft = Fixtures.copy(metadata.defaults); clearTest()
    notice = "Defaults restored to the sample draft only · Nothing saved"
  }

  function revertDraft() {
    draft = Fixtures.copy(saved); conflicted = false; clearTest()
    notice = "Sample draft reverted to the latest fixture values · Nothing saved"
  }

  function inertApply() {
    if (testState === "running") stopTest()
    applyAttempts += 1
    notice = "Fixture Apply is disabled · Draft retained · No authorization or save"
  }

  function capture(path) {
    if (capturePending) return "Capture already pending"
    if (!path || path.charAt(0) !== "/") return "Use an absolute capture path"
    capturePending = true
    captureDelay.path = path
    captureDelay.restart()
    return "Capture queued: " + path
  }

  function describe() {
    return JSON.stringify({ scenario: scenarioName, width: captureContent.width, height: captureContent.height, windowWidth: preview.width, windowHeight: preview.height, draft: draft, saved: saved, dirty: dirty, conflicted: conflicted, testState: testState, testPlace: testPlace, banner: calibration.bannerTitle, detail: calibration.bannerDetail, applyAttempts: applyAttempts, testRequests: testRequests, lastCapture: lastCapture, capturePending: capturePending, fixture: true, cameraOpened: false, authenticated: false })
  }

  Component.onCompleted: {
    selectScenario(Quickshell.env("OMAFACE_PREVIEW_SCENARIO") || "reject")
    var requestedWidth = Number(Quickshell.env("OMAFACE_PREVIEW_WIDTH"))
    var requestedHeight = Number(Quickshell.env("OMAFACE_PREVIEW_HEIGHT"))
    if (requestedWidth > 0) preview.implicitWidth = Math.max(320, requestedWidth)
    if (requestedHeight > 0) preview.implicitHeight = Math.max(360, requestedHeight)
    if (Quickshell.env("OMAFACE_PREVIEW_CAPTURE")) startupCapture.start()
  }

  Timer {
    id: startupCapture
    interval: 1000
    onTriggered: {
      root.capture(Quickshell.env("OMAFACE_PREVIEW_CAPTURE"))
    }
  }
  Timer {
    id: captureDelay
    property string path: ""
    interval: 250
    onTriggered: {
      var target = path
      captureContent.grabToImage(function(result) {
        var saved = result.saveToFile(target)
        root.lastCapture = saved ? target : "FAILED: " + target
        root.capturePending = false
        console.log("Fixture capture " + (saved ? "saved: " : "FAILED: ") + target)
        if (Quickshell.env("OMAFACE_PREVIEW_EXIT_AFTER_CAPTURE") === "1") Qt.quit()
      })
    }
  }

  IpcHandler {
    target: "calibration"
    function scenario(name: string): string { return root.selectScenario(name) }
    function state(): string { return root.describe() }
    function size(width: int, height: int): string {
      root.viewportWidth = Math.max(320, Math.min(1600, width))
      root.viewportHeight = Math.max(360, Math.min(1400, height))
      preview.implicitWidth = root.viewportWidth
      preview.implicitHeight = root.viewportHeight
      return "Fixture content viewport: " + root.viewportWidth + "x" + root.viewportHeight + "; outer window size is compositor-owned"
    }
    function resize(width: int, height: int): string { return size(width, height) }
    function screenshot(path: string): string { return root.capture(path) }
    function capture(path: string): string { return root.capture(path) }
    function action(name: string): string {
      if (name === "defaults") root.defaultsDraft()
      else if (name === "revert") root.revertDraft()
      else if (name === "apply") root.inertApply()
      else if (name === "stop") root.stopTest()
      else if (name === "test") root.selectScenario("streak")
      else if (name === "refresh") root.notice = "Fixture metadata refreshed · No installed configuration was read"
      else return "Unknown action: " + name
      return root.describe()
    }
    function edit(key: string, value: string): string { return root.edit(key, Number(value)) ? root.describe() : "Invalid fixture edit" }
    function quit(): void { Qt.quit() }
  }

  FloatingWindow {
    id: preview
    title: "omaFace · Calibration fixture preview"
    implicitWidth: 740
    implicitHeight: 850
    minimumSize: Qt.size(320, 360)
    color: Commons.Color.menu.background
    visible: true

    BorderSurface {
      id: captureContent
      width: root.viewportWidth || parent.width
      height: root.viewportHeight || parent.height
      color: Commons.Color.menu.background
      borderSpec: Border.surfaceSpec("menu", "border", Commons.Color.menu.border, Math.max(1, Style.space(2)))
      Calibration {
        id: calibration
        anchors.fill: parent
        anchors.margins: Style.spacing.panelPadding
        draft: root.draft
        saved: root.saved
        metadata: root.metadata
        testState: root.testState
        testPlace: root.testPlace
        require: root.require
        frame: root.frame
        result: root.result
        testStatus: root.testStatus
        cameraKind: root.cameraKind
        dirty: root.dirty
        conflicted: root.conflicted
        error: root.error
        notice: root.notice
        canStart: true
        canApply: true
        fixtureLabel: "Fixture · Simulated test · No camera or authentication"
        onValueEdited: function(key, next) { root.edit(key, next) }
        onTestRequested: { root.selectScenario("streak"); root.testRequests += 1 }
        onStopRequested: root.stopTest()
        onPlaceSelected: function(place) { root.testPlace = place }
        onApplyRequested: root.inertApply()
        onDefaultsRequested: root.defaultsDraft()
        onRevertRequested: root.revertDraft()
        onRefreshRequested: root.notice = "Fixture metadata refreshed · No installed configuration was read"
        onBackRequested: {
          if (root.testState === "running") root.stopTest()
          root.notice = "Fixture Back · This harness stays open"
        }
        focus: true
        Keys.onPressed: function(event) {
          var index = event.key - Qt.Key_1
          if (index >= 0 && index < 9 && (event.modifiers & Qt.ControlModifier)) {
            root.selectScenario(Fixtures.scenarios[index]); event.accepted = true
          }
        }
      }
    }
  }
}
