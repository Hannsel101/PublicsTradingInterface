import QtQuick
import QtQuick.Controls.Basic
import TelegramBotsInterface

Button {
    id: button
    required property color actionColor
    implicitHeight: 48
    implicitWidth: Math.max(120, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: 16
    rightPadding: 16
    font.pixelSize: 16
    font.weight: Font.DemiBold
    background: Rectangle {
        radius: 12
        color: !button.enabled ? Theme.elevatedColor
               : button.down ? Qt.darker(button.actionColor, 1.12)
               : button.hovered ? Qt.lighter(button.actionColor, 1.1) : button.actionColor
        border.width: button.visualFocus ? 2 : 1
        border.color: button.visualFocus ? Theme.accentColor
                      : button.enabled ? Qt.lighter(button.actionColor, 1.2) : Theme.borderColor
    }
    contentItem: Text {
        text: button.text
        font: button.font
        color: button.enabled ? Theme.primaryText : Theme.mutedText
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
