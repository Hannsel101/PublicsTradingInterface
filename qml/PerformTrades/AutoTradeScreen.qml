pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    signal leaveMode()

    implicitHeight: contentColumn.implicitHeight
    readonly property bool compactLayout: width < 640

    ColumnLayout {
        id: contentColumn
        width: parent.width
        spacing: 18

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("Auto Mode")
                    color: "#f7f8f8"
                    font.pixelSize: root.compactLayout ? 24 : 30
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("One action across every saved key and its eligible brokerage accounts. No key selection required.")
                    color: "#8a8f98"
                    font.pixelSize: 14
                    wrapMode: Text.WordWrap
                }
            }

            Button {
                id: backButton
                enabled: !AutoTrader.tradeResultsBusy
                text: qsTr("Main menu")
                onClicked: root.leaveMode()
                contentItem: Text {
                    text: backButton.text
                    color: backButton.enabled ? "#f7f8f8" : "#8a8f98"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 14
                    color: "#202126"
                    border.color: "#2f3138"
                }
            }
        }

        TradeResultScreen {
            Layout.fillWidth: true
            Layout.preferredHeight: 520
            Layout.minimumHeight: visible ? 520 : 0
            visible: AutoTrader.tradeResultsVisible
            resultController: AutoTrader
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: keyColumn.implicitHeight + 36
            radius: 24
            color: "#17181c"
            border.color: "#2f3138"
            visible: !AutoTrader.tradeResultsVisible
            Layout.minimumHeight: visible ? implicitHeight : 0

            ColumnLayout {
                id: keyColumn
                anchors.fill: parent
                anchors.margins: 18
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Saved API keys · %1").arg(AutoTrader.keyLabels.length)
                    color: "#f7f8f8"
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Keys are processed in order. Each session closes after its accounts finish, then the next key starts 3 seconds later. Sell All checks held shares first. Debug builds only run preflight checks.")
                    color: "#8a8f98"
                    wrapMode: Text.WordWrap
                    font.pixelSize: 14
                }
                Label {
                    Layout.fillWidth: true
                    visible: AuthClient.apiKeyIndexLoading
                    text: qsTr("Loading saved keys…")
                    color: "#d0d6e0"
                }
                Label {
                    Layout.fillWidth: true
                    visible: AutoTrader.keyLabels.length === 0 && !AuthClient.apiKeyIndexLoading
                    text: qsTr("No keys saved. Return to the main menu to add an API key.")
                    color: "#ff5c7a"
                    wrapMode: Text.WordWrap
                }
                ListView {
                    id: keyList
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(72, Math.min(220, contentHeight + 12))
                    clip: true
                    spacing: 8
                    model: AutoTrader.keyLabels
                    delegate: Rectangle {
                        id: keyRow
                        required property string modelData
                        width: keyList.width
                        height: 48
                        radius: 12
                        color: "#202126"
                        border.color: "#2f3138"
                        Label {
                            anchors.fill: parent
                            anchors.margins: 12
                            text: keyRow.modelData
                            color: "#f7f8f8"
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 15
                        }
                    }
                }
            }
        }

        StockTrade {
            id: orderEntry
            Layout.fillWidth: true
            Layout.preferredHeight: 320
            visible: !AutoTrader.tradeResultsVisible
            enabled: AutoTrader.keyLabels.length > 0 && !AuthClient.apiKeyIndexLoading && !AutoTrader.tradeResultsBusy
            opacity: enabled ? 1 : 0.6
            onPerformBuy: AutoTrader.submit(inputFieldText, "BUY")
            onPerformSell: AutoTrader.submit(inputFieldText, "SELL")
        }
    }
}
