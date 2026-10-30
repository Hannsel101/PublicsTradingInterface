import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Dialog {
    id: dialog
    required property QtObject controller
    required property string botId
    required property string mode
    signal editSettings(string botId)
    objectName: mode === "delete" ? "deleteBotDialog" : mode === "missing" ? "missingChatIdDialog" : "startBotPasswordDialog"
    modal: true
    closePolicy: controller.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    anchors.centerIn: parent
    width: Math.min(440, parent.width - 32)
    title: mode === "delete" ? qsTr("Delete Bot") : mode === "missing" ? qsTr("Private Chat ID required") : qsTr("Unlock Bot to Start")
    palette.window: Theme.cardColor
    palette.windowText: Theme.primaryText
    onClosed: passwordField.clear()
    contentItem: ColumnLayout {
        spacing: 16
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: Theme.primaryText
            text: dialog.mode === "missing" ? qsTr("Edit bot settings now to include your private chat ID.")
                : dialog.mode === "delete" ? qsTr("Delete this bot and its encrypted credentials? Submitted orders and the admission ledger are not erased.")
                : qsTr("Enter this bot’s password. Forgotten passwords cannot recover the encrypted payload. Delete and recreate the bot instead.")
        }
        BotCredentialField {
            id: passwordField
            objectName: "startBotPasswordInput"
            Layout.fillWidth: true
            visible: dialog.mode === "password"
            enabled: !dialog.controller.busy
            echoMode: TextInput.Password
            inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            Accessible.name: qsTr("Bot password")
            onAccepted: confirmButton.clicked()
        }
        Label {
            objectName: "botPasswordError"
            Layout.fillWidth: true
            visible: text.length > 0
            text: dialog.controller.error
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.stoppedAccent
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "cancelBotSecurityDialog"
                text: qsTr("Cancel")
                enabled: !dialog.controller.busy
                onClicked: dialog.close()
            }
            Item { Layout.fillWidth: true }
            Button {
                id: confirmButton
                objectName: dialog.mode === "delete" ? "confirmDeleteBot" : dialog.mode === "missing" ? "editMissingChatSettings" : "confirmStartBotPassword"
                text: dialog.mode === "delete" ? qsTr("Delete") : dialog.mode === "missing" ? qsTr("Edit Settings") : qsTr("Start Bot")
                enabled: !dialog.controller.busy && (dialog.mode !== "password" || passwordField.text.length > 0)
                onClicked: {
                    if (!enabled) return
                    if (dialog.mode === "missing") { dialog.editSettings(dialog.botId); dialog.close() }
                    else if (dialog.mode === "delete") dialog.controller.deleteBot(dialog.botId)
                    else {
                        let password = passwordField.text
                        passwordField.clear()
                        dialog.controller.startBotWithPassword(dialog.botId, password)
                        password = ""
                    }
                }
            }
        }
    }
    Connections {
        target: dialog.controller
        function onActivationFinished(id, success) { if (id === dialog.botId && success) dialog.close() }
        function onBotDeleted(id) { if (id === dialog.botId) dialog.close() }
        function onMissingChatIds(id) { if (id === dialog.botId && dialog.mode === "password") dialog.close() }
    }
}
