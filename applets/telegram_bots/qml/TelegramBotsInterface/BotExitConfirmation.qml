import QtQuick
import QtQuick.Controls.Basic
import TelegramBotsInterface

Dialog {
    id: dialog
    objectName: "stopBotsExitDialog"
    required property QtObject controller
    property bool waitingForStop: false
    readonly property bool stopComplete: controller !== null && !controller.hasRunningBots && !controller.busy
    function finishIfStopped() {
        if (waitingForStop && stopComplete) {
            waitingForStop = false
            confirmed()
        }
    }
    function beginStop() {
        if (waitingForStop || controller === null || controller.busy) return
        waitingForStop = true
        controller.stopAll()
        Qt.callLater(finishIfStopped)
    }
    onStopCompleteChanged: if (waitingForStop) Qt.callLater(finishIfStopped)
    signal confirmed()
    modal: true
    closePolicy: waitingForStop ? Popup.NoAutoClose : Popup.CloseOnEscape
    title: qsTr("Stop bots before leaving")
    palette.window: Theme.cardColor
    palette.windowText: Theme.primaryText
    palette.button: Theme.elevatedColor
    palette.buttonText: Theme.primaryText
    palette.shadow: "#000000"
    width: Math.min(440, Math.max(0, parent ? parent.width - 36 : 440))
    x: parent ? Math.max(0, (parent.width - width) / 2) : 0
    y: parent ? Math.max(0, (parent.height - height) / 2) : 0
    background: Rectangle {
        color: Theme.cardColor
        border.color: Theme.borderColor
        radius: 18
    }
    contentItem: Label {
        objectName: "stopBotsExitMessage"
        text: qsTr('In order to leave all bots must be stopped. Press "Confirm" to continue')
        textFormat: Text.PlainText
        color: Theme.primaryText
        font.pixelSize: 16
        wrapMode: Text.WordWrap
    }
    footer: DialogButtonBox {
        Button {
            objectName: "cancelStopBotsExit"
            text: qsTr("Cancel")
            enabled: !dialog.waitingForStop
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        BotActionButton {
            objectName: "confirmStopBotsExit"
            text: qsTr("Confirm")
            actionColor: Theme.stopAction
            enabled: dialog.controller !== null && !dialog.controller.busy && !dialog.waitingForStop
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: dialog.beginStop()
        }
    }

}
