import QtQuick
import QtQuick.Controls

Button
{
    id: chooseAccountButton
    text: qsTr("Choose Another Account")
    visible: AuthClient.sessionActive

    // Layout and Padding
    implicitWidth: 240
    implicitHeight: 48
    anchors.horizontalCenter: parent.horizontalCenter

    // 1. Text Styling (Ensures text stands out sharply)
    contentItem: Text
    {
        text: chooseAccountButton.text
        font.pointSize: 14
        font.bold: true
        color: "#FFFFFF" // Crisp white text to contrast the vibrant background
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    // 2. Button Background Styling (Pops against greys/darks)
    background: Rectangle
    {
        // Dynamic color shifting for hover/press states
        color: chooseAccountButton.down ? "#0056b3" :
               chooseAccountButton.hovered ? "#007bff" : "#0066ee"

        radius: 8 // Smooth, modern rounded corners

        // Soft outer shadow to physically separate it from the flat background
        layer.enabled: true
        layer.effect: ShaderEffect {
            // Optional: Adds a native drop-shadow effect if MultiEffect is imported,
            // otherwise radius handles the basic flat pop nicely.
        }
    }

    onClicked:
    {
        AuthClient.sessionActive = false
        StockSearchController.tokenActive = false
        console.log("Redirecting user to account selection screen...")
    }
}

