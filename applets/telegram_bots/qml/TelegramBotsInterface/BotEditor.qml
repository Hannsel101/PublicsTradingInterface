import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Flickable {
    id: editor
    required property QtObject controller
    property real pageMargin: 24
    signal cancelled()
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    contentWidth: width
    contentHeight: Math.max(height, form.implicitHeight + pageMargin * 2)
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    function save() {
        if (!saveButton.enabled) return
        const nickname = nicknameField.text.trim()
        const token = tokenField.text
        tokenField.clear()
        editor.controller.saveBot(nickname, token)
    }

    ColumnLayout {
        id: form
        x: editor.pageMargin
        y: editor.pageMargin
        width: editor.width - editor.pageMargin * 2
        height: Math.max(implicitHeight, editor.height - editor.pageMargin * 2)
        spacing: 18
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Add New Bot")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 26
                font.weight: Font.DemiBold
                color: Theme.primaryText
            }
            Button {
                objectName: "cancelBotEditor"
                text: qsTr("Cancel")
                enabled: editor.controller !== null && !editor.controller.busy
                onClicked: editor.cancelled()
            }
        }
        Pane {
            Layout.fillWidth: true
            padding: 20
            background: Rectangle {
                color: Theme.cardColor
                radius: 20
                border.color: Theme.borderColor
            }
            contentItem: ColumnLayout {
                spacing: 12
                Label {
                    text: qsTr("Bot Nickname")
                    color: Theme.secondaryText
                    font.pixelSize: 16
                }
                BotCredentialField {
                    id: nicknameField
                    objectName: "botNicknameInput"
                    Layout.fillWidth: true
                    maximumLength: 128
                    Accessible.name: qsTr("Bot Nickname")
                    enabled: editor.controller !== null && !editor.controller.busy
                    onAccepted: tokenField.forceActiveFocus()
                }
                Label {
                    text: qsTr("Bot API Token")
                    color: Theme.secondaryText
                    font.pixelSize: 16
                }
                BotCredentialField {
                    id: tokenField
                    objectName: "botTokenInput"
                    Layout.fillWidth: true
                    maximumLength: 16 + 1 + 256
                    Accessible.name: qsTr("Bot API Token")
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    validator: RegularExpressionValidator {
                        regularExpression: /^[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}$/
                    }
                    enabled: editor.controller !== null && !editor.controller.busy
                    onAccepted: editor.save()
                }
            }
        }
        Label {
            objectName: "botSaveError"
            Layout.fillWidth: true
            visible: text.length > 0
            text: editor.controller !== null ? editor.controller.error : ""
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.stoppedAccent
            font.pixelSize: 16
        }
        Item { Layout.fillHeight: true }
        BotActionButton {
            id: saveButton
            objectName: "saveBot"
            Layout.fillWidth: true
            text: editor.controller !== null && editor.controller.busy ? qsTr("Saving…") : qsTr("Save Bot")
            actionColor: Theme.addAction
            enabled: editor.controller !== null && !editor.controller.busy
                     && nicknameField.text.trim().length > 0 && tokenField.acceptableInput
            onClicked: editor.save()
        }
    }
}
