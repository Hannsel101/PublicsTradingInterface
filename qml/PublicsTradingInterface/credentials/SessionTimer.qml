import QtQuick
import PublicsTradingInterface
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root

    implicitWidth: 640
    implicitHeight: compactLayout ? 104 : 78

    readonly property bool compactLayout: width < 560
    readonly property int initialSeconds: 300
    property int secondsRemaining: 0
    readonly property bool sessionActive: AuthClient.sessionActive


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
                    color: Theme.successColor
                }

                Label {
                    text: qsTr("Session active")
                    color: Theme.successColor
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Public.com access tokens expire when this timer reaches zero.")
                color: Theme.mutedText
                wrapMode: Text.WordWrap
                font.pixelSize: 13
            }
        }

        Label {
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            text: root.formatTime(root.secondsRemaining)
            color: root.secondsRemaining <= 60 && countdownTimer.running ? Theme.dangerColor : Theme.primaryText
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
        AuthClient.clearUserSession()

    }
}
