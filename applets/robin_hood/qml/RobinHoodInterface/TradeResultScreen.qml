pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    objectName: "robinTradeResults"
    color: "#0b1514"
    focus: true
    Component.onCompleted: scroller.forceActiveFocus()
    Keys.onEscapePressed: {
        if (RobinTrades.tradeResultsComplete && !RobinTrades.tradeResultsBusy)
            RobinTrades.dismissTradeResults()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12
        Flickable {
            id: scroller
            objectName: "robinResultsScroller"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: contents.implicitHeight
            clip: true
            activeFocusOnTab: true
            KeyNavigation.tab: stopButton.visible ? stopButton : doneButton
            KeyNavigation.backtab: doneButton.enabled ? doneButton : stopButton
            Rectangle {
                parent: scroller
                anchors.fill: parent
                color: "transparent"
                border.color: "#a3e635"
                border.width: 2
                visible: scroller.activeFocus
                z: 1
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_End) {
                    contentY = Math.max(0, contentHeight - height)
                    event.accepted = true
                } else if (event.key === Qt.Key_Home) {
                    contentY = 0
                    event.accepted = true
                }
            }
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: contents
                width: scroller.width
                spacing: 14
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Transaction results")
                    color: "#a3e635"
                    font.pixelSize: 24
                    font.bold: true
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "robinTransactionTitle"
                    Layout.fillWidth: true
                    text: RobinTrades.currentTransactionTitle
                    color: "#f3f9f0"
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                    font.pixelSize: 18
                }
                Label {
                    Layout.fillWidth: true
                    text: RobinTrades.tradeResultsBusy
                          ? qsTr("Processing accounts and tickers, including the final 2–3 second cooldown…")
                          : qsTr("Batch finished. Accepted or queued orders are not confirmed fills.")
                    color: "#abc4b8"
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "robinStopExplanation"
                    Layout.fillWidth: true
                    text: qsTr("Stop remaining orders does not cancel already submitted server orders. Unknown orders may have executed: check Robinhood manually before retrying. No automatic retry is performed.")
                    color: "#ffb6a4"
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: RobinTrades.tradeResults
                    delegate: TradeResultCard {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        result: modelData
                        rowIndex: index
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: RobinTrades.error.length > 0
                    text: RobinTrades.error
                    color: "#ffb6a4"
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
        TradeButton {
            id: stopButton
            objectName: "robinStopRemaining"
            Layout.fillWidth: true
            Layout.minimumHeight: 48
            visible: RobinTrades.tradeResultsBusy
            enabled: RobinTrades.tradeResultsBusy
            text: qsTr("Stop remaining orders")
            onClicked: RobinTrades.cancel()
            KeyNavigation.tab: doneButton.enabled ? doneButton : scroller
        }
        TradeButton {
            id: doneButton
            objectName: "robinTradeDone"
            Layout.fillWidth: true
            Layout.minimumHeight: 48
            enabled: RobinTrades.tradeResultsComplete && !RobinTrades.tradeResultsBusy
            text: qsTr("Done")
            onClicked: RobinTrades.dismissTradeResults()
            KeyNavigation.tab: scroller
        }
    }
}
