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
                    z: 1

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
                            text: AuthClient.apiKeyNicknames[delegateRoot.apiKeyLabel] || delegateRoot.apiKeyLabel
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

                    Button {
                        id: editNicknameButton
                        Layout.preferredWidth: 42
                        Layout.preferredHeight: 42
                        enabled: !AuthClient.apiKeyIndexLoading
                        text: "✎"
                        Accessible.name: qsTr("Edit nickname for %1").arg(AuthClient.apiKeyNicknames[delegateRoot.apiKeyLabel] || delegateRoot.apiKeyLabel)
                        contentItem: Text {
                            text: editNicknameButton.text
                            color: root.primaryText
                            font.pixelSize: 22
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 12
                            color: editNicknameButton.down ? root.accentColor : root.surfaceColor
                            border.color: root.borderColor
                        }
                        onClicked: {
                            nicknamePopup.editingLabel = delegateRoot.apiKeyLabel
                            nicknameField.text = AuthClient.apiKeyNicknames[delegateRoot.apiKeyLabel] || ""
                            nicknameError.text = ""
                            nicknamePopup.open()
                            nicknameField.forceActiveFocus()
                        }
                    }
                    Button {
                        id: deleteKeyButton
                        Layout.preferredWidth: 42
                        Layout.preferredHeight: 42
                        enabled: !AuthClient.apiKeyIndexLoading
                        text: "🗑"
                        Accessible.name: qsTr("Delete %1").arg(AuthClient.apiKeyNicknames[delegateRoot.apiKeyLabel] || delegateRoot.apiKeyLabel)
                        contentItem: Text {
                            text: deleteKeyButton.text
                            color: "#ff5c7a"
                            font.pixelSize: 20
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 12
                            color: deleteKeyButton.down ? "#3a1720" : root.surfaceColor
                            border.color: root.borderColor
                        }
                        onClicked: {
                            deletePopup.deletingLabel = delegateRoot.apiKeyLabel
                            deleteError.text = ""
                            deletePopup.open()
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    enabled: !AuthClient.apiKeyIndexLoading
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

    Popup {
        id: nicknamePopup
        property string editingLabel: ""
        parent: Overlay.overlay
        width: Math.min(parent.width - 32, 380)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        modal: true
        focus: true
        padding: 18
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            radius: 18
            color: root.elevatedColor
            border.color: root.accentColor
        }
        contentItem: ColumnLayout {
            spacing: 14
            Label {
                Layout.fillWidth: true
                text: qsTr("Edit API key nickname")
                color: root.primaryText
                font.pixelSize: 19
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Leave blank to show the default key name.")
                color: root.secondaryText
                wrapMode: Text.WordWrap
            }
            TextField {
                id: nicknameField
                Layout.fillWidth: true
                placeholderText: qsTr("Nickname")
                color: root.primaryText
                selectByMouse: true
                background: Rectangle {
                    radius: 10
                    color: root.surfaceColor
                    border.color: nicknameField.activeFocus ? root.accentColor : root.borderColor
                }
                onAccepted: saveNicknameButton.clicked()
            }
            Label {
                id: nicknameError
                Layout.fillWidth: true
                visible: text !== ""
                color: "#ff5c7a"
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    Layout.fillWidth: true
                    text: qsTr("Cancel")
                    onClicked: nicknamePopup.close()
                }
                Button {
                    id: saveNicknameButton
                    Layout.fillWidth: true
                    text: qsTr("Save")
                    onClicked: {
                        if (AuthClient.setApiKeyNickname(nicknamePopup.editingLabel, nicknameField.text))
                            nicknamePopup.close()
                        else
                            nicknameError.text = qsTr("Could not save the nickname. Please try again.")
                    }
                }
            }
        }
    }

    Popup {
        id: deletePopup
        property string deletingLabel: ""
        parent: Overlay.overlay
        width: Math.min(parent.width - 32, 380)
        x: (parent.width - width) / 2
        y: (parent.height - height) / 2
        modal: true
        focus: true
        padding: 18
        closePolicy: AuthClient.apiKeyIndexLoading ? Popup.NoAutoClose : Popup.CloseOnEscape
        background: Rectangle {
            radius: 18
            color: root.elevatedColor
            border.color: "#ff5c7a"
        }
        contentItem: ColumnLayout {
            spacing: 14
            Label {
                Layout.fillWidth: true
                text: qsTr("Delete saved API key?")
                color: root.primaryText
                font.pixelSize: 19
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                readonly property string nickname: AuthClient.apiKeyNicknames[deletePopup.deletingLabel] || ""
                text: nickname !== ""
                      ? qsTr("Remove %1 (%2) from the system keychain? Its nickname will also be removed from the app settings. This cannot be undone.").arg(nickname, deletePopup.deletingLabel)
                      : qsTr("Remove %1 from the system keychain? Any saved nickname will also be removed from the app settings. This cannot be undone.").arg(deletePopup.deletingLabel)
                color: root.secondaryText
                wrapMode: Text.WordWrap
            }
            Label {
                id: deleteError
                Layout.fillWidth: true
                visible: text !== ""
                color: "#ff5c7a"
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Button {
                    Layout.fillWidth: true
                    enabled: !AuthClient.apiKeyIndexLoading
                    text: qsTr("Cancel")
                    onClicked: deletePopup.close()
                }
                Button {
                    Layout.fillWidth: true
                    enabled: !AuthClient.apiKeyIndexLoading
                    text: AuthClient.apiKeyIndexLoading ? qsTr("Deleting…") : qsTr("Confirm deletion")
                    onClicked: {
                        deleteError.text = ""
                        if (!AuthClient.removeStoredApiKey(deletePopup.deletingLabel))
                            deleteError.text = AuthClient.apiKeyError || qsTr("Could not start deletion. Please try again.")
                    }
                }
            }
        }
    }
    Connections {
        target: AuthClient
        function onApiKeyRemovalFinished(label, succeeded, message) {
            if (label !== deletePopup.deletingLabel) return
            if (succeeded)
                deletePopup.close()
            else
                deleteError.text = message
        }
    }
}
