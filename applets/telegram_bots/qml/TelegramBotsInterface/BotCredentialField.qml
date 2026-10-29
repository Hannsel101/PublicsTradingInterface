import QtQuick
import QtQuick.Controls.Basic
import TelegramBotsInterface

TextField {
    id: field
    implicitHeight: 48
    padding: 12
    font.pixelSize: 16
    color: Theme.primaryText
    placeholderTextColor: Theme.mutedText
    selectionColor: Theme.accentColor
    selectedTextColor: Theme.backgroundColor
    background: Rectangle {
        radius: 10
        color: Theme.elevatedColor
        border.width: field.activeFocus ? 2 : 1
        border.color: field.activeFocus ? Theme.accentColor : Theme.borderColor
    }
}
