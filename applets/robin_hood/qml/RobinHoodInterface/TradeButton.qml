import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root
    implicitHeight: 48
    palette.buttonText: enabled ? "#0b1514" : "#abc4b8"
    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        color: root.enabled ? "#0b1514" : "#abc4b8"
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        radius: 14
        color: root.enabled ? (root.down ? "#83bd2c" : "#a3e635") : "#31403a"
        border.color: root.activeFocus ? "#f3f9f0" : "#36534b"
        border.width: root.activeFocus ? 2 : 1
    }
}
