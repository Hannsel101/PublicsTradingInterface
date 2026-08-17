import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item
{
    id: root
    width: 440
    height: 320
    visible: true

    // Main layout container holding all structural UI rows sequentially
    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width * 0.85
        spacing: 16

        // SECTION 1: Result Display Area (Positioned above the text input field)
        Rectangle {
            id: resultContainer
            Layout.fillWidth: true
            height: 70
            color: "#ffffff"
            radius: 8
            border.color: "#e5e5ea"
            border.width: 1

            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width * 0.9
                spacing: 4

                Label {
                    text: "OUTPUT CHANNEL"
                    font.pixelSize: 10
                    font.bold: true
                    color: "#8e8e93" // Muted gray accent for structural labeling
                    Layout.alignment: Qt.AlignHCenter
                }

                Label {
                    id: resultLabel
                    text: "Awaiting execution request..."
                    font.pixelSize: 15
                    font.weight: Font.Medium
                    color: "#1c1c1e" // Standard clean dark text
                    horizontalAlignment: Text.AlignHCenter
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                }
            }
        }

        // Decorative separating line to distinct output from input
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#d1d1d6"
        }

        // SECTION 2: User Input Field
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 6

            Label {
                text: "Source String Input"
                font.pixelSize: 12
                font.bold: true
                color: "#1c1c1e"
            }

            TextField {
                id: inputField
                placeholderText: "Type text data here..."
                Layout.fillWidth: true
                font.pixelSize: 14
                padding: 12
                selectByMouse: true

                // Custom look and feel mimicking modern application frames
                background: Rectangle {
                    radius: 6
                    border.color: inputField.activeFocus ? "#007aff" : "#c7c7cc"
                    border.width: inputField.activeFocus ? 2 : 1
                    color: "#ffffff"
                }
            }
        }

        // SECTION 3: Operation Actions Container (Two distinctive button routines)
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Button Routine A: Uppercase Transformer Logic
            Button {
                id: btnUppercase
                text: "Transform: UPPERCASE"
                Layout.fillWidth: true

                contentItem: Text {
                    text: btnUppercase.text
                    font.pixelSize: 13
                    font.bold: true
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: btnUppercase.down ? "#0051a8" : (btnUppercase.hovered ? "#0062cc" : "#007aff")
                }

                onClicked: {
                    // Operational validation constraint checking
                    if (inputField.text.trim() === "") {
                        resultLabel.text = "Error: Input element cannot be blank."
                        resultContainer.border.color = "#ff3b30" // Red boundary fault alert
                    } else {
                        // Execution processing logic
                        resultLabel.text = inputField.text.toUpperCase()
                        resultContainer.border.color = "#34c759" // Green validation success alert
                    }
                }
            }

            // Button Routine B: Text Reversing Vector Logic
            Button {
                id: btnReverse
                text: "Operation: REVERSE"
                Layout.fillWidth: true

                contentItem: Text {
                    text: btnReverse.text
                    font.pixelSize: 13
                    font.bold: true
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: 6
                    color: btnReverse.down ? "#434190" : (btnReverse.hovered ? "#4c49a1" : "#5856d6")
                }

                onClicked: {
                    // Operational validation constraint checking
                    if (inputField.text.trim() === "") {
                        resultLabel.text = "Error: Input element cannot be blank."
                        resultContainer.border.color = "#ff3b30" // Red boundary fault alert
                    } else {
                        // Execution processing logic: splits text by indices array, inverts structural positioning, recombines into a flat string
                        let reversedString = inputField.text.split("").reverse().join("");
                        resultLabel.text = reversedString
                        resultContainer.border.color = "#34c759" // Green validation success alert
                    }
                }
            }
        }
    }
}
