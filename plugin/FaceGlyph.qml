import QtQuick

// The Face ID mark, drawn in theme colours so it sits with the rest of the
// shell: four lightly rounded corner brackets around a face made of square
// pixels on a 9 x 9 grid, in the terminal look of the rest of Omarchy.
//   idle      quiet pixel face, the scanner is ready
//   scanning  brackets breathe while a band sweeps the grid and lights the
//             pixels it passes, like a dot-matrix display
//   success   the face's pixels give way to a pixel check mark, drawn in
//   failure   a short shake with a row glitch in the error colour
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
  property real glitch: 0

  implicitWidth: 64
  implicitHeight: 64

  // Cells as [row, column] on the 9 x 9 grid.
  readonly property var faceCells: [
    [2, 2], [3, 2], [2, 6], [3, 6],          // eyes
    [5, 2], [6, 3], [6, 4], [6, 5], [5, 6]   // mouth
  ]
  // Two pixels thick, in stroke order (down the short arm, up the long one).
  readonly property var checkCells: [
    [4, 1], [5, 1], [5, 2], [6, 2], [6, 3], [7, 3],
    [5, 4], [6, 4], [4, 5], [5, 5], [3, 6], [4, 6], [2, 7], [3, 7]
  ]

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
    NumberAnimation { target: root; property: "bracketInset"; to: 3; duration: 650; easing.type: Easing.InOutSine }
    NumberAnimation { target: root; property: "bracketInset"; to: 0; duration: 650; easing.type: Easing.InOutSine }
  }

  SequentialAnimation {
    id: sweep
    loops: Animation.Infinite
    PropertyAction { target: root; property: "scanAlpha"; value: 1 }
    NumberAnimation { target: root; property: "scanPos"; from: 0; to: 1; duration: 950; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "scanPos"; from: 1; to: 0; duration: 950; easing.type: Easing.InOutQuad }
  }

  NumberAnimation { id: bracketReset; target: root; property: "bracketInset"; to: 0; duration: 180; easing.type: Easing.OutQuad }
  NumberAnimation { id: scanFade; target: root; property: "scanAlpha"; to: 0; duration: 160 }

  SequentialAnimation {
    id: checkIn
    NumberAnimation { target: root; property: "faceAlpha"; to: 0; duration: 150; easing.type: Easing.OutQuad }
    NumberAnimation { target: root; property: "checkProgress"; from: 0; to: 1; duration: 320; easing.type: Easing.Linear }
  }

  SequentialAnimation {
    id: headShake
    PropertyAction { target: root; property: "failMix"; value: 1 }
    PropertyAction { target: root; property: "glitch"; value: 1 }
    NumberAnimation { target: root; property: "shake"; to: -6; duration: 45; easing.type: Easing.OutQuad }
    NumberAnimation { target: root; property: "shake"; to: 6; duration: 70; easing.type: Easing.InOutQuad }
    PropertyAction { target: root; property: "glitch"; value: 0 }
    NumberAnimation { target: root; property: "shake"; to: -4; duration: 70; easing.type: Easing.InOutQuad }
    NumberAnimation { target: root; property: "shake"; to: 3; duration: 60; easing.type: Easing.InOutQuad }
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
  onGlitchChanged: canvas.requestPaint()
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

      var base = root.mix(root.color, root.errorColor, root.failMix)
      var success = root.checkProgress > 0

      // Corner brackets: flat ends, a small corner radius.
      ctx.lineCap = "butt"
      ctx.lineJoin = "round"
      ctx.lineWidth = root.lineWidth / s
      ctx.strokeStyle = success ? root.successColor : base
      var i = 7 + root.bracketInset
      var len = 17, r = 3, far = 100 - i
      ctx.beginPath()
      ctx.moveTo(i, i + len); ctx.lineTo(i, i + r); ctx.quadraticCurveTo(i, i, i + r, i); ctx.lineTo(i + len, i)
      ctx.moveTo(far - len, i); ctx.lineTo(far - r, i); ctx.quadraticCurveTo(far, i, far, i + r); ctx.lineTo(far, i + len)
      ctx.moveTo(far, far - len); ctx.lineTo(far, far - r); ctx.quadraticCurveTo(far, far, far - r, far); ctx.lineTo(far - len, far)
      ctx.moveTo(i + len, far); ctx.lineTo(i + r, far); ctx.quadraticCurveTo(i, far, i, far - r); ctx.lineTo(i, far - len)
      ctx.stroke()

      // The 9 x 9 pixel grid between the brackets.
      var origin = 22.6, pitch = 6.2, size = 5.1
      function cell(row, col, alpha, colour) {
        if (alpha <= 0) return
        var dx = root.glitch > 0 && (row % 3 === 1) ? (row % 2 ? 3 : -3) : 0
        ctx.globalAlpha = Math.min(1, alpha)
        ctx.fillStyle = colour
        ctx.fillRect(origin + col * pitch + dx, origin + row * pitch, size, size)
      }

      // While scanning: the band lights the whole grid faintly where it passes.
      var band = root.scanPos * 8
      if (root.scanAlpha > 0) {
        for (var gr = 0; gr < 9; gr++) {
          var near = Math.max(0, 1 - Math.abs(gr - band) / 1.6)
          if (near <= 0) continue
          for (var gc = 0; gc < 9; gc++) cell(gr, gc, root.scanAlpha * near * 0.28, base)
        }
      }

      // The face, brighter where the band is.
      if (root.faceAlpha > 0) {
        for (var k = 0; k < root.faceCells.length; k++) {
          var fc = root.faceCells[k]
          var lift = root.scanAlpha > 0 ? Math.max(0, 1 - Math.abs(fc[0] - band) / 1.6) : 0
          cell(fc[0], fc[1], root.faceAlpha * (0.75 + 0.25 * lift), base)
        }
      }

      // The check mark, one pixel after another along its stroke.
      if (success) {
        var n = root.checkCells.length
        for (var j = 0; j < n; j++) {
          var t = root.checkProgress * n - j
          if (t <= 0) break
          cell(root.checkCells[j][0], root.checkCells[j][1], t, root.successColor)
        }
      }
      ctx.globalAlpha = 1
      ctx.restore()
    }
  }
}
