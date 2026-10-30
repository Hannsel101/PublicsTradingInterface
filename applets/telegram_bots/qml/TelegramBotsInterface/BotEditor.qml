import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Flickable {
    id: editor
    required property QtObject controller
    property real pageMargin: 24
    property var editRecord: null
    readonly property bool editingExisting: editRecord !== null
    readonly property bool protectedRecord: editingExisting && !!editRecord.passwordProtected
    property bool unlocked: !protectedRecord
    property string localError: ""
    function clearSensitive() {
        tokenField.clear(); passwordField.clear(); confirmationField.clear(); unlockPassword.clear(); allowedChats.clear()
        editor.controller.lockBotSettings()
    }
    Component.onDestruction: clearSensitive()
    Connections {
        target: editor.controller
        function onSettingsUnlocked(id, ids) {
            if (editor.editingExisting && id === editor.editRecord.id) {
                allowedChats.text = ids.join("\n"); editor.unlocked = true
            }
        }
    }
    readonly property bool validChatIds: {
        const text = allowedChats.text
        const maximum = editor.controller !== null ? editor.controller.maximumAllowedChatId : ""
        if (!maximum || text.length > 32768 || /(^|,)\s*(,|$)/.test(text) && text.trim().length > 0) return false
        return text.split(/[,\r\n]/).every(part => {
            const id = part.trim()
            return id.length === 0 || (/^[1-9][0-9]{0,15}$/.test(id)
                && (id.length < maximum.length || (id.length === maximum.length && id <= maximum)))
        })
    }
    signal cancelled()
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    contentWidth: width
    contentHeight: Math.max(height, form.implicitHeight + pageMargin * 2)
    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

    function save() {
        if (!saveButton.enabled) return
        if (passwordField.text !== confirmationField.text) {
            editor.localError = qsTr("Passwords do not match."); return
        }
        editor.localError = ""
        let token = tokenField.text
        let password = passwordField.text
        let confirmation = confirmationField.text
        tokenField.clear(); passwordField.clear(); confirmationField.clear()
        editor.controller.saveProtectedBot(editor.editingExisting ? editor.editRecord.id : "",
            nicknameField.text.trim(), token, allowedChats.text, password, confirmation)
        token = ""; password = ""; confirmation = ""
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
                text: editor.editingExisting ? qsTr("Edit Bot Permissions") : qsTr("Add New Bot")
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
                onClicked: { editor.clearSensitive(); editor.cancelled() }
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
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Use at least 12 characters. This password is required each time you start the bot. Forgotten passwords cannot recover the encrypted payload; delete and recreate the bot instead.")
                    color: Theme.secondaryText
                }
                BotCredentialField {
                    id: unlockPassword
                    objectName: "unlockBotSettingsPassword"
                    Layout.fillWidth: true
                    visible: editor.protectedRecord && !editor.unlocked
                    enabled: !editor.controller.busy
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Current password to unlock settings")
                    Accessible.name: qsTr("Current password to unlock settings")
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                }
                Button {
                    objectName: "unlockBotSettings"
                    text: qsTr("Unlock Settings")
                    visible: editor.protectedRecord && !editor.unlocked
                    enabled: !editor.controller.busy && unlockPassword.text.length > 0
                    onClicked: {
                        let password = unlockPassword.text
                        unlockPassword.clear()
                        editor.controller.unlockBotSettings(editor.editRecord.id, password)
                        password = ""
                    }
                }
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
                    text: editor.editingExisting ? editor.editRecord.nickname : ""
                    readOnly: editor.editingExisting
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
                    placeholderText: editor.editingExisting ? qsTr("Unchanged — leave blank") : ""
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    validator: RegularExpressionValidator {
                        regularExpression: /^[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}$/
                    }
                    enabled: editor.controller !== null && !editor.controller.busy
                    onAccepted: editor.save()
                }
                Label {
                    text: qsTr("Allowed Telegram Chat IDs")
                    color: Theme.secondaryText
                    font.pixelSize: 16
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                TextArea {
                    id: allowedChats
                    objectName: "allowedTelegramChatIdsInput"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    text: editor.editingExisting && editor.editRecord.allowedChatIds ? editor.editRecord.allowedChatIds.join("\n") : ""
                    placeholderText: qsTr("123456789, 987654321")
                    Accessible.name: qsTr("Allowed Telegram Chat IDs")
                    enabled: editor.controller !== null && !editor.controller.busy && editor.unlocked
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    color: Theme.primaryText
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                    background: Rectangle { color: Theme.elevatedColor; radius: 8; border.color: editor.validChatIds ? Theme.borderColor : Theme.stoppedAccent }
                }
                BotCredentialField {
                    id: passwordField
                    objectName: "botPasswordInput"
                    Layout.fillWidth: true
                    enabled: !editor.controller.busy && editor.unlocked
                    echoMode: TextInput.Password
                    placeholderText: editor.protectedRecord ? qsTr("Current password to save") : qsTr("Password (at least 12 characters)")
                    Accessible.name: qsTr("Bot password")
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    onTextEdited: editor.localError = ""
                }
                BotCredentialField {
                    id: confirmationField
                    objectName: "botPasswordConfirmationInput"
                    Layout.fillWidth: true
                    enabled: !editor.controller.busy && editor.unlocked
                    echoMode: TextInput.Password
                    placeholderText: qsTr("Confirm password")
                    Accessible.name: qsTr("Confirm bot password")
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    onTextEdited: editor.localError = ""
                    onAccepted: editor.save()
                }
                Label {
                    objectName: "chatPolicyValidationError"
                    visible: !editor.validChatIds
                    text: qsTr("Enter decimal private Chat IDs from 1 to %1, separated by commas or new lines.").arg(editor.controller !== null ? editor.controller.maximumAllowedChatId : "4503599627370495")
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.stoppedAccent
                }
                Label {
                    text: qsTr("Positive decimal private Chat IDs, separated by commas or new lines. Empty denies all trading. Saved encrypted in the OS keychain. Stop this bot before editing; Stop never reverses submitted orders.")
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.secondaryText
                    font.pixelSize: 13
                }
            }
        }
        Label {
            objectName: "botSaveError"
            Layout.fillWidth: true
            visible: text.length > 0
            text: editor.localError || (editor.controller !== null ? editor.controller.error : "")
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
                     && editor.unlocked && passwordField.text.length >= 12 && confirmationField.text.length >= 12
                     && nicknameField.text.trim().length > 0 && editor.validChatIds
                     && (tokenField.acceptableInput || (editor.editingExisting && tokenField.text.length === 0))
            onClicked: editor.save()
        }
    }
}
