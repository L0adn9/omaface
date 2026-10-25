import QtQuick
import qs.Commons
import qs.Commons as Commons
import qs.Ui

// A compact numeric setting: one label, one value and one slider.
// Detail/range/default copy is deliberately not rendered; the bounds are
// still enforced here and by the engine.
Item {
  id: root

  property string settingKey: ""
  property string title: ""
  property string detail: ""
  property real value: 0
  property real defaultValue: 0
  property var bounds: ({ min: 0, max: 1, step: 0.01, integer: false })
  property bool reducedSecurity: false
  property color foreground: Commons.Color.menu.text
  // Settings passes a colour that stands out in the current theme.
  property color warnColor: Commons.Color.urgent
  property color accent: Commons.Color.accent
  property string fontFamily: Style.font.menuFamily
  property string inputError: ""
  property string note: ""

  signal edited(real value)
  signal focused(var item)

  implicitHeight: content.implicitHeight
  height: implicitHeight
  readonly property real minimum: Number(bounds.min)
  readonly property real maximum: Number(bounds.max)
  readonly property real step: Number(bounds.step)
  readonly property bool integer: bounds.integer === true

  function snap(next) {
    var n = minimum + Math.round((next - minimum) / step) * step
    return Math.max(minimum, Math.min(maximum, Number(n.toFixed(8))))
  }

  function change(next, quantize) {
    if (!enabled || !isFinite(next)) return
    var n = quantize ? snap(next) : next
    if (n < minimum || n > maximum || (integer && Math.floor(n) !== n)) return
    inputError = ""
    if (n !== value) edited(n)
  }

  function adjust(key) {
    if (key === Qt.Key_Home) change(minimum, true)
    else if (key === Qt.Key_End) change(maximum, true)
    else if (key === Qt.Key_Right || key === Qt.Key_Up) change(value + step, true)
    else if (key === Qt.Key_Left || key === Qt.Key_Down) change(value - step, true)
    else return false
    return true
  }

  onValueChanged: {
    if (!decimalField.activeFocus) decimalField.text = String(value)
  }

  Column {
    id: content
    width: parent.width
    spacing: Style.space(2)

    Item {
      width: parent.width
      height: Math.max(labelText.implicitHeight, input.implicitHeight)

      Text {
        id: labelText
        anchors.left: parent.left
        anchors.right: input.left
        anchors.rightMargin: Style.space(6)
        anchors.verticalCenter: parent.verticalCenter
        text: root.reducedSecurity ? "⚠ " + root.title : root.title
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: root.reducedSecurity ? root.warnColor : root.foreground
        font.bold: root.reducedSecurity
        font.family: root.fontFamily
        font.pixelSize: Style.font.bodySmall
      }

      Item {
        id: input
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: root.integer ? integerField.width : decimalField.width
        height: Math.max(integerField.implicitHeight, decimalField.implicitHeight)

        NumberField {
          id: integerField
          objectName: root.integer ? "number_" + root.settingKey : ""
          visible: root.integer
          width: Style.space(54)
          fieldWidth: width
          spacing: 0
          foreground: root.reducedSecurity ? root.warnColor : root.foreground
          accent: root.accent
          fontFamily: root.fontFamily
          fontSize: Style.font.bodySmall
          value: Math.round(root.value)
          from: Math.ceil(root.minimum)
          to: Math.floor(root.maximum)
          stepSize: Math.max(1, Math.round(root.step))
          onModified: function(next) { root.change(next, false) }
          Connections {
            target: integerField.field
            function onActiveFocusChanged() {
              if (integerField.field.activeFocus) root.focused(root)
            }
          }
        }

        TextField {
          id: decimalField
          objectName: root.integer ? "" : "number_" + root.settingKey
          visible: !root.integer
          width: Style.space(58)
          height: Style.spacing.controlHeight
          foreground: root.reducedSecurity ? root.warnColor : root.foreground
          accent: root.accent
          font.family: root.fontFamily
          font.pixelSize: Style.font.bodySmall
          horizontalAlignment: TextInput.AlignHCenter
          verticalAlignment: TextInput.AlignVCenter
          inputMethodHints: Qt.ImhFormattedNumbersOnly
          selectByMouse: true
          // A fixed control height with the kit's input padding clips the
          // text when the font renders taller; centre it in the full height.
          verticalPadding: 0
          text: String(root.value)
          property bool editedText: false
          // Qt only reports editingFinished for acceptable input, so an
          // out-of-range entry is also settled on focus loss and Enter.
          // Either way the field ends up showing the draft value.
          function commit() {
            if (editedText) {
              if (acceptableInput && text.trim() !== "") root.change(Number(text), false)
              else root.inputError = "Out of range"
            }
            editedText = false
            text = String(root.value)
          }
          validator: DoubleValidator {
            bottom: root.minimum
            top: root.maximum
            decimals: 8
            notation: DoubleValidator.StandardNotation
            locale: "C"
          }
          onTextEdited: editedText = true
          onActiveFocusChanged: {
            if (activeFocus) root.focused(root)
            else commit()
          }
          onEditingFinished: commit()
          Keys.onPressed: function(event) {
            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !acceptableInput) {
              commit()
              event.accepted = true
              return
            }
            if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
              root.adjust(event.key)
              text = String(root.value)
              editedText = false
              event.accepted = true
            }
          }
        }
      }
    }

    BorderSurface {
      id: sliderFocus
      objectName: "slider_" + root.settingKey
      width: parent.width
      height: slider.implicitHeight + Style.space(4)
      radius: Style.cornerRadius
      color: activeFocus ? Style.focusFillFor(root.foreground, root.accent) : "transparent"
      borderSpec: activeFocus ? Border.controlSpec("focus", root.foreground, root.accent) : Border.none()
      activeFocusOnTab: true
      onActiveFocusChanged: if (activeFocus) root.focused(root)
      Keys.onPressed: function(event) {
        if (root.adjust(event.key)) event.accepted = true
      }

      PanelSlider {
        id: slider
        anchors.fill: parent
        anchors.margins: Style.space(2)
        value: root.value
        minimum: root.minimum
        maximum: root.maximum
        step: root.step
        integer: root.integer
        trackColor: Style.selectedFillFor(root.foreground, root.accent)
        fillColor: root.accent
        knobColor: root.foreground
        onMoved: function(next) {
          sliderFocus.forceActiveFocus()
          liveValue = root.snap(next)
          root.change(next, true)
        }
        onReleased: function(next) { root.change(next, true) }
      }
    }

    Text {
      width: parent.width
      visible: root.note !== "" && root.inputError === ""
      text: root.note
      textFormat: Text.PlainText
      wrapMode: Text.WordWrap
      color: root.foreground
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }

    Text {
      width: parent.width
      visible: root.inputError !== ""
      text: root.inputError
      textFormat: Text.PlainText
      color: root.warnColor
      font.family: root.fontFamily
      font.pixelSize: Style.font.caption
    }
  }
}
