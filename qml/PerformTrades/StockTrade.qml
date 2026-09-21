import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 640
    implicitHeight: 320

    property alias inputFieldText: inputField.text
    signal performBuy()
    signal performSell()

    readonly property bool compactLayout: width < 520
    readonly property color primaryText: "#f7f8f8"
    readonly property color secondaryText: "#d0d6e0"
    readonly property color mutedText: "#8a8f98"
    readonly property color accentColor: "#7170ff"
    readonly property color buyColor: "#10b981"
    readonly property color sellColor: "#ff5c7a"
    readonly property color surfaceColor: "#101115"
    readonly property color elevatedColor: "#202126"
    readonly property color borderColor: "#2f3138"

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: surfaceColor
        border.color: borderColor
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: compactLayout ? 14 : 18
            spacing: compactLayout ? 14 : 18

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: outputLayout.implicitHeight + 24
                radius: 16
                color: elevatedColor
                border.color: borderColor
                border.width: 1

                ColumnLayout {
                    id: outputLayout
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    Label {
                        text: qsTr("Execution mode")
                        color: mutedText
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Buy trades one share per account. Sell All uses each account's complete held balance for the entered ticker. Live orders run only in release builds.")
                        color: secondaryText
                        font.pixelSize: 14
                        wrapMode: Text.WordWrap
                        lineHeight: 1.15
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("Ticker symbol")
                    color: secondaryText
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                TextField {
                    id: inputField
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    placeholderText: qsTr("AAPL, TSLA, MSFT…")
                    selectByMouse: true
                    font.pixelSize: 18
                    font.capitalization: Font.AllUppercase
                    color: primaryText
                    placeholderTextColor: mutedText
                    leftPadding: 16
                    rightPadding: 16

                    background: Rectangle {
                        radius: 15
                        color: "#08090a"
                        border.color: inputField.activeFocus ? accentColor : borderColor
                        border.width: inputField.activeFocus ? 2 : 1
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Button {
                    id: buyButton
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    enabled: inputField.text.trim() !== ""
                    text: qsTr("Buy")

                    contentItem: Text {
                        text: buyButton.text
                        color: buyButton.enabled ? "white" : mutedText
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 15
                        color: !buyButton.enabled ? elevatedColor : buyButton.down ? "#0f8d68" : buyColor
                        opacity: buyButton.enabled ? 1 : 0.65
                    }

                    onClicked: performBuy()
                }

                Button {
                    id: sellButton
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    enabled: inputField.text.trim() !== ""
                    text: qsTr("Sell All")

                    contentItem: Text {
                        text: sellButton.text
                        color: sellButton.enabled ? "white" : mutedText
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 15
                        color: !sellButton.enabled ? elevatedColor : sellButton.down ? "#d64562" : sellColor
                        opacity: sellButton.enabled ? 1 : 0.65
                    }

                    onClicked: performSell()
                }
            }
        }
    }
}
