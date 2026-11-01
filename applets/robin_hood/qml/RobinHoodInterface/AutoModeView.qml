pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

FocusScope {
    id: root
    objectName: "robinAutoModeView"
    required property var trades
    property double presentationTimeMs: Date.now()
    readonly property bool hasResults: trades !== null && trades.tradeResults.length > 0
    readonly property bool busy: trades !== null && trades.busy
    signal backRequested()
    Timer {
        objectName: "robinAutoExpiryTimer"
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: root.presentationTimeMs = Date.now()
    }
    Rectangle { anchors.fill: parent; color: "#0b1514" }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Auto Mode")
                color: "#f3f9f0"
                font.pixelSize: 28
                font.bold: true
            }
            TradeButton {
                objectName: "robinAutoBack"
                text: qsTr("Sessions")
                enabled: !root.busy
                onClicked: root.backRequested()
            }
        }
        Label {
            objectName: "robinAutoExecutionMode"
            Layout.fillWidth: true
            text: root.trades && root.trades.liveOrdersEnabled ? qsTr("LIVE ORDERS") : qsTr("Debug preview · No orders sent")
            color: root.trades && root.trades.liveOrdersEnabled ? "#a3e635" : "#f6d365"
            wrapMode: Text.Wrap
            font.bold: true
        }
        TextField {
            id: input
            objectName: "robinAutoTickers"
            Layout.fillWidth: true
            Layout.minimumHeight: 48
            enabled: !root.busy
            placeholderText: qsTr("AAPL or AAPL, TSLA")
            Accessible.name: qsTr("Ticker symbols, comma-separated")
            selectByMouse: true
            font.pixelSize: 18
            color: "#f3f9f0"
            placeholderTextColor: "#abc4b8"
            leftPadding: 12
            rightPadding: 12
            readonly property bool acceptableSymbols: text.trim().length > 0 && !/[^A-Za-z0-9.,\s-]/.test(text)
            onTextEdited: {
                const normalized = text.replace(/[a-z]/g, character => character.toUpperCase())
                if (normalized === text) return
                const caret = cursorPosition
                const selected = selectionStart !== selectionEnd
                const anchor = caret === selectionStart ? selectionEnd : selectionStart
                text = normalized
                if (selected) select(anchor, caret)
                else cursorPosition = caret
            }
            background: Rectangle {
                color: "#142522"
                radius: 12
                border.color: input.activeFocus ? "#a3e635" : "#36534b"
                border.width: input.activeFocus ? 2 : 1
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            TradeButton {
                objectName: "robinAutoBuy"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumHeight: 52
                text: qsTr("Buy 1 Share")
                enabled: root.trades !== null && root.trades.available && !root.busy && input.acceptableSymbols
                onClicked: root.trades.submit(input.text, "buy")
            }
            TradeButton {
                objectName: "robinAutoSell"
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumHeight: 52
                text: qsTr("Sell All Shares")
                enabled: root.trades !== null && root.trades.available && !root.busy && input.acceptableSymbols
                onClicked: root.trades.submit(input.text, "sell")
            }
        }
        TradeButton {
            objectName: "robinAutoCancel"
            visible: root.busy
            text: qsTr("Stop remaining")
            onClicked: root.trades.cancel()
        }
        Label {
            Layout.fillWidth: true
            visible: root.busy
            text: qsTr("Stopping does not cancel orders already sent.")
            color: "#abc4b8"
            wrapMode: Text.Wrap
        }
        Label {
            objectName: "robinAutoError"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.trades ? root.trades.error : qsTr("Auto Mode unavailable")
            textFormat: Text.PlainText
            color: "#ef8175"
            wrapMode: Text.Wrap
        }
        RowLayout {
            visible: root.hasResults
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: root.trades ? root.trades.currentTransactionTitle : ""
                textFormat: Text.PlainText
                color: "#f3f9f0"
                wrapMode: Text.Wrap
            }
            TradeButton {
                objectName: "robinAutoClearResults"
                text: qsTr("Clear results")
                enabled: !root.busy
                onClicked: root.trades.clearResults()
            }
        }
        AutoModeResults {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 80
            visible: root.hasResults
            model: root.trades ? root.trades.tradeResults : []
        }
        ListView {
            id: sessions
            visible: !root.hasResults
            objectName: "robinAutoSessions"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 90
            clip: true
            spacing: 8
            model: RobinSessions
            activeFocusOnTab: true
            Accessible.name: qsTr("Saved login sessions, read only")
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: session
                required property int sessionIndex
                required property string accountName
                required property string expiresAt
                readonly property double expirationMs: Date.parse(expiresAt)
                readonly property bool active: isFinite(expirationMs) && expirationMs > root.presentationTimeMs
                width: sessions.width
                height: 88
                radius: 14
                color: "#142522"
                border.color: sessions.activeFocus ? "#a3e635" : "#36534b"
                Accessible.role: Accessible.ListItem
                Accessible.name: accountName
                Accessible.description: status.text
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6
                    Label {
                        objectName: "robinAutoSessionName_" + session.sessionIndex
                        Layout.fillWidth: true
                        text: session.accountName
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: "#f3f9f0"
                        font.pixelSize: 16
                    }
                    Rectangle {
                        Layout.preferredWidth: status.implicitWidth + 16
                        Layout.preferredHeight: 28
                        color: session.active ? "#a3e635" : "#ef8175"
                        radius: 8
                        Label {
                            id: status
                            objectName: "robinAutoSessionStatus_" + session.sessionIndex
                            anchors.centerIn: parent
                            text: session.active ? qsTr("Active") : qsTr("Re-Authentication Required")
                            color: "#0b1514"
                            font.pixelSize: 13
                            font.bold: true
                        }
                    }
                }
            }
        }
    }
}
