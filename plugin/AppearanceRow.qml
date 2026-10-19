import QtQuick
import qs.Commons
import qs.Ui

// One enrolled appearance: its name, when it was added and with which
// camera, and Rename / Remove. Renaming happens in place (Enter saves, Esc
// cancels); the owner only hears the outcome. The buttons keep a fixed gap
// from the text, which elides instead of running into them.
Item {
  id: row

  property string name: ""
  property string created: ""
  property string kind: "ir"
  property bool editing: false
  property bool interactive: true
  property color foreground: "white"
  property color accent: foreground
  property string fontFamily: Style.font.family

  signal renameStarted()
  signal renamed(string name)
  signal renameCancelled()
  signal removeRequested()

  implicitHeight: Math.max(buttons.implicitHeight, editing ? field.implicitHeight : details.implicitHeight)

  Column {
    id: details
    visible: !row.editing
    anchors.left: parent.left
    anchors.right: buttons.left
    anchors.rightMargin: Style.space(14)
    anchors.verticalCenter: parent.verticalCenter
    spacing: Style.space(1)

    Text {
      width: parent.width
      elide: Text.ElideRight
      // Names in right-to-left scripts would otherwise sit against the
      // buttons instead of under the section header.
      horizontalAlignment: Text.AlignLeft
      textFormat: Text.PlainText
      text: row.name || "Face"
      color: row.foreground
      font.family: row.fontFamily
      font.pixelSize: Style.font.body
    }

    Text {
      width: parent.width
      elide: Text.ElideRight
      textFormat: Text.PlainText
      text: String(row.created || "").replace("T", "  ").slice(0, 17) + (row.kind === "rgb" ? "  ·  webcam" : "")
      color: row.foreground
      opacity: 0.5
      font.family: row.fontFamily
      font.pixelSize: Style.font.caption
    }
  }

  TextField {
    id: field
    visible: row.editing
    anchors.left: parent.left
    anchors.right: buttons.left
    anchors.rightMargin: Style.space(10)
    anchors.verticalCenter: parent.verticalCenter
    foreground: row.foreground
    accent: row.accent
    font.family: row.fontFamily
    maximumLength: 40
    placeholderText: "Name, e.g. Glasses"
    onVisibleChanged: if (visible) {
      text = row.name
      selectAll()
      forceActiveFocus()
    }
    onAccepted: row.renamed(text)
    Keys.onEscapePressed: function(event) {
      row.renameCancelled()
      event.accepted = true
    }
  }

  Row {
    id: buttons
    anchors.right: parent.right
    anchors.verticalCenter: parent.verticalCenter
    spacing: Style.space(6)

    Button {
      text: row.editing ? "Save" : "Rename"
      bordered: true
      foreground: row.editing ? row.accent : row.foreground
      fontFamily: row.fontFamily
      fontSize: Style.font.bodySmall
      enabled: row.interactive
      onClicked: row.editing ? row.renamed(field.text) : row.renameStarted()
    }

    Button {
      text: "Remove"
      bordered: true
      foreground: row.foreground
      fontFamily: row.fontFamily
      fontSize: Style.font.bodySmall
      enabled: row.interactive && !row.editing
      onClicked: row.removeRequested()
    }
  }
}
