import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    spacing: 12

    Label {
        objectName: "robinExecutionMode"
        Layout.fillWidth: true
        text: RobinTrades.liveOrdersEnabled ? qsTr("LIVE ORDERS · Release build") : qsTr("PREVIEW ONLY · Debug build · No orders sent")
        color: "#a3e635"
        wrapMode: Text.Wrap
        font.bold: true
    }
    TextField {
        id: input
        objectName: "robinTickerInput"
        Layout.fillWidth: true
        Layout.minimumHeight: 52
        placeholderText: qsTr("AAPL or AAPL, TSLA, GM")
        Accessible.name: qsTr("Ticker symbols, comma-separated")
        selectByMouse: true
        onTextEdited: {
            const uppercase = input.text.replace(/[a-z]/g, character => character.toUpperCase())
            if (uppercase === input.text) return
            const caret = input.cursorPosition
            const selected = input.selectionStart !== input.selectionEnd
            const anchor = caret === input.selectionStart ? input.selectionEnd : input.selectionStart
            input.text = uppercase
            if (selected) input.select(anchor, caret)
            else input.cursorPosition = caret
        }
        enabled: !RobinTrades.tradeResultsBusy
        color: "#f3f9f0"
        placeholderTextColor: "#abc4b8"
        font.pixelSize: 18
        leftPadding: 12
        rightPadding: 12
        background: Rectangle {
            radius: 14
            color: "#142522"
            border.color: input.activeFocus ? "#a3e635" : "#36534b"
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 12
        TradeButton {
            objectName: "robinBuyButton"
            Layout.fillWidth: true
            Layout.minimumHeight: 52
            text: qsTr("Buy 1 each")
            enabled: RobinTrades.available && !RobinTrades.tradeResultsBusy && input.text.trim().length > 0
            onClicked: RobinTrades.submit(input.text, "buy")
        }
        TradeButton {
            objectName: "robinSellButton"
            Layout.fillWidth: true
            Layout.minimumHeight: 52
            text: qsTr("Sell All")
            enabled: RobinTrades.available && !RobinTrades.tradeResultsBusy && input.text.trim().length > 0
            onClicked: RobinTrades.submit(input.text, "sell")
        }
    }
    Label {
        objectName: "robinTradesUnavailable"
        Layout.fillWidth: true
        visible: !RobinTrades.available
        text: RobinTrades.unavailableReason
        textFormat: Text.PlainText
        color: "#ffb6a4"
        wrapMode: Text.Wrap
    }
    Label {
        objectName: "robinTradeError"
        Layout.fillWidth: true
        visible: text.length > 0
        text: RobinTrades.error
        textFormat: Text.PlainText
        color: "#ffb6a4"
        wrapMode: Text.Wrap
    }
}
