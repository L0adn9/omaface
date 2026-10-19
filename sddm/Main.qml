import QtQuick 2.0
import SddmComponents 2.0

// Omarchy's SDDM theme with omaFace. The greeter starts a face scan on its
// own; an empty Enter scans again, a typed password signs in as before.
// The face check runs inside PAM (omaface-pam in /etc/pam.d/sddm), which skips
// the camera whenever a password was typed.
Rectangle {
  id: root
  width: 640
  height: 480
  color: "#1a1b26"

  property string currentUser: userModel.lastUser
  property bool loginFailed: false
  property string faceState: "idle"
  property bool faceAttempt: false
  property bool pendingPassword: false
  property int faceFailures: 0
  // What the scanner asks for right now: "turn-left", "turn-right" or
  // "turn-back" (the webcam's head-turn check), or "" while it just looks.
  property string faceLive: ""
  property int sessionIndex: {
    for (var i = 0; i < sessionModel.rowCount(); i++) {
      var name = (sessionModel.data(sessionModel.index(i, 0), Qt.DisplayRole) || "").toString()
      if (name.indexOf("uwsm") !== -1)
        return i
    }
    return sessionModel.lastIndex
  }

  function startFace() {
    if (root.faceAttempt || root.faceFailures >= 5) return
    root.faceAttempt = true
    root.faceState = "scanning"
    root.loginFailed = false
    sddm.login(root.currentUser, "", root.sessionIndex)
  }

  function submitPassword() {
    if (root.faceAttempt) {
      // PAM is still busy with the camera; send the password right after.
      root.pendingPassword = true
      return
    }
    sddm.login(root.currentUser, password.text, root.sessionIndex)
  }

  Connections {
    target: sddm
    function onLoginFailed() {
      if (root.faceAttempt) {
        root.faceAttempt = false
        root.faceFailures += 1
        root.faceState = "failure"
        faceIdle.restart()
        if (root.pendingPassword && password.text.length > 0) {
          root.pendingPassword = false
          sddm.login(root.currentUser, password.text, root.sessionIndex)
        }
        password.focus = true
        return
      }
      root.loginFailed = true
      password.text = ""
      password.focus = true
    }
    function onLoginSucceeded() {
      root.loginFailed = false
      if (root.faceAttempt) root.faceState = "success"
    }
  }

  // The scanner writes its state to /run/omaface/status; the greeter reads it
  // while a scan runs (QML_XHR_ALLOW_FILE_READ is set for it in 20-omaface.conf).
  Timer {
    interval: 150
    repeat: true
    running: root.faceAttempt
    onRunningChanged: if (!running) root.faceLive = ""
    onTriggered: {
      var request = new XMLHttpRequest()
      request.onreadystatechange = function() {
        if (request.readyState !== XMLHttpRequest.DONE) return
        try {
          var status = JSON.parse(request.responseText || "{}")
          var state = String(status.state || "")
          root.faceLive = status.service === "login" && state.indexOf("turn-") === 0 ? state : ""
        } catch (e) {
          root.faceLive = ""
        }
      }
      request.open("GET", "file:///run/omaface/status")
      request.send()
    }
  }

  Timer {
    id: faceIdle
    interval: 1100
    onTriggered: if (root.faceState === "failure") root.faceState = "idle"
  }

  // Give the greeter a moment to settle, then look for a face.
  Timer {
    interval: 700
    running: true
    onTriggered: root.startFace()
  }

  Column {
    anchors.centerIn: parent
    spacing: 40

    Image {
      id: logo
      source: "logo.png"
      width: Math.min(sourceSize.width, root.width * 0.8)
      height: sourceSize.width > 0 ? Math.round(width * sourceSize.height / sourceSize.width) : 0
      fillMode: Image.PreserveAspectFit
      anchors.horizontalCenter: parent.horizontalCenter
    }

    // The mark, and under it what the scanner asks for. The prompt line keeps
    // its height when empty so nothing below moves as it changes.
    Column {
      anchors.horizontalCenter: parent.horizontalCenter
      spacing: 14
      opacity: root.faceFailures >= 5 ? 0.25 : 1

      Item {
        width: 72
        height: 72
        anchors.horizontalCenter: parent.horizontalCenter

        FaceGlyph {
          anchors.fill: parent
          mode: root.faceState
          color: "#9ece6a"
          successColor: "#9ece6a"
          errorColor: "#f7768e"
          opacity: root.faceState === "idle" ? 0.55 : 1
        }

        MouseArea {
          anchors.fill: parent
          cursorShape: Qt.PointingHandCursor
          onClicked: root.startFace()
        }
      }

      Text {
        anchors.horizontalCenter: parent.horizontalCenter
        height: 20
        verticalAlignment: Text.AlignVCenter
        text: !root.faceAttempt ? ""
          : root.faceLive === "turn-left" ? "\u2190  turn your head left"
          : root.faceLive === "turn-right" ? "turn your head right  \u2192"
          : root.faceLive === "turn-back" ? "face the camera again"
          : "look at the camera"
        color: "#9ece6a"
        opacity: root.faceLive !== "" ? 1 : 0.6
        font.family: "JetBrainsMono Nerd Font"
        font.pixelSize: 16
      }
    }

    Row {
      anchors.horizontalCenter: parent.horizontalCenter
      spacing: 15

      Image {
        source: root.loginFailed ? "lock-failed.png" : "lock.png"
        width: 34
        height: 38
        fillMode: Image.PreserveAspectFit
        anchors.verticalCenter: parent.verticalCenter
      }

      Item {
        width: entry.width
        height: entry.height

        Image {
          id: entry
          source: root.loginFailed ? "entry-failed.png" : "entry.png"
          anchors.centerIn: parent
        }

        Row {
          anchors.left: parent.left
          anchors.leftMargin: 20
          anchors.verticalCenter: parent.verticalCenter
          spacing: 5

          Repeater {
            model: Math.min(password.text.length, 21)

            Image {
              source: "bullet.png"
              width: 7
              height: 7
            }
          }
        }

        TextInput {
          id: password
          anchors.fill: parent
          anchors.leftMargin: 20
          anchors.rightMargin: 20
          verticalAlignment: TextInput.AlignVCenter
          echoMode: TextInput.Password
          font.family: "JetBrainsMono Nerd Font"
          font.pixelSize: 24
          font.letterSpacing: 5
          passwordCharacter: "•"
          color: "transparent"
          selectionColor: "transparent"
          selectedTextColor: "transparent"
          cursorDelegate: Item {}
          focus: true

          onTextChanged: root.loginFailed = false

          Keys.onPressed: {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
              if (password.text.length === 0) root.startFace()
              else root.submitPassword()
              event.accepted = true
            }
          }
        }
      }
    }

  }

  Component.onCompleted: password.forceActiveFocus()
}
