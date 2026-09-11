import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window
import "qml/DisplayAccountInformation"
import "qml/PerformTrades"

ApplicationWindow {
    id: window

    width: 1000
    height: 700
    minimumWidth: 360
    minimumHeight: 620
    visible: true
    title: qsTr("Publics Brokerage API Multi-Account Trader")
    color: window.backgroundColor

    readonly property bool tokenActive: StockSearchController.tokenActive
    readonly property bool compactLayout: width < 720
    readonly property bool keySelected: AuthClient.selectedApiKeyLabel !== ""
    readonly property bool canStartSession: window.keySelected && AuthClient.apiKeyReady && !AuthClient.apiKeyLoading && !AuthClient.sessionActive
    readonly property real pageMargin: Math.max(18, Math.min(width * 0.06, 56))
    readonly property real cardRadius: 24

    readonly property color backgroundColor: "#08090a"
    readonly property color panelColor: "#0f1011"
    readonly property color surfaceColor: "#17181c"
    readonly property color elevatedColor: "#202126"
    readonly property color primaryText: "#f7f8f8"
    readonly property color secondaryText: "#d0d6e0"
    readonly property color mutedText: "#8a8f98"
    readonly property color accentColor: "#7170ff"
    readonly property color accentPressed: "#5e6ad2"
    readonly property color successColor: "#10b981"
    readonly property color dangerColor: "#ff5c7a"
    readonly property color borderColor: "#2f3138"

    Rectangle {
        anchors.fill: parent
        color: window.backgroundColor

        Rectangle {
            width: Math.min(parent.width * 0.72, 720)
            height: width
            radius: width / 2
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: -height * 0.45
            color: "#25215f"
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
                        color: window.accentColor
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        font.letterSpacing: 1.2
                    }

                    Label {
                        text: qsTr("Multi-Account Brokerage Console")
                        color: window.primaryText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.pixelSize: window.compactLayout ? 30 : 42
                        font.weight: Font.DemiBold
                    }

                    Label {
                        visible: !ApiWorker.tradeResultsVisible
                        text: qsTr("Store Public.com API keys once, choose a saved key, then start a short-lived session that loads every eligible brokerage account for that key")
                        color: window.mutedText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        font.pixelSize: window.compactLayout ? 14 : 16
                        lineHeight: 1.25
                    }
                }
            }

            TradeResultScreen {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(500, page.height - contentColumn.y - 140)
                visible: ApiWorker.tradeResultsVisible
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: sessionTimer.implicitHeight + 32
                radius: window.cardRadius
                color: window.panelColor
                border.color: window.borderColor
                border.width: 1
                visible: AuthClient.sessionActive && !ApiWorker.tradeResultsVisible
                Layout.minimumHeight: visible ? implicitHeight : 0

                ValidityTimer {
                    id: sessionTimer
                    anchors.fill: parent
                    anchors.margins: 16
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.minimumHeight: visible ? (window.tokenActive ? tradeColumn.implicitHeight + 44 : keyColumn.implicitHeight + 44) : 0
                radius: window.cardRadius
                color: window.surfaceColor
                border.color: window.borderColor
                border.width: 1
                visible: !ApiWorker.tradeResultsVisible

                ColumnLayout {
                    id: keyColumn
                    visible: !window.tokenActive
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
                                color: window.primaryText
                                font.pixelSize: window.compactLayout ? 22 : 26
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            Label {
                                text: qsTr("Only PublicsApiKey labels are shown. API secrets stay in the OS-backed keychain and are never displayed after saving.")
                                color: window.mutedText
                                font.pixelSize: 14
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    ApiKeyList {
                        id: apiKeys
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(220, Math.min(340, window.height * 0.30))
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: sessionControlLayout.implicitHeight + 28
                        radius: 18
                        color: window.elevatedColor
                        border.color: window.keySelected ? window.accentColor : window.borderColor
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
                                        ? qsTr("Loading %1 from the system keychain…").arg(AuthClient.selectedApiKeyLabel)
                                        : AuthClient.apiKeyReady
                                          ? qsTr("%1 is ready").arg(AuthClient.selectedApiKeyLabel)
                                          : qsTr("%1 is selected but not ready").arg(AuthClient.selectedApiKeyLabel)
                                color: AuthClient.apiKeyReady ? window.successColor : window.secondaryText
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: AuthClient.apiKeyError !== ""
                                text: AuthClient.apiKeyError
                                color: window.dangerColor
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
                                    color: startSessionButton.enabled ? "white" : window.mutedText
                                    font.pixelSize: 15
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }

                                background: Rectangle {
                                    radius: 14
                                    color: !startSessionButton.enabled ? "#202126" : startSessionButton.down ? window.accentPressed : window.accentColor
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
                        color: window.elevatedColor
                        border.color: window.borderColor
                        border.width: 1

                        ColumnLayout {
                            id: storeKeyLayout
                            anchors.fill: parent
                            anchors.margins: 14
                            spacing: 12

                            Label {
                                text: qsTr("Add a new key")
                                color: window.secondaryText
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
                                    color: window.primaryText
                                    placeholderTextColor: window.mutedText
                                    leftPadding: 16
                                    rightPadding: 16

                                    background: Rectangle {
                                        radius: 14
                                        color: "#101115"
                                        border.color: userInputField.activeFocus ? window.accentColor : window.borderColor
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
                                        color: storeKeyButton.enabled ? "white" : window.mutedText
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    background: Rectangle {
                                        radius: 14
                                        color: !storeKeyButton.enabled ? "#202126" : storeKeyButton.down ? window.accentPressed : window.accentColor
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
                    visible: window.tokenActive
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
                                color: window.primaryText
                                font.pixelSize: window.compactLayout ? 22 : 26
                                font.weight: Font.DemiBold
                            }

                            Label {
                                text: ApiWorker.tradeResultsVisible
                                      ? qsTr("Review each account result, then tap Done to return to the trade workspace.")
                                      : qsTr("Orders run across all eligible accounts loaded for %1.").arg(AuthClient.selectedApiKeyLabel)
                                color: window.mutedText
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                                font.pixelSize: 14
                            }
                        }

                        ChooseAccountButton {
                            visible: !ApiWorker.tradeResultsVisible
                            Layout.preferredWidth: window.compactLayout ? 148 : 190
                            Layout.minimumHeight: 46
                        }
                    }

                    StockTrade {
                        id: stockTradeInterface
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
        }
    }
}
