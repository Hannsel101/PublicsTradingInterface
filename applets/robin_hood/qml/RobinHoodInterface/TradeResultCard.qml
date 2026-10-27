import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    required property var result
    required property int rowIndex
    readonly property string status: result.status || "pending"
    readonly property string statusLabel: {
        switch (status) {
        case "submitted": return qsTr("Submitted · Accepted/queued, not filled")
        case "filled": return qsTr("Filled")
        case "failed": return qsTr("Failed")
        case "unknown": return qsTr("Unknown · Check Robinhood before retrying")
        case "cancelled": return qsTr("Cancelled · Not submitted")
        case "skipped": return qsTr("Skipped · No sellable shares")
        case "preview": return qsTr("Preview · No order sent")
        default: return qsTr("Pending")
        }
    }
    readonly property color statusColor: status === "filled" ? "#a3e635"
                                       : status === "failed" || status === "unknown" ? "#ffb6a4" : "#55cbb4"
    implicitHeight: content.implicitHeight + 28
    radius: 16
    color: "#142522"
    border.color: statusColor
    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8
        ResultText {
            objectName: "robinResultAccount_" + root.rowIndex
            Layout.fillWidth: true
            text: qsTr("%1 · %2").arg(root.result.accountLabel || qsTr("Account")).arg(root.result.accountNumber || "")
            font.bold: true
        }
        ResultText {
            objectName: "robinResultSymbol_" + root.rowIndex
            Layout.fillWidth: true
            text: root.result.symbol || ""
            font.pixelSize: 18
            font.bold: true
        }
        ResultText {
            objectName: "robinResultSide_" + root.rowIndex
            Layout.fillWidth: true
            text: root.result.side === "buy" ? qsTr("Buy · Shares") : qsTr("Sell · Shares")
            color: "#abc4b8"
        }
        ResultText {
            objectName: "robinResultQuantity_" + root.rowIndex
            Layout.fillWidth: true
            text: root.result.quantity || ""
        }
        ResultText {
            objectName: "robinResultStatus_" + root.rowIndex
            Layout.fillWidth: true
            text: root.statusLabel
            color: root.statusColor
            font.bold: true
        }
        ResultText {
            objectName: "robinResultMessage_" + root.rowIndex
            Layout.fillWidth: true
            text: root.result.message || ""
        }
        ResultText {
            objectName: "robinResultOrderId_" + root.rowIndex
            Layout.fillWidth: true
            visible: !!root.result.orderId
            text: qsTr("Order ID: %1").arg(root.result.orderId || "")
            color: "#abc4b8"
        }
        ResultText {
            objectName: "robinResultRefId_" + root.rowIndex
            Layout.fillWidth: true
            visible: !!root.result.refId
            text: qsTr("Reference ID: %1").arg(root.result.refId || "")
            color: "#abc4b8"
        }
    }
}
