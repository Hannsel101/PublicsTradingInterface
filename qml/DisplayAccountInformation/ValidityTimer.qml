import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 640
    implicitHeight: compactLayout ? 104 : 78

    readonly property bool compactLayout: width < 560
    readonly property int initialSeconds: 300
    property int secondsRemaining: 0
    property bool sessionActive: AuthClient.sessionActive

    readonly property color primaryText: "#f7f8f8"
    readonly property color mutedText: "#8a8f98"
    readonly property color warningColor: "#ff5c7a"
    readonly property color successColor: "#10b981"

    visible: root.sessionActive

    onSessionActiveChanged: {
        if (root.sessionActive) {
            root.secondsRemaining = root.initialSeconds
            countdownTimer.start()
        } else {
            root.secondsRemaining = 0
            countdownTimer.stop()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 9
                    Layout.preferredHeight: 9
                    radius: 5
                    color: root.successColor
                }

                Label {
                    text: qsTr("Session active")
                    color: root.successColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Public.com access tokens expire when this timer reaches zero.")
                color: root.mutedText
                wrapMode: Text.WordWrap
                font.pixelSize: 13
            }
        }

        Label {
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            text: root.formatTime(root.secondsRemaining)
            color: root.secondsRemaining <= 60 && countdownTimer.running ? root.warningColor : root.primaryText
            font.pixelSize: root.compactLayout ? 34 : 42
            font.weight: Font.DemiBold
            font.family: "Menlo"
        }
    }

    Timer {
        id: countdownTimer
        interval: 1000
        repeat: true
        onTriggered: {
            root.secondsRemaining -= 1
            if (root.secondsRemaining <= 0) {
                countdownTimer.stop()
                root.performTimeoutAction()
            }
        }
    }

    function formatTime(totalSeconds) {
        let minutes = Math.floor(totalSeconds / 60)
        let seconds = totalSeconds % 60
        return minutes.toString().padStart(2, "0") + ":" + seconds.toString().padStart(2, "0")
    }

    function performTimeoutAction() {
        AuthClient.sessionActive = false
        StockSearchController.tokenActive = false
    }
}
