import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import "qml/DisplayAccountInformation"


ApplicationWindow {
    id: window
    width: 640
    height: 480
    minimumWidth: 200
    minimumHeight: 250
    visible: true
    title: qsTr("Publics Brokerage API Multi-Account Trader")
    property bool lightMode: Application.styleHints.colorScheme === Qt.Light
    property color reallyDark: "#1f1f1f"
    property color dark: "#262626"
    property color reallyLight: "#e7e7e7"
    property color light: "#e0e0e0"


    /**
      * When the
      */
    property bool tokenValid: false

    // Custom Action Function
    function printUserSecretKey(secretKey)
    {
        console.log("User submitted key:", secretKey)
        userInputField.clear()
    }

    ValidityTimer
    {
        id: testTimer
        anchors.top: parent.top
        anchors.left: parent.left
        width: 100
        height: 50
    }

    Column
    {
        id: userKeyInput
        anchors.centerIn: parent
        spacing: 15

        TextField
        {
            id: userInputField
            placeholderText: "Enter your secret key here..."
            width: 250

            // Triggered automatically when the user presses Enter/Return
            onAccepted:
            {
               printUserSecretKey(userInputField.text)
            }
        }

        Button
        {
            text: "Submit"

            // Triggered when clicking the button manually
            onClicked:
            {
                printUserSecretKey(userInputField.text)
            }
        }
        z:3
    }
}
