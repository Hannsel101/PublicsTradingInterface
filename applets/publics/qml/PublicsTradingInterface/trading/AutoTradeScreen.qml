pragma ComponentBehavior: Bound

import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    signal leaveMode()

    implicitHeight: contentColumn.implicitHeight

    ColumnLayout {
        id: contentColumn
        width: parent.width
        spacing: 18

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
            color: Theme.cardColor
            border.color: Theme.borderColor
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
                    color: Theme.primaryText
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Keys are processed in order. Each session closes after its accounts finish, then the next key starts 3 seconds later. Sell All checks held shares first. Debug builds only run preflight checks.")
                    color: Theme.mutedText
                    wrapMode: Text.WordWrap
                    font.pixelSize: 14
                }
                Label {
                    Layout.fillWidth: true
                    visible: AuthClient.apiKeyIndexLoading
                    text: qsTr("Loading saved keys…")
                    color: Theme.secondaryText
                }
                Label {
                    Layout.fillWidth: true
                    visible: AutoTrader.keyLabels.length === 0 && !AuthClient.apiKeyIndexLoading
                    text: qsTr("No keys saved. Return to the main menu to add an API key.")
                    color: Theme.dangerColor
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
                        color: Theme.elevatedColor
                        border.color: Theme.borderColor
                        Label {
                            anchors.fill: parent
                            anchors.margins: 12
                            text: AuthClient.apiKeyNicknames[keyRow.modelData] || keyRow.modelData
                            elide: Text.ElideRight
                            color: Theme.primaryText
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 15
                        }
                    }
                }
            }
        }

        StockTrade {
            Layout.minimumHeight: implicitHeight
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
