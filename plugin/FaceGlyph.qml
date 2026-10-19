import QtQuick

// The Face ID mark, drawn in theme colours so it sits with the rest of the
// shell: four corner brackets around a line-drawn face.
//   idle      quiet face, the scanner is ready
//   scanning  brackets breathe while a scan line sweeps the face
//   success   the face folds into a check mark
//   failure   a short head-shake in the error colour, then back to idle
Item {
  id: root

  property string mode: "idle"
  property color color: "white"
  property color successColor: color
  property color errorColor: "red"
  property real lineWidth: Math.max(1.5, width / 26)

  property real bracketInset: 0
  property real scanPos: 0
  property real scanAlpha: 0
  property real faceAlpha: 1
  property real checkProgress: 0
  property real shake: 0
  property real failMix: 0

  implicitWidth: 64
  implicitHeight: 64

  onModeChanged: applyState()
  Component.onCompleted: applyState()

  function applyState() {
    breathe.stop()
    sweep.stop()
    if (mode === "scanning") {
      checkIn.stop()
      faceAlpha = 1
      checkProgress = 0
      failMix = 0
      breathe.start()
      sweep.start()
    } else if (mode === "success") {
      bracketReset.start()
      scanFade.start()
      checkIn.restart()
    } else if (mode === "failure") {
      bracketReset.start()
      scanFade.start()
      faceAlpha = 1
      checkProgress = 0
      headShake.restart()
    } else {
      bracketReset.start()
      scanFade.start()
      faceAlpha = 1
      checkProgress = 0
      failMix = 0
    }
  }

  SequentialAnimation {
    id: breathe
    loops: Animation.Infinite
    NumberAnimation { target: root; property: "bracketInset"; to: 4; duration: 650; easing.type: Easing.InOutSine }
    NumberAnimation { target: root; property: "bracketInset"; to: 0; duration: 650; easing.type: Easing.InOutSine }
  }

  SequentialAnimation {
    id: sweep
    loops: Animation.Infinite
    PropertyAction { target: root; property: "scanAlpha"; value: 1 }
    NumberAnimation { target: root; property: "scanPos"; from: 0; to: 1; duration: 900; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "scanPos"; from: 1; to: 0; duration: 900; easing.type: Easing.InOutQuad }
  }

  NumberAnimation { id: bracketReset; target: root; property: "bracketInset"; to: 0; duration: 180; easing.type: Easing.OutQuad }
  NumberAnimation { id: scanFade; target: root; property: "scanAlpha"; to: 0; duration: 160 }

  SequentialAnimation {
    id: checkIn
    NumberAnimation { target: root; property: "faceAlpha"; to: 0; duration: 140; easing.type: Easing.OutQuad }
    NumberAnimation { target: root; property: "checkProgress"; from: 0; to: 1; duration: 260; easing.type: Easing.OutCubic }
  }

  SequentialAnimation {
    id: headShake
    PropertyAction { target: root; property: "failMix"; value: 1 }
    NumberAnimation { target: root; property: "shake"; to: -7; duration: 45; easing.type: Easing.OutQuad }
    NumberAnimation { target: root; property: "shake"; to: 7; duration: 70; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "shake"; to: -5; duration: 70; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "shake"; to: 4; duration: 60; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "shake"; to: 0; duration: 55; easing.type: Easing.OutQuad }
    PauseAnimation { duration: 450 }
    NumberAnimation { target: root; property: "failMix"; to: 0; duration: 300 }
  }

  onBracketInsetChanged: canvas.requestPaint()
  onScanPosChanged: canvas.requestPaint()
  onScanAlphaChanged: canvas.requestPaint()
  onFaceAlphaChanged: canvas.requestPaint()
  onCheckProgressChanged: canvas.requestPaint()
  onFailMixChanged: canvas.requestPaint()
  onColorChanged: canvas.requestPaint()
  onWidthChanged: canvas.requestPaint()
  onHeightChanged: canvas.requestPaint()

  function mix(a, b, t) {
    return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t)
  }

  Canvas {
    id: canvas
    anchors.fill: parent
    anchors.leftMargin: root.shake
    anchors.rightMargin: -root.shake
    antialiasing: true
    renderStrategy: Canvas.Cooperative

    onPaint: {
      var ctx = getContext("2d")
      ctx.reset()
      var s = Math.min(width, height) / 100
      ctx.save()
      ctx.translate((width - 100 * s) / 2, (height - 100 * s) / 2)
      ctx.scale(s, s)
      ctx.lineCap = "round"
      ctx.lineJoin = "round"
      ctx.lineWidth = root.lineWidth / s

      var base = root.mix(root.color, root.errorColor, root.failMix)
      var success = root.checkProgress > 0
      ctx.strokeStyle = success ? root.successColor : base

      // Corner brackets.
      var i = 6 + root.bracketInset
      var len = 18, r = 8, far = 100 - i
      ctx.beginPath()
      ctx.moveTo(i, i + len); ctx.lineTo(i, i + r); ctx.quadraticCurveTo(i, i, i + r, i); ctx.lineTo(i + len, i)
      ctx.moveTo(far - len, i); ctx.lineTo(far - r, i); ctx.quadraticCurveTo(far, i, far, i + r); ctx.lineTo(far, i + len)
      ctx.moveTo(far, far - len); ctx.lineTo(far, far - r); ctx.quadraticCurveTo(far, far, far - r, far); ctx.lineTo(far - len, far)
      ctx.moveTo(i + len, far); ctx.lineTo(i + r, far); ctx.quadraticCurveTo(i, far, i, far - r); ctx.lineTo(i, far - len)
      ctx.stroke()

      // Face: eyes, nose, smile.
      if (root.faceAlpha > 0) {
        ctx.globalAlpha = root.faceAlpha
        ctx.strokeStyle = base
        ctx.beginPath()
        ctx.moveTo(36, 37); ctx.lineTo(36, 45)
        ctx.moveTo(64, 37); ctx.lineTo(64, 45)
        ctx.moveTo(51, 37); ctx.lineTo(51, 57); ctx.lineTo(46, 57)
        ctx.moveTo(37, 67); ctx.quadraticCurveTo(50, 76, 63, 67)
        ctx.stroke()
        ctx.globalAlpha = 1
      }

      // Check mark drawn along its length.
      if (success) {
        var p = root.checkProgress
        var a = {x: 31, y: 52}, b = {x: 45, y: 66}, c = {x: 70, y: 36}
        var l1 = Math.hypot(b.x - a.x, b.y - a.y), l2 = Math.hypot(c.x - b.x, c.y - b.y)
        var d = p * (l1 + l2)
        ctx.strokeStyle = root.successColor
        ctx.lineWidth = root.lineWidth * 1.15 / s
        ctx.beginPath()
        ctx.moveTo(a.x, a.y)
        if (d <= l1) {
          ctx.lineTo(a.x + (b.x - a.x) * d / l1, a.y + (b.y - a.y) * d / l1)
        } else {
          ctx.lineTo(b.x, b.y)
          var t = (d - l1) / l2
          ctx.lineTo(b.x + (c.x - b.x) * t, b.y + (c.y - b.y) * t)
        }
        ctx.stroke()
      }

      // Scan line between the brackets.
      if (root.scanAlpha > 0) {
        var y = 20 + root.scanPos * 60
        ctx.globalAlpha = root.scanAlpha * 0.55
        ctx.strokeStyle = base
        ctx.lineWidth = root.lineWidth * 0.6 / s
        ctx.beginPath()
        ctx.moveTo(20, y); ctx.lineTo(80, y)
        ctx.stroke()
        ctx.globalAlpha = 1
      }
      ctx.restore()
    }
  }
}
