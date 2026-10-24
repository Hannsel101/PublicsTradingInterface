import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic

Button {
    id: chooseAccountButton

    text: qsTr("Switch key")
    visible: AuthClient.sessionActive
    implicitWidth: 180
    implicitHeight: 46

    contentItem: Text {
        text: chooseAccountButton.text
        color: Theme.primaryText
        font.pixelSize: 14
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 14
        color: chooseAccountButton.down ? Theme.selectedColor : chooseAccountButton.hovered ? Theme.elevatedColor : Theme.cardColor
        border.color: chooseAccountButton.hovered ? Theme.accentColor : Theme.borderColor
        border.width: 1
    }

    onClicked: {
        AuthClient.clearUserSession()

        ApiWorker.clearSubAccountsList()
    }
}
