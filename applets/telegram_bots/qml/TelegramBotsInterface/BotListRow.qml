import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import TelegramBotsInterface

ItemDelegate {
    id: row
    required property var record
    implicitHeight: 72
    leftPadding: 12
    rightPadding: 12
    Accessible.name: qsTr("%1, %2").arg(record.nickname).arg(record.active ? qsTr("Active") : qsTr("Not Active"))
    background: Rectangle {
        radius: 12
        color: row.record.active ? Theme.runningSurface : Theme.stoppedSurface
        border.width: row.highlighted || row.visualFocus ? 2 : 1
        border.color: row.highlighted || row.visualFocus ? Theme.accentColor
                      : row.record.active ? Theme.runningAccent : Theme.stoppedAccent
    }
    contentItem: RowLayout {
        spacing: 12
        Label {
            objectName: "botNickname"
            Layout.fillWidth: true
            text: row.record.nickname
            textFormat: Text.PlainText
            color: Theme.primaryText
            font.pixelSize: 16
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
        BotActivityBadge {
            active: row.record.active
            starting: row.record.starting
        }
    }
}
