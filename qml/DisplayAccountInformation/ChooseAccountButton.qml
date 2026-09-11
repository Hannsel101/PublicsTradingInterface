import QtQuick
import QtQuick.Controls.Basic

Button {
    id: chooseAccountButton

    text: qsTr("Switch key")
    visible: AuthClient.sessionActive
    implicitWidth: 180
    implicitHeight: 46

    contentItem: Text {
        text: chooseAccountButton.text
        color: "#f7f8f8"
        font.pixelSize: 14
        font.weight: Font.DemiBold
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: 14
        color: chooseAccountButton.down ? "#25285f" : chooseAccountButton.hovered ? "#202126" : "#17181c"
        border.color: chooseAccountButton.hovered ? "#7170ff" : "#2f3138"
        border.width: 1
    }

    onClicked: {
        AuthClient.sessionActive = false
        StockSearchController.tokenActive = false
        ApiWorker.clearSubAccountsList()
    }
}
