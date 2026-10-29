pragma Singleton
import QtQuick

QtObject {
    readonly property color backgroundColor: "#08090a"
    readonly property color cardColor: "#17181c"
    readonly property color elevatedColor: "#101b24"
    readonly property color primaryText: "#f7f8f8"
    readonly property color secondaryText: "#d0d6e0"
    readonly property color mutedText: "#8a8f98"
    readonly property color accentColor: "#00D2FF"
    readonly property color borderColor: "#29343e"
    readonly property color glowColor: "#003a4b"
    readonly property color addAction: "#7750c2"
    readonly property color startAction: "#21694f"
    readonly property color stopAction: "#a7445c"
    readonly property color runningSurface: "#12372d"
    readonly property color stoppedSurface: "#361c28"
    readonly property color runningAccent: "#54d49c"
    readonly property color stoppedAccent: "#e18b9e"
    readonly property url robotHead: Qt.resolvedUrl("assets/robot-head.svg")
}
