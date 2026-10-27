pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Dialog {
    id: root
    required property var account
    readonly property string updatedAt: account ? account.holdingsUpdatedAt : ""
    readonly property string status: account ? account.holdingsStatus : "unavailable"
    readonly property string snapshotError: account ? account.holdingsError : ""
    objectName: "robinAccountInfoDialog"
    modal: true
    focus: true
    title: qsTr("Account information")
    standardButtons: Dialog.Close
    onOpened: holdingsList.forceActiveFocus(Qt.PopupFocusReason)
    footer: DialogButtonBox {
        background: Rectangle { color: "#142522" }
        delegate: Button {
            id: closeButton
            implicitHeight: 40
            contentItem: Label {
                text: closeButton.text
                color: "#f3f9f0"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: closeButton.down ? "#36534b" : "#244536"
                border.color: closeButton.activeFocus ? "#a3e635" : "#36534b"
                border.width: closeButton.activeFocus ? 2 : 1
            }
        }
    }
    header: Label {
        text: root.title
        color: "#f3f9f0"
        font.pixelSize: 20
        font.bold: true
        padding: 16
        wrapMode: Text.Wrap
    }
    contentItem: ColumnLayout {
        spacing: 10
        Label {
            objectName: "robinInfoIdentity"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            text: root.account ? qsTr("%1 · %2").arg(root.account.displayName).arg(root.account.accountNumber) : ""
            color: "#a3e635"
            font.bold: true
            wrapMode: Text.WrapAnywhere
        }
        Label {
            objectName: "robinInfoSnapshot"
            Layout.fillWidth: true
            text: qsTr("Stored snapshot · Not live data")
            color: "#abc4b8"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "robinInfoUpdated"
            Layout.fillWidth: true
            text: root.updatedAt ? qsTr("Last updated: %1").arg(Qt.formatDateTime(new Date(root.updatedAt), "yyyy-MM-dd hh:mm:ss"))
                                 : qsTr("Never downloaded")
            Accessible.description: root.updatedAt
            color: "#abc4b8"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "robinInfoStatus"
            Layout.fillWidth: true
            text: root.snapshotError ? (root.updatedAt ? qsTr("Refresh failed · Showing stored snapshot") : qsTr("Refresh failed · No stored holdings snapshot"))
                  : root.status === "fresh" ? qsTr("Latest successful sync")
                  : root.status === "cached" ? qsTr("Cached snapshot · May be out of date")
                  : root.status === "stale" ? qsTr("Stale snapshot · May be out of date") : qsTr("Holdings unavailable")
            color: "#abc4b8"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "robinInfoError"
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: text !== ""
            text: root.snapshotError
            color: "#ffb6a4"
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            Accessible.name: text
            ToolTip.text: text
            ToolTip.visible: errorHover.hovered
            HoverHandler { id: errorHover }
        }
        Label {
            objectName: "robinInfoEmpty"
            Layout.fillWidth: true
            visible: holdingsList.count === 0
            text: root.updatedAt ? qsTr("No stock holdings in this stored snapshot")
                                 : qsTr("Account information available after first successful sync")
            color: "#f3f9f0"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "robinInfoHoldingsHeading"
            Layout.fillWidth: true
            text: qsTr("Stock holdings · Shares")
            color: "#f3f9f0"
            wrapMode: Text.Wrap
        }
        ListView {
            id: holdingsList
            objectName: "robinHoldingsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            activeFocusOnTab: true
            Accessible.name: qsTr("Stored stock holdings")
            Keys.onPressed: event => {
                if (event.key === Qt.Key_End) {
                    positionViewAtEnd()
                    event.accepted = true
                } else if (event.key === Qt.Key_Home) {
                    positionViewAtBeginning()
                    event.accepted = true
                }
            }
            clip: true
            spacing: 10
            model: root.account ? root.account.holdings : []
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Rectangle {
                objectName: "robinHoldingsFocus"
                parent: holdingsList
                anchors.fill: parent
                visible: holdingsList.activeFocus
                color: "#00000000"
                border.color: "#a3e635"
                border.width: 2
                radius: 4
                Accessible.ignored: true
            }
            delegate: ColumnLayout {
                id: holdingRow
                required property int index
                required property var model
                readonly property var holding: model.modelData || model
                width: holdingsList.width - 16
                spacing: 4
                Label {
                    objectName: "robinHoldingSymbol_" + holdingRow.index
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: holdingRow.holding.symbol
                    color: "#f3f9f0"
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    objectName: "robinHoldingQuantity_" + holdingRow.index
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: holdingRow.holding.quantity
                    Accessible.name: qsTr("%1 shares: %2").arg(holdingRow.holding.symbol).arg(text)
                    color: "#abc4b8"
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
    background: Rectangle {
        radius: 16
        color: "#142522"
        border.color: "#36534b"
    }
}
