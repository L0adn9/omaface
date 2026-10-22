"""omaFace additions to Omarchy's lock screen and polkit dialog.

Each patch replaces one exact snippet of the upstream file. Every `old` must
appear exactly once, otherwise the build stops and the installed plugin is
left alone (see build-plugin.py). Keep snippets small so upstream changes
elsewhere in the files never conflict.

The anchors target Omarchy 4.5, which renamed the shell's IpcHandler to
ShellIpc and split fingerprint state into FingerprintModel.js. On older files
the anchors do not match, so setup stops before replacing a working plugin.
"""

LOCK_SERVICE = [
    # State for the third authentication flow.
    (
        "  property bool strandedLockResolved: false\n",
        """  property bool strandedLockResolved: false

  // omaFace (io.github.l0adn9.omaface): a third PAM flow beside password and
  // fingerprint. It only ever adds a way in; the password always works.
  property bool faceConfigured: false
  property string faceState: "idle"
  property int faceFailures: 0
  property double faceLockedAt: 0
  property double faceStartedAt: 0
  property double faceEndedAt: 0
  readonly property int faceMaxFailures: 5
  readonly property bool faceAvailable: faceConfigured && faceFailures < faceMaxFailures
  property string faceLive: ""
""",
    ),
    (
        "    fingerprintRetryTimer.stop()\n    fingerprintReachTimer.stop()\n"
        "    fingerprintResumeTimer.stop()\n    if (passwordPam.active) passwordPam.abort()\n"
        "    if (fingerprintPam.active) fingerprintPam.abort()\n",
        """    fingerprintRetryTimer.stop()
    fingerprintReachTimer.stop()
    fingerprintResumeTimer.stop()
    faceSuccessTimer.stop()
    faceIdleTimer.stop()
    faceFailures = 0
    faceState = "idle"
    if (facePam.active) facePam.abort()
    if (passwordPam.active) passwordPam.abort()
    if (fingerprintPam.active) fingerprintPam.abort()
""",
    ),
    (
        "    lockRequested = true\n    armBlankTimer()\n",
        "    lockRequested = true\n    faceLockedAt = Date.now()\n    armBlankTimer()\n",
    ),
    (
        "      root.refreshBackground()\n      root.refreshFingerprintStatus()\n    })\n\n    return true\n",
        "      root.refreshBackground()\n      root.refreshFingerprintStatus()\n      root.refreshFaceStatus()\n    })\n\n    return true\n",
    ),
    # Scan control.
    (
        "  WlSessionLock {\n    id: sessionLock\n",
        """  function refreshFaceStatus() {
    if (!faceCheckProc.running) faceCheckProc.running = true
  }

  // Activity on the lock screen (a key, the pointer, a click on the mark)
  // starts a scan. A lock you have just asked for stays locked while you are
  // still sitting there, so the first moments after locking are ignored.
  function faceWake() {
    if (!lockRequested || !sessionLock.secure || !faceAvailable) return
    if (facePam.active || faceState === "success") return
    var now = Date.now()
    if (now - faceLockedAt < 1500) return
    if (now - faceEndedAt < 1200) return
    startFace()
  }

  function startFace() {
    if (!lockRequested || !sessionLock.secure || !faceAvailable) return
    if (facePam.active || faceState === "success") return
    faceIdleTimer.stop()
    faceStartedAt = Date.now()
    faceState = "scanning"
    if (!facePam.start()) {
      faceState = "idle"
      faceEndedAt = Date.now()
    }
  }

  // The PAM result only says yes or no; the engine's status file says
  // whether "no" was a stranger's face or simply nobody in front.
  function readFaceOutcome() {
    faceStatusFile.reload()
    try {
      var status = JSON.parse(String(faceStatusFile.text() || "{}"))
      if (status.service === "lock" && status.at * 1000 >= faceStartedAt - 2000) return String(status.state || "")
    } catch (e) {}
    return ""
  }

  function handleFaceFinished(result) {
    faceEndedAt = Date.now()
    if (!lockRequested) return
    if (result === PamResult.Success) {
      faceState = "success"
      faceSuccessTimer.restart()
      return
    }
    var outcome = readFaceOutcome()
    if (outcome === "failure") {
      faceFailures += 1
      faceState = "failure"
      faceIdleTimer.restart()
    } else {
      // "attention": your face, but eyes closed or not on the camera; the
      // view says so until the next scan.
      faceState = outcome === "unavailable" || outcome === "attention" ? outcome : "idle"
    }
  }

  WlSessionLock {
    id: sessionLock
""",
    ),
    (
        "        failedAttempts: root.failedAttempts\n        inputEnabled: root.lockRequested\n",
        """        failedAttempts: root.failedAttempts
        faceState: root.faceState
        faceLive: root.faceLive
        faceAvailable: root.faceAvailable
        inputEnabled: root.lockRequested
""",
    ),
    (
        "        onClearFailureRequested: root.failureMessage = \"\"\n        onWakeRequested: root.runWake()\n",
        """        onClearFailureRequested: root.failureMessage = ""
        onWakeRequested: {
          root.runWake()
          root.faceWake()
        }
        onFaceRequested: root.startFace()
""",
    ),
    (
        "      failedAttempts: 0\n      inputEnabled: false\n      loadBackground: root.previewVisible\n",
        "      failedAttempts: 0\n      faceAvailable: root.faceConfigured\n      inputEnabled: false\n      loadBackground: root.previewVisible\n",
    ),
    # PAM context, timers and the readiness probe.
    (
        "  Timer {\n    id: fingerprintRetryTimer\n",
        """  PamContext {
    id: facePam
    config: "omaface-lock"
    user: root.userName

    onCompleted: function(result) {
      root.handleFaceFinished(result)
    }

    onError: function(error) {
      root.handleFaceFinished(PamResult.Error)
    }
  }

  // Lets the check mark finish drawing before the lock surface goes away.
  Timer {
    id: faceSuccessTimer
    interval: 420
    repeat: false
    onTriggered: if (root.lockRequested) root.finishUnlock()
  }

  Timer {
    id: faceIdleTimer
    interval: 1100
    repeat: false
    onTriggered: if (root.faceState === "failure") root.faceState = "idle"
  }

  // Opening the lid wakes the machine while the lock is up. A suspend shows
  // up as a jump in wall-clock time between ticks; scan right away then, the
  // way Windows Hello greets you when the lid opens.
  Timer {
    id: faceResumeWatch
    interval: 2000
    repeat: true
    running: root.lockRequested && root.faceAvailable
    property double lastTick: 0
    onRunningChanged: lastTick = Date.now()
    onTriggered: {
      var now = Date.now()
      var resumed = lastTick > 0 && now - lastTick > interval + 4000
      lastTick = now
      if (resumed) root.startFace()
    }
  }

  FileView {
    id: faceStatusFile
    path: "/run/omaface/status"
    blockLoading: true
    printErrors: false
  }

  // The webcam's head-turn check asks for moves while it scans; the lock
  // view shows them as they come.
  FileView {
    path: "/run/omaface/status"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: {
      try {
        var status = JSON.parse(String(text() || "{}"))
        var state = String(status.state || "")
        root.faceLive = status.service === "lock" && state.indexOf("turn-") === 0 ? state : ""
      } catch (e) {
        root.faceLive = ""
      }
    }
  }

  // After the password unlocks, earlier rejected scans no longer count
  // (webcam strikes otherwise stay until then).
  Process {
    id: faceResetProc
    command: ["bash", "-c", "[[ -x @PREFIX@/bin/omaface ]] && @PREFIX@/bin/omaface reset"]
  }

  Process {
    id: faceCheckProc
    command: ["bash", "-c", "[[ -f /etc/pam.d/omaface-lock ]] && [[ -x @PREFIX@/bin/omaface ]] && @PREFIX@/bin/omaface ready lock && echo yes || echo no"]
    stdout: StdioCollector { id: faceCheckStdout; waitForEnd: true }
    onExited: {
      root.faceConfigured = String(faceCheckStdout.text || "").trim() === "yes"
      if (!root.faceConfigured && facePam.active) facePam.abort()
    }
  }

  Timer {
    id: fingerprintRetryTimer
""",
    ),
    (
        "  Component.onCompleted: {\n    knownScreenCount = realScreenCount()\n"
        "    refreshBackground()\n    refreshFingerprintStatus()\n",
        """  Component.onCompleted: {
    knownScreenCount = realScreenCount()
    refreshBackground()
    refreshFingerprintStatus()
    refreshFaceStatus()
""",
    ),
    (
        "        fingerprint: root.fingerprintConfigured,\n",
        "        fingerprint: root.fingerprintConfigured,\n        face: root.faceConfigured,\n        faceState: root.faceState,\n        faceFailures: root.faceFailures,\n",
    ),
    (
        "      root.refreshBackground()\n      root.refreshFingerprintStatus()\n      root.previewVisible = true\n",
        "      root.refreshBackground()\n      root.refreshFingerprintStatus()\n      root.refreshFaceStatus()\n      root.previewVisible = true\n",
    ),
    # The polkit dialog lives in this service too, so one plugin owns every
    # password prompt in the shell; omarchy.polkit stays disabled meanwhile.
    (
        "  ShellIpc {\n    target: \"lock\"\n",
        """  Polkit {
    id: polkitDialog
  }

  // Settings exist only while open (omarchy-shell omaface settings).
  Loader {
    id: faceSettings
    active: false
    source: "Settings.qml"
    onLoaded: item.closed.connect(function() { faceSettings.active = false })
  }

  ShellIpc {
    target: "omaface"

    function status(): string {
      return JSON.stringify({
        plugin: "io.github.l0adn9.omaface",
        lock: true,
        polkitRegistered: polkitDialog.agentRegistered,
        face: root.faceConfigured,
        faceState: root.faceState
      })
    }

    function refresh(): string {
      root.refreshFaceStatus()
      polkitDialog.refreshFace()
      return "ok"
    }

    function settings(): string {
      if (!faceSettings.active) {
        faceSettings.active = true
        return "open"
      }
      // Through dismiss(), so a running enrollment releases the camera first.
      if (faceSettings.item) faceSettings.item.dismiss()
      else faceSettings.active = false
      return "closed"
    }
  }

  ShellIpc {
    target: "lock"
""",
    ),
    (
        "      if (result === PamResult.Success) root.finishUnlock()\n      else root.handlePasswordFailure()\n",
        """      if (result === PamResult.Success) {
        if (!faceResetProc.running) faceResetProc.running = true
        root.finishUnlock()
      } else root.handlePasswordFailure()
""",
    ),
]

