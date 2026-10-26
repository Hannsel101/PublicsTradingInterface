import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PublicsTradingInterface

ApplicationWindow {
    id: launcherWindow
    width: 1000
    height: 700
    minimumWidth: 360
    minimumHeight: 620
    visible: true
    title: qsTr("Publics Trading Interface")
    color: "#08090a"

    StackLayout {
        anchors.fill: parent
        currentIndex: Launcher.activeAppletId === "" ? 0 : 1

        ScrollView {
            id: home
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            contentHeight: choices.implicitHeight + 72
            ColumnLayout {
                id: choices
                width: home.availableWidth - 48
                x: 24
                y: 36
                spacing: 18

                Label {
                    text: qsTr("Choose an applet")
                    font.pixelSize: 30
                    font.bold: true
                    color: "#f7f8f8"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                Label {
                    text: qsTr("Your tools in one place")
                    color: "#d0d6e0"
                    Layout.fillWidth: true
                }
                AppletChoice {
                    objectName: "openPublics"
                    appletName: qsTr("Publics")
                    stripeColor: "#25285f"
                    Layout.fillWidth: true
                    onClicked: Launcher.activate("publics")
                }
                AppletChoice {
                    objectName: "openRobinHood"
                    appletName: qsTr("Robin Hood")
                    stripeColor: "#a3e635"
                    enabled: false
                    Layout.fillWidth: true
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0
            Rectangle {
                color: "#0f1011"
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    Button {
                        id: backButton
                        objectName: "returnHome"
                        text: qsTr("← Applets")
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 44
                        enabled: Launcher.canReturnHome
                        onClicked: Launcher.returnHome()
                        background: Rectangle {
                            radius: 6
                            color: backButton.pressed ? "#292b31" : "#202126"
                            border.color: backButton.activeFocus ? "#d0d6e0" : "#42444d"
                        }
                        contentItem: Text {
                            text: backButton.text
                            color: backButton.enabled ? "#f7f8f8" : "#8a8f98"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Label {
                        text: qsTr("Publics Trading")
                        color: "#f7f8f8"
                        font.bold: true
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                    }
                    Item { Layout.preferredWidth: 100 }
                }
            }
            Loader {
                id: appletLoader
                objectName: "appletLoader"
                Layout.fillWidth: true
                Layout.fillHeight: true
                active: Launcher.activeAppletId === "publics"
                sourceComponent: Component { CoreView {} }
            }
        }
    }
}
