import QtQuick 2.15
import QtQuick.Controls

Item
{
    visible: StockSearchController.tokenActive

        Column {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 50
            spacing: 10
            width: 300

            Label {
                text: "Search Stock Ticker:"
                font.bold: true
            }

            TextField {
                id: searchInput
                width: parent.width
                placeholderText: "Type 'AAPL', 'TSLA'..."

                // Forward changes to C++ backend
                onTextChanged: StockSearchController.updateSearch(text)
            }
        }

        // Dropdown popup anchored directly to the search Input
        Popup {
            id: suggestionPopup
            x: searchInput.x + searchInput.parent.x
            y: searchInput.y + searchInput.parent.y + searchInput.height
            width: searchInput.width
            height: Math.min(200, suggestionListView.contentHeight)
            padding: 0

            // Open/Close dynamically based on backend data and focus
            visible: searchInput.activeFocus && StockSearchController.suggestions.length > 0
            focus: false
            closePolicy: Popup.CloseOnPressOutside

            background: Rectangle {
                border.color: "#CCCCCC"
                radius: 4
            }

            ListView {
                id: suggestionListView
                anchors.fill: parent
                clip: true
                model: StockSearchController.suggestions

                delegate: ItemDelegate {
                    width: parent.width
                    text: modelData

                    onClicked: {
                        searchInput.text = modelData;
                        suggestionPopup.close();
                        searchInput.focus = false; // Defocus field if desired
                    }
                }
            }
        }
}
