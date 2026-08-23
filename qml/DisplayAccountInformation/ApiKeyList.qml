import QtQuick
import QtQuick.Controls

Item
{
    id: root
    width: 400
    height: 500

    property var apiKeysModel: AuthClient.secretKeys

    Text
    {
        id: titleText
        text: "Choose or Store an API Key"
        height: parent.height*0.1
        width: parent.width
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        horizontalAlignment: Text.AlignHCenter
        font.pointSize: 20
        color: "black"
    }

    Rectangle
    {
        id: listContainer
        width: parent.width
        height: parent.height*0.90
        anchors.top: titleText.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        color: "#1e1e24" // Dark sleek background

        ListView
        {
            id: keysListView
            anchors.fill: parent
            anchors.margins: 20
            model: apiKeysModel
            spacing: 8
            clip: true // Ensures content stays inside scroll boundary
            currentIndex: -1 // Nothing selected by default

            delegate: Rectangle
            {
                id: delegateRoot
                width: keysListView.width
                height: 55
                radius: 8

                // Core logic: Evaluate if this specific item is the active selection
                readonly property bool isSelected: index === keysListView.currentIndex

                readonly property string apiKeyValue: modelData

                // Soft transition animations for background and tint changes
                Behavior on color { ColorAnimation { duration: 200 } }

                // Dynamic color handling based on selection status
                color: isSelected ? "#2ecc71" : "#2d2d35"
                border.color: isSelected ? "#27ae60" : "#3f3f4a"
                border.width: 1

                Row
                {
                    anchors.fill: parent
                    anchors.leftMargin: 15
                    anchors.rightMargin: 15
                    spacing: 12


                    // Status Indicator
                    Rectangle
                    {
                        width: 10
                        height: 10
                        radius: 5
                        color: delegateRoot.isSelected ? "#ffffff" : "#7f8c8d"
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // API Key Name Label
                    Text
                    {
                        text: "Public Api Key " + index
                        color: delegateRoot.isSelected ? "#ffffff" : "#dcdde1"
                        font.pixelSize: 15
                        font.bold: delegateRoot.isSelected
                        anchors.verticalCenter: parent.verticalCenter
                        verticalAlignment: Text.AlignVCenter

                        // Dimming opacity rule when not active
                        opacity: delegateRoot.isSelected ? 1.0 : 0.65
                        Behavior on opacity { NumberAnimation { duration: 200 } }
                    }
                }

                // Foreground Grey Tint overlay for unselected elements
                Rectangle
                {
                    anchors.fill: parent
                    radius: parent.radius
                    color: "#111116"
                    opacity: delegateRoot.isSelected ? 0.0 : 0.25
                    visible: keysListView.currentIndex !== -1 // Only dim if a selection exists
                    Behavior on opacity { NumberAnimation { duration: 200 } }
                }

                // Click interaction interceptor
                MouseArea
                {
                    anchors.fill: parent
                    onClicked:
                    {
                        keysListView.currentIndex = index
                        AuthClient.secretKey = apiKeyValue
                        ApiWorker.clearSubAccountsList();
                        AuthClient.clearUserSession();
                    }
                }
            }

            // Standard Scrollbar implementation for desktop platforms
            ScrollBar.vertical: ScrollBar
            {
                policy: ScrollBar.AsNeeded
                active: true
            }
        }
    }
}
