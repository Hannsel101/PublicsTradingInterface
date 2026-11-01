pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ListView {
    id: root
    objectName: "robinAutoResults"
    clip: true
    spacing: 8
    activeFocusOnTab: true
    Accessible.name: qsTr("Auto Mode results")
    ScrollBar.vertical: ScrollBar {}
    delegate: Rectangle {
        id: card
        required property int index
        required property var modelData
        readonly property bool preview: modelData.preview === true
        readonly property bool sent: modelData.success === true && !preview
        readonly property bool pending: modelData.status === "pending" && !preview && !sent
        width: root.width
        height: content.implicitHeight + 24
        radius: 14
        color: "#142522"
        border.color: root.activeFocus ? "#a3e635" : "#36534b"
        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 12
            spacing: 6
            Label {
                objectName: "robinAutoResultIdentity_" + card.index
                Layout.fillWidth: true
                text: qsTr("%1 · %2 · %3 · %4").arg(card.modelData.sessionName || "").arg(card.modelData.accountNumber || "").arg(card.modelData.symbol || "").arg(card.modelData.side || "")
                textFormat: Text.PlainText
                color: "#f3f9f0"
                wrapMode: Text.WrapAnywhere
            }
            Label {
                objectName: "robinAutoResultStatus_" + card.index
                Layout.fillWidth: true
                text: card.preview ? qsTr("Debug preview · No order sent")
                    : card.pending ? qsTr("Waiting / Submitting")
                    : card.sent ? qsTr("Sent successfully · Not fill-confirmed")
                    : qsTr("Not sent successfully · %1").arg(card.modelData.status || "")
                textFormat: Text.PlainText
                color: card.preview ? "#f6d365" : card.pending ? "#abc4b8" : card.sent ? "#a3e635" : "#ef8175"
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                objectName: "robinAutoResultMessage_" + card.index
                Layout.fillWidth: true
                text: card.modelData.message || ""
                textFormat: Text.PlainText
                color: "#abc4b8"
                wrapMode: Text.WrapAnywhere
            }
        }
    }
}
