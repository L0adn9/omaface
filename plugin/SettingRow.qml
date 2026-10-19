import QtQuick
import qs.Commons
import qs.Ui

// One setting in the omaFace window: a title, a line of detail and a switch.
// The text column always ends before the switch and wraps, so no title or
// detail can run underneath it whatever the wording or the screen scale.
// The switch is controlled: it reports a click (toggled) and the owner
// decides the state, like Omarchy's other panels.
Item {
  id: row

  property string title: ""
  property string detail: ""
  property bool checked: false
  property bool interactive: true
  property bool busy: false
  property color foreground: "white"
  property color accent: foreground
  property string fontFamily: Style.font.family

  signal toggled()

  implicitHeight: Math.max(texts.implicitHeight, toggle.implicitHeight)

  Column {
    id: texts
    anchors.left: parent.left
    anchors.right: toggle.left
    anchors.rightMargin: Style.space(16)
    anchors.verticalCenter: parent.verticalCenter
    spacing: Style.space(2)

    Text {
      width: parent.width
      wrapMode: Text.WordWrap
      textFormat: Text.PlainText
      text: row.title
      color: row.foreground
      opacity: row.interactive ? 1 : 0.55
      font.family: row.fontFamily
      font.pixelSize: Style.font.body
    }

    Text {
      width: parent.width
      visible: row.detail !== ""
      wrapMode: Text.WordWrap
      textFormat: Text.PlainText
      text: row.detail
      color: row.foreground
      opacity: 0.5
      font.family: row.fontFamily
      font.pixelSize: Style.font.caption
    }
  }

  ToggleSwitch {
    id: toggle
    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    checked: row.checked
    busy: row.busy
    interactive: row.interactive
    // No keyboard cursor in this window, so no room reserved for its ring:
    // the switch lines up with the separators and buttons.
    cursorRing: false
    foreground: row.foreground
    accent: row.accent
    onToggled: row.toggled()
  }
}
