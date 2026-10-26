import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "robinWebLoginPanel"
    spacing: 12

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        Layout.topMargin: 12
        spacing: 12
        Label {
            objectName: "robinBrowserOrigin"
            Layout.fillWidth: true
            text: RobinBrowser.loginUrl.toString()
            color: "#f3f9f0"
            wrapMode: Text.WrapAnywhere
            Accessible.name: qsTr("Official Robinhood sign-in: %1").arg(text)
        }
        Button {
            objectName: "cancelRobinBrowserLogin"
            text: qsTr("Cancel")
            enabled: RobinSessions.browserLoginPending
            onClicked: RobinSessions.cancelSignIn()
        }
    }
    Label {
        objectName: "robinBrowserError"
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        visible: text !== ""
        text: [RobinBrowser.error, RobinSessions.error].filter(message => message !== "").join("\n")
        color: "#ffb6a4"
        wrapMode: Text.Wrap
    }
    ProgressBar {
        objectName: "robinBrowserProgress"
        Layout.fillWidth: true
        Layout.leftMargin: 12
        Layout.rightMargin: 12
        visible: RobinSessions.busy
        indeterminate: true
        Accessible.name: RobinSessions.browserLoginPending
                         ? qsTr("Complete sign-in on Robinhood; session verification may be in progress")
                         : qsTr("Securely saving the verified session")
    }
    Loader {
        objectName: "robinWebLoginLoader"
        Layout.fillWidth: true
        Layout.fillHeight: true
        active: RobinBrowser.active
        source: RobinBrowser.viewSource
    }
}
