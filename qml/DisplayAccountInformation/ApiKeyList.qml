pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 420
    implicitHeight: 300

    readonly property color surfaceColor: "#101115"
    readonly property color elevatedColor: "#202126"
    readonly property color primaryText: "#f7f8f8"
    readonly property color secondaryText: "#d0d6e0"
    readonly property color mutedText: "#8a8f98"
    readonly property color accentColor: "#7170ff"
    readonly property color borderColor: "#2f3138"

    property var apiKeysModel: AuthClient.secretKeys

    Rectangle {
        anchors.fill: parent
        radius: 18
        color: root.surfaceColor
        border.color: root.borderColor
        border.width: 1

        Label {
            id: emptyState
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 360)
            visible: keysListView.count === 0
            text: qsTr("No API keys stored yet. Add one below to begin.")
            color: root.mutedText
            font.pixelSize: 15
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }

        ListView {
            id: keysListView
            anchors.fill: parent
            anchors.margins: 12
            model: root.apiKeysModel
            spacing: 10
            clip: true
            currentIndex: -1

            delegate: Rectangle {
                id: delegateRoot

                required property int index
                required property string modelData

                width: keysListView.width
                height: 64
                radius: 16
                color: delegateRoot.isSelected ? "#25285f" : root.elevatedColor
                border.color: delegateRoot.isSelected ? root.accentColor : root.borderColor
                border.width: delegateRoot.isSelected ? 2 : 1

                readonly property bool isSelected: delegateRoot.modelData === AuthClient.selectedApiKeyLabel
                readonly property string apiKeyLabel: delegateRoot.modelData

                Behavior on color { ColorAnimation { duration: 140 } }
                Behavior on border.color { ColorAnimation { duration: 140 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 12

                    Rectangle {
                        Layout.preferredWidth: 12
                        Layout.preferredHeight: 12
                        radius: 6
                        color: delegateRoot.isSelected ? root.accentColor : root.mutedText
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: delegateRoot.apiKeyLabel
                            color: root.primaryText
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            text: delegateRoot.isSelected ? qsTr("Selected for session") : qsTr("Tap to select")
                            color: delegateRoot.isSelected ? root.secondaryText : root.mutedText
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        keysListView.currentIndex = delegateRoot.index
                        AuthClient.secretKey = delegateRoot.apiKeyLabel
                        ApiWorker.clearSubAccountsList()
                        AuthClient.clearUserSession()
                        StockSearchController.tokenActive = false
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }
        }
    }
}
