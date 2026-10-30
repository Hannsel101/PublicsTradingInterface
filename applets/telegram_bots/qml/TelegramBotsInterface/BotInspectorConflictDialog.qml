import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Dialog {
    id: dialog
    required property QtObject controller
    required property string botId
    property bool submitted: false
    objectName: "inspectorConflictDialog"
    modal: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: Math.min(460, parent.width - 32)
    palette.window: Theme.cardColor
    palette.windowText: Theme.primaryText
    function choose(action) {
        if (submitted || (action === "restart" && controller.busy)) return
        submitted = true
        if (!controller.resolveInspectorConflict(botId, action)) {
            submitted = false
            if (action === "cancel") close()
            return
        }
        close()
    }
    onRejected: choose("cancel")
    contentItem: ColumnLayout {
        spacing: 20
        Label {
            objectName: "inspectorConflictMessage"
            Layout.fillWidth: true
            text: qsTr("Ngrok inspector port is already in use! Close the inspector port and try again?")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.primaryText
        }
        Label {
            objectName: "inspectorOwnershipNotice"
            Layout.fillWidth: true
            text: qsTr("Only an app-recorded orphan will be closed. Untracked or supervised ngrok instances must be closed manually.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.secondaryText
        }
        Label {
            objectName: "inspectorMacSignalNotice"
            visible: Qt.platform.os === "osx"
            Layout.fillWidth: true
            text: qsTr("This app verifies process identity before sending macOS shutdown signals. The process may change between those operations; Confirm accepts that risk.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.secondaryText
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "confirmInspectorConflict"
                text: qsTr("Confirm")
                enabled: !dialog.submitted && !dialog.controller.busy
                onClicked: dialog.choose("restart")
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "cancelInspectorConflict"
                text: qsTr("Cancel")
                enabled: !dialog.submitted
                onClicked: dialog.choose("cancel")
            }
        }
    }
    Connections {
        target: dialog.controller
        function onBotsChanged() {
            if (dialog.submitted) return
            const record = dialog.controller.bots.find(row => row.id === dialog.botId)
            if (!record || !record.starting || record.stopping) dialog.close()
        }
    }
}
