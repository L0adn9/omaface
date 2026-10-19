import QtQuick

// Enrollment ring: ticks around the edge light up as the head turns toward
// them, the way Face ID setup fills its circle. Inside, a braille render of
// the IR camera so you can see yourself without a video window.
Item {
  id: root

  property var filled: []
  property var preview: []
  property bool done: false
  property color color: "white"
  property color accent: color
  property string fontFamily: "monospace"

  implicitWidth: 240
  implicitHeight: 240

  onFilledChanged: ring.requestPaint()
  onDoneChanged: ring.requestPaint()
  onColorChanged: ring.requestPaint()
  onAccentChanged: ring.requestPaint()

  Canvas {
    id: ring
    anchors.fill: parent
    antialiasing: true

    onPaint: {
      var ctx = getContext("2d")
      ctx.reset()
      var n = Math.max(1, root.filled.length || 24)
      var ticks = n * 3
      var cx = width / 2, cy = height / 2
      var outer = Math.min(width, height) / 2 - 2
      var inner = outer * 0.86
      ctx.lineCap = "round"
      ctx.lineWidth = Math.max(2, outer / 40)
      for (var t = 0; t < ticks; t++) {
        var seg = Math.floor(t / 3)
        var on = root.done || root.filled[seg] === 1
        var a = -Math.PI / 2 + t / ticks * Math.PI * 2
        ctx.strokeStyle = on ? root.accent : root.color
        ctx.globalAlpha = on ? 1 : 0.22
        var r2 = on ? outer : outer - (outer - inner) * 0.35
        ctx.beginPath()
        ctx.moveTo(cx + Math.cos(a) * inner, cy + Math.sin(a) * inner)
        ctx.lineTo(cx + Math.cos(a) * r2, cy + Math.sin(a) * r2)
        ctx.stroke()
      }
      ctx.globalAlpha = 1
    }
  }

  Text {
    id: art
    anchors.centerIn: parent
    textFormat: Text.PlainText
    text: (root.preview || []).join("\n")
    visible: !root.done && text.length > 0
    color: root.color
    opacity: 0.85
    font.family: root.fontFamily
    // 30 braille columns by 12 rows fill about 18 x 14.4 em.
    font.pixelSize: Math.max(4, Math.floor(root.width * 0.62 / 18))
    lineHeightMode: Text.ProportionalHeight
    lineHeight: 1.0
    horizontalAlignment: Text.AlignHCenter
  }

  FaceGlyph {
    anchors.centerIn: parent
    width: parent.width * 0.42
    height: width
    visible: root.done || art.text.length === 0
    mode: root.done ? "success" : "scanning"
    color: root.color
    successColor: root.accent
  }
}