LOCK_VIEW = [
    (
        "  property string passwordText: \"\"\n  property bool syncingPasswordText: false\n",
        """  property string passwordText: ""
  property bool syncingPasswordText: false
  property string faceState: "idle"
  property string faceLive: ""
  property bool faceAvailable: false
""",
    ),
    (
        "  signal wakeRequested()\n",
        "  signal wakeRequested()\n  signal faceRequested()\n",
    ),
    (
        "    BorderSurface {\n      id: inputField\n",
        """    // omaFace: the mark sits above the field and animates while the IR
    // camera looks. Clicking it asks for another scan.
    Rectangle {
      id: faceTile
      width: Math.round(root.fieldHeight * 1.25)
      height: width
      radius: Style.cornerRadius
      color: Commons.Color.lock.background
      anchors.horizontalCenter: inputField.horizontalCenter
      anchors.bottom: inputField.top
      anchors.bottomMargin: Math.round(root.fieldHeight * 0.45)
      visible: (root.faceAvailable && root.faceState !== "unavailable") || root.faceState === "success"
      opacity: visible ? 1 : 0

      FaceGlyph {
        objectName: "faceIndicator"
        anchors.centerIn: parent
        width: Math.round(parent.width * 0.66)
        height: width
        mode: root.faceState
        opacity: root.faceState === "idle" ? 0.6 : 1
        color: Commons.Color.lock.text
        successColor: Commons.Color.lock.borderActive
        errorColor: Commons.Color.lock.textError
        Behavior on opacity { NumberAnimation { duration: 180 } }
      }

      MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: {
          root.wakeRequested()
          root.faceRequested()
        }
      }
    }

    Text {
      anchors.horizontalCenter: faceTile.horizontalCenter
      anchors.bottom: faceTile.top
      anchors.bottomMargin: Math.round(root.fieldHeight * 0.25)
      visible: faceTile.visible && (root.faceLive !== "" || root.faceState === "scanning" || root.faceState === "attention")
      text: root.faceLive === "turn-left" ? "\u2190  Turn your head left"
        : root.faceLive === "turn-right" ? "Turn your head right  \u2192"
        : root.faceLive === "turn-back" ? "Face the camera again"
        : root.faceState === "attention" ? "Open your eyes and look at the screen" : "Look at the screen"
      color: Commons.Color.lock.text
      opacity: root.faceLive !== "" ? 1 : 0.7
      font.family: Style.font.family
      font.pixelSize: root.faceLive !== "" ? Style.font.body : Style.font.bodySmall
    }

    BorderSurface {
      id: inputField
""",
    ),
]

