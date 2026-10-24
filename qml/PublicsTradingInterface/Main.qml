import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "credentials"
import "trading"

ApplicationWindow {
    id: window

    width: 1000
    height: 700
    minimumWidth: 360
    minimumHeight: 620
    visible: true
    title: qsTr("Publics Brokerage API Multi-Account Trader")
    color: Theme.backgroundColor

    property bool autoMode: false
    readonly property bool compactLayout: width < 720
    readonly property bool keySelected: AuthClient.selectedApiKeyLabel !== ""
    readonly property bool canStartSession: window.keySelected && AuthClient.apiKeyReady && !AuthClient.apiKeyLoading && !AuthClient.apiKeyIndexLoading && !AuthClient.sessionActive
    function displayKey(label) { return AuthClient.apiKeyNicknames[label] || label }
    readonly property real pageMargin: Math.max(18, Math.min(width * 0.06, 56))
    readonly property real cardRadius: 24


    Rectangle {
        anchors.fill: parent
        color: Theme.backgroundColor

        Rectangle {
            width: Math.min(parent.width * 0.72, 720)
            height: width
            radius: width / 2
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: -height * 0.45
            color: Theme.glowColor
            opacity: 0.35
        }
    }

    Flickable {
        id: page
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + (window.pageMargin * 2)

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: contentColumn
            x: window.pageMargin
            y: window.pageMargin
            width: page.width - (window.pageMargin * 2)
            spacing: window.compactLayout ? 18 : 24

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: qsTr("PUBLICS TRADING")
                        color: Theme.accentColor
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.2
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        Label {
                            text: window.autoMode ? qsTr("All-Key Brokerage Console") : qsTr("Multi-Account Brokerage Console")
                            color: Theme.primaryText
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            font.pixelSize: window.compactLayout ? 30 : 42
                            font.weight: Font.DemiBold
                        }

                        Button {
                            id: backButton
                            Layout.alignment: Qt.AlignVCenter
                            visible: window.autoMode
                            enabled: !AutoTrader.tradeResultsBusy
                            text: qsTr("Main menu")
                            onClicked: autoScreen.leaveMode()
                            contentItem: Text {
                                text: backButton.text
                                color: backButton.enabled ? Theme.primaryText : Theme.mutedText
                                font.pixelSize: 14
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 14
                                color: Theme.elevatedColor
                                border.color: Theme.borderColor
                            }
                        }
                    }
                }
            }

            TradeResultScreen {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(500, page.height - contentColumn.y - 140)
                visible: !window.autoMode && ApiWorker.tradeResultsVisible
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: sessionTimer.implicitHeight + 32
                radius: window.cardRadius
                color: Theme.panelColor
                border.color: Theme.borderColor
                border.width: 1
                visible: !window.autoMode && AuthClient.sessionActive && !ApiWorker.tradeResultsVisible
                Layout.minimumHeight: visible ? implicitHeight : 0

                SessionTimer {
                    id: sessionTimer
                    anchors.fill: parent
                    anchors.margins: 16
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: visible ? (AuthClient.sessionActive ? tradeColumn.implicitHeight + 44 : keyColumn.implicitHeight + 44) : 0
                radius: window.cardRadius
                color: Theme.cardColor
                border.color: Theme.borderColor
                border.width: 1
                visible: !window.autoMode && !ApiWorker.tradeResultsVisible

                ColumnLayout {
                    id: keyColumn
                    visible: !AuthClient.sessionActive
                    anchors.fill: parent
                    anchors.margins: window.compactLayout ? 18 : 24
                    spacing: 18

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                text: qsTr("Choose or store an API key")
                                color: Theme.primaryText
                                font.pixelSize: window.compactLayout ? 22 : 26
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    ApiKeyList {

                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(220, Math.min(340, window.height * 0.30))
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: sessionControlLayout.implicitHeight + 28
                        radius: 18
                        color: Theme.elevatedColor
                        border.color: window.keySelected ? Theme.accentColor : Theme.borderColor
                        border.width: window.keySelected ? 2 : 1

                        ColumnLayout {
                            id: sessionControlLayout
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12

                            Label {
                                Layout.fillWidth: true
                                text: !window.keySelected
                                      ? qsTr("Select a PublicsApiKey entry to start a session")
                                      : AuthClient.apiKeyLoading
                                        ? qsTr("Loading %1 from the system keychain…").arg(window.displayKey(AuthClient.selectedApiKeyLabel))
                                        : AuthClient.apiKeyReady
                                          ? qsTr("%1 is ready").arg(window.displayKey(AuthClient.selectedApiKeyLabel))
                                          : qsTr("%1 is selected but not ready").arg(window.displayKey(AuthClient.selectedApiKeyLabel))
                                color: AuthClient.apiKeyReady ? Theme.successColor : Theme.secondaryText
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: AuthClient.apiKeyError !== ""
                                text: AuthClient.apiKeyError
                                color: Theme.dangerColor
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }

                            Button {
                                id: startSessionButton
                                Layout.fillWidth: true
                                Layout.minimumHeight: 50
                                enabled: window.canStartSession
                                text: AuthClient.apiKeyLoading ? qsTr("Loading key…") : qsTr("Start session and load accounts")

                                contentItem: Text {
                                    text: startSessionButton.text
                                    color: startSessionButton.enabled ? "white" : Theme.mutedText
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }

                                background: Rectangle {
                                    radius: 14
                                    color: !startSessionButton.enabled ? Theme.elevatedColor : startSessionButton.down ? Theme.accentPressed : Theme.accentColor
                                    opacity: startSessionButton.enabled ? 1 : 0.75
                                }

                                onClicked: AuthClient.requestToken()
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: storeKeyLayout.implicitHeight + 28
                        radius: 18
                        color: Theme.elevatedColor
                        border.color: Theme.borderColor
                        border.width: 1

                        ColumnLayout {
                            id: storeKeyLayout
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12

                            Label {
                                text: qsTr("Add a new key")
                                color: Theme.secondaryText
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                TextField {
                                    id: userInputField
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: 48
                                    placeholderText: qsTr("Paste Public.com API secret")
                                    echoMode: TextInput.Password
                                    selectByMouse: true
                                    font.pixelSize: 15
                                    color: Theme.primaryText
                                    placeholderTextColor: Theme.mutedText
                                    leftPadding: 16
                                    rightPadding: 16

                                    background: Rectangle {
                                        radius: 14
                                        color: Theme.surfaceColor
                                        border.color: userInputField.activeFocus ? Theme.accentColor : Theme.borderColor
                                        border.width: userInputField.activeFocus ? 2 : 1
                                    }

                                    onAccepted: storeKeyButton.clicked()
                                }

                                Button {
                                    id: storeKeyButton
                                    Layout.preferredWidth: window.compactLayout ? 116 : 150
                                    Layout.minimumHeight: 48
                                    enabled: userInputField.text.trim() !== "" && !AuthClient.apiKeyIndexLoading
                                    text: AuthClient.apiKeyIndexLoading ? qsTr("Loading keys…") : qsTr("Store key")

                                    contentItem: Text {
                                        text: storeKeyButton.text
                                        color: storeKeyButton.enabled ? "white" : Theme.mutedText
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    background: Rectangle {
                                        radius: 14
                                        color: !storeKeyButton.enabled ? Theme.elevatedColor : storeKeyButton.down ? Theme.accentPressed : Theme.accentColor
                                        opacity: storeKeyButton.enabled ? 1 : 0.75
                                    }

                                    onClicked: {
                                        if (AuthClient.storeNextApiKey("", userInputField.text)) {
                                            userInputField.clear()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    id: tradeColumn
                    visible: AuthClient.sessionActive
                    anchors.fill: parent
                    anchors.margins: window.compactLayout ? 18 : 24
                    spacing: 18

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Label {
                                text: ApiWorker.tradeResultsVisible ? qsTr("Transaction results") : qsTr("Trade workspace")
                                color: Theme.primaryText
                                font.pixelSize: window.compactLayout ? 22 : 26
                                font.weight: Font.DemiBold
                            }

                            Label {
                                text: ApiWorker.tradeResultsVisible
                                      ? qsTr("Review each account result, then tap Done to return to the trade workspace.")
                                      : qsTr("Orders run across all eligible accounts loaded for %1.").arg(window.displayKey(AuthClient.selectedApiKeyLabel))
                                color: Theme.mutedText
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                font.pixelSize: 14
                            }
                        }

                        SwitchKeyButton {
                            visible: !ApiWorker.tradeResultsVisible
                            Layout.preferredWidth: window.compactLayout ? 148 : 190
                            Layout.minimumHeight: 46
                        }
                    }

                    StockTrade {

                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(260, Math.min(380, window.height * 0.44))
                        visible: !ApiWorker.tradeResultsVisible

                        onPerformBuy: {
                            if (isDebugMode) {
                                ApiWorker.executePreflight(inputFieldText, "BUY")
                            } else {
                                ApiWorker.executeTrade(inputFieldText, "BUY")
                            }
                        }

                        onPerformSell: {
                            if (isDebugMode) {
                                ApiWorker.executePreflight(inputFieldText, "SELL")
                            } else {
                                ApiWorker.executeTrade(inputFieldText, "SELL")
                            }
                        }
                    }
                }
            }

            Button {
                id: autoModeButton
                Layout.fillWidth: true
                Layout.minimumHeight: 56
                visible: !window.autoMode && !AuthClient.sessionActive && !ApiWorker.tradeResultsVisible
                text: qsTr("Auto Mode")
                contentItem: Text {
                    text: autoModeButton.text
                    color: "white"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 16
                    color: autoModeButton.down ? Theme.accentPressed : Theme.accentColor
                }
                onClicked: {
                    AuthClient.clearUserSession()
                    AuthClient.secretKey = ""
                    ApiWorker.clearSubAccountsList()

                    window.autoMode = true
                    page.contentY = 0
                }
            }

            AutoTradeScreen {
                id: autoScreen
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                visible: window.autoMode
                onLeaveMode: {
                    window.autoMode = false
                    page.contentY = 0
                }
            }
        }
    }
}
