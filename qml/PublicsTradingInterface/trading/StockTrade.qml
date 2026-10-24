import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 640
    implicitHeight: Math.max(320, tradeLayout.implicitHeight + 2 * (root.compactLayout ? 14 : 18))

    property alias inputFieldText: inputField.text
    signal performBuy()
    signal performSell()

    readonly property bool compactLayout: width < 520

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: Theme.surfaceColor
        border.color: Theme.borderColor
        border.width: 1

        ColumnLayout {
            id: tradeLayout
            anchors.fill: parent
            anchors.margins: compactLayout ? 14 : 18
            spacing: compactLayout ? 14 : 18

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: outputLayout.implicitHeight + 24
                radius: 16
                color: Theme.elevatedColor
                border.color: Theme.borderColor
                border.width: 1

                ColumnLayout {
                    id: outputLayout
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    Label {
                        text: qsTr("Execution mode")
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Buy trades one share of each ticker per account. Sell All uses each account's complete held balance for each ticker. Live orders run only in release builds.")
                        color: Theme.secondaryText
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
                    text: qsTr("Ticker symbols")
                    color: Theme.secondaryText
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                TextField {
                    id: inputField
                    objectName: "tickerInput"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    placeholderText: qsTr("Single: AAPL  or  Multi: AAPL, TSLA, GM")
                    selectByMouse: true
                    font.pixelSize: 18
                    font.capitalization: Font.AllUppercase
                    color: Theme.primaryText
                    placeholderTextColor: Theme.mutedText
                    leftPadding: 16
                    rightPadding: 16

                    background: Rectangle {
                        radius: 15
                        color: Theme.backgroundColor
                        border.color: inputField.activeFocus ? Theme.accentColor : Theme.borderColor
                        border.width: inputField.activeFocus ? 2 : 1
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Button {
                    id: buyButton
                    objectName: "buyButton"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    enabled: /[a-zA-Z]/.test(inputField.text)
                    text: qsTr("Buy")

                    contentItem: Text {
                        text: buyButton.text
                        color: buyButton.enabled ? "white" : Theme.mutedText
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 15
                        color: !buyButton.enabled ? Theme.elevatedColor : buyButton.down ? Theme.buyPressed : Theme.successColor
                        opacity: buyButton.enabled ? 1 : 0.65
                    }

                    onClicked: performBuy()
                }

                Button {
                    id: sellButton
                    objectName: "sellButton"
                    Layout.fillWidth: true
                    Layout.minimumHeight: 52
                    enabled: /[a-zA-Z]/.test(inputField.text)
                    text: qsTr("Sell All")

                    contentItem: Text {
                        text: sellButton.text
                        color: sellButton.enabled ? "white" : Theme.mutedText
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: 15
                        color: !sellButton.enabled ? Theme.elevatedColor : sellButton.down ? Theme.sellPressed : Theme.dangerColor
                        opacity: sellButton.enabled ? 1 : 0.65
                    }

                    onClicked: performSell()
                }
            }
        }
    }
}
