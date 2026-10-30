import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Item {
    id: root
    objectName: "telegramBotConsole"
    property QtObject controller: typeof TelegramBots === "undefined" ? null : TelegramBots
    property bool editing: false
    property var editRecord: null
    readonly property bool canRequestLeave: controller !== null && !controller.busy
    property string dialogMode: ""
    property string dialogBotId: ""
    property string inspectorBotId: ""
    property var inspectorQueue: []
    function showNextInspectorConflict() {
        if (inspectorLoader.active || !root.controller) return
        while (root.inspectorQueue.length > 0) {
            const id = root.inspectorQueue[0]
            root.inspectorQueue = root.inspectorQueue.slice(1)
            const record = root.controller.bots.find(row => row.id === id)
            if (!record || !record.starting || record.stopping) continue
            root.inspectorBotId = id
            inspectorLoader.active = true
            return
        }
    }
    function openSecurity(mode, id) {
        securityLoader.active = false
        root.dialogMode = mode
        root.dialogBotId = id
        securityLoader.active = true
    }
    function editBot(id) {
        const record = root.controller.bots.find(row => row.id === id)
        if (!record) return
        root.controller.clearError()
        root.editRecord = record
        root.editing = true
    }
    signal leaveConfirmed()
    function requestLeave() {
        if (!root.canRequestLeave) return
        if (root.controller.hasRunningBots) exitLoader.active = true
        else root.leaveConfirmed()
    }
    readonly property bool compactLayout: width < 720
    readonly property real pageMargin: Math.max(18, Math.min(width * 0.06, 56))

    Rectangle {
        anchors.fill: parent
        clip: true
        color: Theme.backgroundColor
        Rectangle {
            width: Math.min(parent.width * 0.72, 720)
            height: width
            radius: width / 2
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: -height * 0.45
            color: Theme.glowColor
            opacity: 0.35
        }
    }

    Flickable {
        id: page
        visible: !root.editing
        objectName: "telegramPageScroller"
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + root.pageMargin * 2
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: contentColumn
            x: root.pageMargin
            y: root.pageMargin
            width: page.width - root.pageMargin * 2
            spacing: root.compactLayout ? 18 : 24

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    objectName: "telegramBrand"
                    text: qsTr("Telegram Bots")
                    color: Theme.accentColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    font.letterSpacing: 1.2
                }
                Label {
                    objectName: "telegramConsoleTitle"
                    text: qsTr("Telegram Bot Console")
                    color: Theme.primaryText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.pixelSize: root.compactLayout ? 30 : 42
                    font.weight: Font.DemiBold
                }
            }

            BotManagementPanel {
                objectName: "telegramManagePanel"
                controller: root.controller
                Layout.fillWidth: true
                compactLayout: root.compactLayout
                onEditRequested: record => root.editBot(record.id)
                onDeleteRequested: id => root.openSecurity("delete", id)
            }
            BotActionButton {
                objectName: "addNewBot"
                Layout.fillWidth: true
                text: qsTr("+ Add New Bot")
                actionColor: Theme.addAction
                enabled: root.controller !== null && !root.controller.busy
                onClicked: {
                    root.controller.clearError()
                    root.editRecord = null
                    root.editing = true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                BotActionButton {
                    objectName: "startBot"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    text: qsTr("Start Bot")
                    actionColor: Theme.startAction
                    enabled: root.controller !== null && root.controller.canStart
                    onClicked: root.controller.startBot()
                }
                BotActionButton {
                    objectName: "stopBot"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    text: qsTr("Stop Bot")
                    actionColor: Theme.stopAction
                    enabled: root.controller !== null && root.controller.canStop
                    onClicked: root.controller.stopBot()
                }
            }
            Label {
                objectName: "telegramRuntimeError"
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.controller !== null ? root.controller.error : ""
                textFormat: Text.PlainText
                color: Theme.stoppedAccent
                font.pixelSize: 16
                wrapMode: Text.WordWrap
            }
            Label {
                objectName: "telegramVaultNotice"
                Layout.fillWidth: true
                text: qsTr("This password vault is separate from Robinhood. Older bots from a shared credential vault are not imported or erased; recreate them here using their BotFather credentials.")
                textFormat: Text.PlainText
                color: Theme.secondaryText
                wrapMode: Text.WordWrap
            }
        }
    }
    Loader {
        anchors.fill: parent
        active: root.editing
        visible: active
        sourceComponent: Component {
            BotEditor {
                controller: root.controller
                pageMargin: root.pageMargin
                editRecord: root.editRecord
                onCancelled: { root.editing = false; root.editRecord = null }
            }
        }
    }
    Connections {
        target: root.controller
        function onBotSaved() { root.editing = false; root.editRecord = null }
        function onMissingChatIds(id) { Qt.callLater(() => root.openSecurity("missing", id)) }
        function onPasswordRequested(id) { root.openSecurity("password", id) }
        function onPasswordSetupRequired(id) { root.editBot(id) }
        function onInspectorPortConflict(id) {
            if ((inspectorLoader.active && root.inspectorBotId === id) || root.inspectorQueue.indexOf(id) >= 0) return
            root.inspectorQueue = root.inspectorQueue.concat([id])
            root.showNextInspectorConflict()
        }
    }
    Loader {
        id: inspectorLoader
        active: false
        sourceComponent: Component {
            BotInspectorConflictDialog {
                parent: root
                controller: root.controller
                botId: root.inspectorBotId
                Component.onCompleted: open()
                onClosed: {
                    inspectorLoader.active = false
                    root.inspectorBotId = ""
                    Qt.callLater(root.showNextInspectorConflict)
                }
            }
        }
    }
    Loader {
        id: securityLoader
        active: false
        sourceComponent: Component {
            BotSecurityDialog {
                parent: root
                controller: root.controller
                botId: root.dialogBotId
                mode: root.dialogMode
                Component.onCompleted: open()
                onClosed: securityLoader.active = false
                onEditSettings: id => root.editBot(id)
            }
        }
    }
    Loader {
        id: exitLoader
        active: false
        sourceComponent: Component {
            BotExitConfirmation {
                parent: root
                controller: root.controller
                Component.onCompleted: open()
                onClosed: exitLoader.active = false
                onConfirmed: {
                    exitLoader.active = false
                    root.leaveConfirmed()
                }
            }
        }
    }
}
