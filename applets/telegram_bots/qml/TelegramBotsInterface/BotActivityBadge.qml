import QtQuick
import QtQuick.Controls
import TelegramBotsInterface

Rectangle {
    id: badge
    objectName: "botActivityBadge"
    property bool active: false
    property bool starting: false
    property int phase: 0
    readonly property bool motionRunning: ticker.running
    implicitWidth: 104
    implicitHeight: 40
    radius: 12
    color: active ? Theme.runningSurface : Theme.stoppedSurface
    border.color: active ? Theme.runningAccent : Theme.stoppedAccent
    onActiveChanged: if (!active) phase = 0
    Timer {
        id: ticker
        interval: 500
        repeat: true
        running: badge.active && badge.visible
        onTriggered: badge.phase = (badge.phase + 1) % 4
    }
    Repeater {
        model: 4
        Rectangle {
            required property int index
            width: 4
            height: 4
            radius: 2
            x: index === 0 || index === 3 ? 5 : badge.width - 9
            y: index < 2 ? 4 : badge.height - 8
            color: badge.active ? (badge.phase === index ? Theme.runningAccent : "#275844")
                                : Theme.stoppedAccent
            Accessible.ignored: true
        }
    }
    Label {
        objectName: "botActivityText"
        anchors.centerIn: parent
        text: badge.active ? qsTr("Active") : badge.starting ? qsTr("Connecting…") : qsTr("Not Active")
        color: badge.active ? Theme.runningAccent : Theme.stoppedAccent
        font.pixelSize: 12
        font.weight: Font.DemiBold
    }
}
