import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

Pane {
    id: panel
    property bool compactLayout: false
    required property QtObject controller
    readonly property int botCount: controller !== null ? controller.bots.length : 0
    padding: compactLayout ? 18 : 24
    background: Rectangle {
        radius: 24
        color: Theme.cardColor
        border.color: Theme.borderColor
    }
    contentItem: ColumnLayout {
        spacing: 18
        Label {
            objectName: "telegramManageTitle"
            text: qsTr("Manage Telegram Bots")
            color: Theme.primaryText
            font.pixelSize: panel.compactLayout ? 22 : 26
            font.weight: Font.DemiBold
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: panel.compactLayout ? 160 : 180
            radius: 18
            color: Theme.elevatedColor
            border.color: Theme.borderColor
            ListView {
                id: botList
                objectName: "telegramBotList"
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                reuseItems: true
                spacing: 8
                visible: panel.botCount > 0
                model: panel.controller !== null ? panel.controller.bots : []
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                delegate: BotListRow {
                    required property var modelData
                    width: botList.width
                    record: modelData
                    highlighted: panel.controller !== null && panel.controller.selectedId === record.id
                    onClicked: panel.controller.selectBot(record.id)
                }
            }
            Column {
                visible: panel.botCount === 0
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 32)
                spacing: 16
                Image {
                    objectName: "telegramEmptyRobot"
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 64
                    height: 64
                    source: Theme.robotHead
                    sourceSize.width: 128
                    sourceSize.height: 128
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Label {
                    objectName: "telegramEmptyState"
                    width: parent.width
                    text: qsTr("No bots configured")
                    color: Theme.secondaryText
                    font.pixelSize: 16
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
