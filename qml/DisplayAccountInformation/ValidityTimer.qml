import QtQuick 2.15
import QtQuick.Controls

Item
{
    id: root

    // Custom properties to manage the time state
    property int secondsRemaining: 0
    property int initialSeconds: 300 // session stays active for 5min
    property bool validSecretKey: false
    property bool sessionActive: AuthClient.sessionActive



    /**
      * When a valid secret key is introduced the timer will be displayed
      */
    onValidSecretKeyChanged:
    {
        root.visible = validSecretKey
    }

    Column
    {
        anchors.top: parent.top
        anchors.left: parent.left
        spacing: 0

        // 1. Session Clock Label
        Text
        {
            id: sessionLabel
            text: "Note: When this timer goes to 0 the Publics API session tokens have expired"
            font.pointSize: 10
            verticalAlignment: Text.AlignBottom
            color: "black"
        }

        // 2. The Visual Clock Display
        Text
        {
            id: timerDisplay
            text: formatTime(secondsRemaining)
            font.pointSize: 48
            font.bold: true
            color: secondsRemaining <= 60 && countdownTimer.running ? "red" : "black" // Turns red for warning
            anchors.left: parent.left
            verticalAlignment: Text.AlignTop
        }

        // 1. Control Button
        Button
        {
            id: refreshSessionButton
            text: "Reset Session"
            anchors.left: parent.left
            enabled: !sessionActive
            onClicked:
            {
                    // Set the starting point and begin
                    AuthClient.requestToken()
                    secondsRemaining = initialSeconds
                    countdownTimer.start()
                    sessionActive = true
            }
        }
    }

    // 3. The Core Timer Engine
    Timer {
        id: countdownTimer
        interval: 1000  // Fires every 1 second (1000 milliseconds)
        repeat: true    // Must repeat to update the clock display every second

        onTriggered:
        {
            secondsRemaining -= 1

            // Check if time has completely run out
            if (secondsRemaining <= 0) {
                countdownTimer.stop() // Halt the repeats
                performTimeoutAction() // Execute your final action
            }
        }
    }

    // JavaScript Helper to format raw seconds into a clean "00:00" string
    function formatTime(totalSeconds)
    {
        let minutes = Math.floor(totalSeconds / 60)
        let seconds = totalSeconds % 60

        // Adds a leading zero if the number is a single digit (e.g., "5" becomes "05")
        let paddedMinutes = minutes.toString().padStart(2, '0')
        let paddedSeconds = seconds.toString().padStart(2, '0')

        return paddedMinutes + ":" + paddedSeconds
    }

    // runs when the timer expires
    function performTimeoutAction()
    {
        sessionActive = false
    }
}
