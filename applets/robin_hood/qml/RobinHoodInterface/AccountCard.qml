pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    required property string displayName
    required property string accountNumber
    required property string currentBalance
    property var holdings: []
    property string holdingsUpdatedAt: ""
    property string holdingsStatus: "unavailable"
    property string holdingsError: ""
    signal informationRequested(var account)
    objectName: "robinAccountTile_" + accountNumber
    implicitHeight: details.implicitHeight + 28
    radius: 16
    color: "#142522"
    border.color: "#36534b"
    RowLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8
        ColumnLayout {
            id: details
            Layout.fillWidth: true
            spacing: 6
            Label {
                objectName: "robinAccountName_" + root.accountNumber
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.displayName
                color: "#a3e635"
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("ACCOUNT  /  %1").arg(root.accountNumber || qsTr("Unavailable"))
                textFormat: Text.PlainText
                color: "#abc4b8"
                wrapMode: Text.WrapAnywhere
            }
            Label {
                objectName: "robinAccountBalance_" + root.accountNumber
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Balance: %1").arg(root.currentBalance)
                color: "#abc4b8"
                wrapMode: Text.Wrap
            }
        }
        Button {
            id: info
            objectName: "robinAccountInfo_" + root.accountNumber
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40
            Layout.alignment: Qt.AlignTop
            icon.source: Qt.resolvedUrl("assets/account-info.svg")
            icon.width: 18
            icon.height: 18
            icon.color: "#a3e635"
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Account information for %1 %2").arg(root.displayName).arg(root.accountNumber)
            ToolTip.text: Accessible.name
            ToolTip.visible: hovered || activeFocus
            ToolTip.delay: 400
            onClicked: root.informationRequested(root)
            background: Rectangle {
                radius: 10
                color: info.down ? "#36534b" : "#244536"
                border.color: info.activeFocus ? "#a3e635" : "#36534b"
                border.width: info.activeFocus ? 2 : 1
            }
        }
    }
}