POLKIT = [
    (
        "  property bool laptopClosed: false\n",
        """  property bool laptopClosed: false
  // omaFace: omaface-pam sits in the polkit stack and a face is enrolled.
  // The engine's status file drives the animation while the camera looks.
  property bool facePamConfigured: false
  property bool faceReady: false
  readonly property bool faceConfigured: facePamConfigured && faceReady
  property string faceState: "idle"
  property double flowStartedAt: 0
  property string pendingPassword: ""
  property string faceLive: ""
  readonly property bool agentRegistered: polkitAgent.isRegistered
""",
    ),
    (
        "  readonly property bool fingerprintMode: fingerprintConfigured && !laptopClosed",
        "  readonly property bool fingerprintMode: (fingerprintConfigured || faceConfigured) && !laptopClosed",
    ),
    (
        "  function loadPamConfig(raw) {\n    fingerprintConfigured = PolkitModel.fingerprintConfiguredFromPamConfig(raw)\n  }\n",
        """  function loadPamConfig(raw) {
    fingerprintConfigured = PolkitModel.fingerprintConfiguredFromPamConfig(raw)
    facePamConfigured = String(raw || "").indexOf("omaface-pam") !== -1
  }

  function refreshFace() {
    if (!faceReadyProc.running) faceReadyProc.running = true
  }

  function loadFaceStatus(raw) {
    try {
      var status = JSON.parse(String(raw || "{}"))
      if (status.service !== "polkit" || status.at * 1000 < flowStartedAt - 2000) return
      var state = String(status.state || "")
      // The webcam's head-turn prompts are still a scan, with a live hint.
      faceLive = state.indexOf("turn-") === 0 ? state : ""
      if (faceLive !== "") state = "scanning"
      faceState = state === "choose" || state === "scanning" || state === "success" || state === "failure" ? state : "idle"
    } catch (e) {}
  }

  // Ask first: the camera waits until the person picks omaFace (Enter or a
  // click). Typing picks the password instead, and what was typed is kept
  // for the password field polkit asks for next.
  function chooseFace(choice) {
    if (faceChooseProc.running) return
    faceState = choice === "face" ? "scanning" : "idle"
    faceChooseProc.command = ["@PREFIX@/bin/omaface", "choose", choice]
    faceChooseProc.running = true
  }

  function takeTyping(event) {
    if (event.key === Qt.Key_Backspace) {
      pendingPassword = pendingPassword.slice(0, -1)
      return true
    }
    var text = String(event.text || "")
    if (text.length === 0 || text.charCodeAt(0) < 32) return false
    pendingPassword += text
    if (faceState === "choose" || faceState === "scanning") chooseFace("password")
    return true
  }
""",
    ),
    (
        "  function beginFlow() {\n    closeTimer.stop()\n    closing = false\n",
        """  function beginFlow() {
    closeTimer.stop()
    closeTimer.interval = 300
    closing = false
    flowStartedAt = Date.now()
    faceState = "idle"
    pendingPassword = ""
    refreshFace()
""",
    ),
    (
        "    function onAuthenticationSucceeded() {\n      root.closing = true\n      closeTimer.restart()\n",
        """    function onAuthenticationSucceeded() {
      root.closing = true
      // Let the check mark draw when the face is what got you in.
      closeTimer.interval = root.faceState === "success" ? 520 : 300
      closeTimer.restart()
""",
    ),
    (
        "  Process {\n    id: laptopClosedProc\n",
        """  FileView {
    path: "/run/omaface/status"
    watchChanges: true
    printErrors: false
    onLoaded: root.loadFaceStatus(text())
    onFileChanged: reload()
  }

  Process {
    id: faceChooseProc
  }

  Process {
    id: faceReadyProc
    command: ["bash", "-c", "[[ -x @PREFIX@/bin/omaface ]] && @PREFIX@/bin/omaface ready polkit && echo yes || echo no"]
    stdout: StdioCollector { id: faceReadyOut; waitForEnd: true }
    onExited: root.faceReady = String(faceReadyOut.text || "").trim() === "yes"
  }

  Process {
    id: laptopClosedProc
""",
    ),
    (
        "        visible: root.fingerprintMode\n        text: \"\\udb80\\ude37\"\n",
        "        visible: root.fingerprintMode && !root.faceConfigured\n        text: \"\\udb80\\ude37\"\n",
    ),
    (
        "      Row {\n        id: cardRow\n",
        """      FaceGlyph {
        anchors.centerIn: parent
        width: Math.round(root.fieldHeight * 0.8)
        height: width
        visible: root.fingerprintMode && root.faceConfigured
        mode: root.faceState
        color: root.foreground
        successColor: root.accent
        errorColor: Commons.Color.polkit.textError
      }

      MouseArea {
        anchors.fill: parent
        visible: root.fingerprintMode && root.faceConfigured && root.faceState === "choose"
        cursorShape: Qt.PointingHandCursor
        onClicked: root.chooseFace("face")
      }

      Row {
        id: cardRow
""",
    ),
    (
        "    if (responseRequired) submitted = false\n  }\n",
        """    if (responseRequired) submitted = false
    // Keys typed while omaFace was on offer belong to the password.
    if (responseRequired && pendingPassword.length) {
      passwordInput.text = pendingPassword
      pendingPassword = ""
      passwordInput.cursorPosition = passwordInput.text.length
    }
  }
""",
    ),
    (
        "  function cancelRequest() {\n    var flow = polkitAgent.flow\n    passwordInput.text = \"\"\n",
        "  function cancelRequest() {\n    var flow = polkitAgent.flow\n    passwordInput.text = \"\"\n    pendingPassword = \"\"\n    // Polkit's helper only learns of the cancel after PAM returns; tell the\n    // engine now so the wait (or the camera) ends at once.\n    if (faceState === \"choose\" || faceState === \"scanning\") chooseFace(\"password\")\n",
    ),
    (
        """          } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (root.responseRequired) root.submitResponse()
            event.accepted = true
          }
""",
        """          } else if (root.faceState === "choose" && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            root.chooseFace("face")
            event.accepted = true
          } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (root.responseRequired) root.submitResponse()
            event.accepted = true
          } else if (root.fingerprintMode && root.faceConfigured && !root.responseRequired && root.takeTyping(event)) {
            event.accepted = true
          }
""",
    ),
    (
        "    Rectangle {\n      width: Math.min(justificationText.implicitWidth",
        """    Text {
      anchors.horizontalCenter: card.horizontalCenter
      anchors.top: card.bottom
      anchors.topMargin: Style.space(10)
      visible: root.fingerprintMode && root.faceConfigured && (root.faceState === "choose" || root.faceState === "scanning")
      text: root.faceState === "choose" ? "Enter or click for omaFace  \u00b7  type for password"
        : root.faceLive === "turn-left" ? "\u2190  Turn your head left"
        : root.faceLive === "turn-right" ? "Turn your head right  \u2192"
        : root.faceLive === "turn-back" ? "Face the camera again"
        : "Look at the screen"
      color: root.foreground
      opacity: 0.7
      font.family: root.fontFamily
      font.pixelSize: Style.font.bodySmall
    }

    Rectangle {
      width: Math.min(justificationText.implicitWidth""",
    ),
    (
        "    function onAuthenticationRequestCancelled() {\n      root.closing = true\n      closeTimer.restart()\n    }\n",
        """    function onAuthenticationRequestCancelled() {
      root.closing = true
      closeTimer.restart()
      // Cancelled by polkit (the asking program went away): end omaFace too.
      if (root.faceState === "choose" || root.faceState === "scanning") root.chooseFace("password")
    }
""",
    ),
]

# (upstream path relative to the shell plugins dir, file name in the plugin, patches)
FILES = [
    ("lock/Service.qml", "Service.qml", LOCK_SERVICE),
    ("lock/LockView.qml", "LockView.qml", LOCK_VIEW),
    ("lock/FingerprintModel.js", "FingerprintModel.js", []),
    ("lock/LockFeedSurface.qml", "LockFeedSurface.qml", []),
    ("polkit/PolkitAgent.qml", "Polkit.qml", POLKIT),
    ("polkit/PolkitModel.js", "PolkitModel.js", []),
]
