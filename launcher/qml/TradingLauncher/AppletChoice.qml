import QtQuick
import QtQuick.Controls.Basic

Button {
    id: choice
    required property color stripeColor
    property string appletName: ""
    text: appletName
    height: 76
    implicitHeight: 76
    Accessible.name: appletName + (enabled ? "" : qsTr(" (coming soon)"))

    background: Rectangle {
        radius: 10
        clip: true
        color: !choice.enabled ? "#18191d" : choice.pressed ? "#292b31" : "#202126"
        border.color: !choice.enabled ? "#35383f" : choice.activeFocus ? "#d0d6e0" : "#42444d"
        border.width: choice.activeFocus ? 2 : 1
        Rectangle {
            width: 16
            height: parent.height
            x: parent.width * 0.7 - width / 2
            color: choice.stripeColor
            opacity: choice.enabled ? 1 : 0.65
        }
    }
    contentItem: Text {
        text: choice.text
        color: choice.enabled ? "#f7f8f8" : "#9299a3"
        font: choice.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
