pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

FocusScope {
    id: root
    objectName: "robinNativeLoginForm"
    readonly property bool editable: RobinSessions.nativeLoginPending && !RobinSessions.nativeLoginWorking
                                     && RobinSessions.nativeLoginCancellable && !RobinSessions.nativeCodeRequired
    component CredentialField: TextField {
        id: field
        color: "#f3f9f0"
        selectionColor: "#36534b"
        selectedTextColor: "#f3f9f0"
        background: Rectangle {
            color: "#142522"
            radius: 10
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? "#a3e635" : "#36534b"
        }
    }
    component ActionButton: Button {
        id: action
        background: Rectangle {
            radius: 12
            color: action.down ? "#244536" : "#142522"
            border.color: action.activeFocus ? "#a3e635" : "#36534b"
            border.width: action.activeFocus ? 2 : 1
        }
        contentItem: Label {
            text: action.text
            color: action.enabled ? "#f3f9f0" : "#abc4b8"
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
    function clearSecrets() {
        password.clear()
        authenticator.clear()
        verification.clear()
    }
    function cancel() {
        if (!RobinSessions.nativeLoginCancellable)
            return
        clearSecrets()
        RobinSessions.cancelSignIn()
    }
    onVisibleChanged: if (!visible) clearSecrets()
    Component.onDestruction: clearSecrets()
    Component.onCompleted: email.forceActiveFocus()
    Connections {
        target: RobinSessions
        function onSessionSaved() { root.clearSecrets() }
    }
    Keys.onEscapePressed: root.cancel()
    Rectangle { anchors.fill: parent; color: "#0b1514" }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 18
        spacing: 12
        Flickable {
            id: scroller
            objectName: "robinNativeLoginScroller"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: fields.implicitHeight + 24
            clip: true
            activeFocusOnTab: true
            KeyNavigation.tab: RobinSessions.nativeCodeRequired ? verification : email
            Rectangle {
                parent: scroller
                anchors.fill: parent
                color: "#00000000"
                border.color: "#a3e635"
                border.width: 2
                visible: scroller.activeFocus
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_End)
                    contentY = Math.max(0, contentHeight - height)
                else if (event.key === Qt.Key_Home)
                    contentY = 0
                else if (event.key === Qt.Key_PageDown)
                    contentY = Math.min(Math.max(0, contentHeight - height), contentY + height)
                else if (event.key === Qt.Key_PageUp)
                    contentY = Math.max(0, contentY - height)
                else
                    return
                event.accepted = true
            }
            ScrollBar.vertical: ScrollBar {}
            function reveal(item) {
                const y = item.mapToItem(contentItem, 0, 0).y
                if (y < contentY) contentY = y
                else if (y + item.height > contentY + height)
                    contentY = y + item.height - height
            }
            ColumnLayout {
                id: fields
                width: Math.min(scroller.width, 540)
                x: (scroller.width - width) / 2
                spacing: 12
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 6; radius: 3; color: "#a3e635" }
                Label { text: qsTr("ROBIN HOOD  /  SECURE SIGN-IN"); color: "#55cbb4"; font.pixelSize: 11; font.letterSpacing: 1.5 }
                Label { text: qsTr("Sign in to Robinhood"); color: "#f3f9f0"; font.pixelSize: 26; font.bold: true; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Your email and password are saved securely only after your login and account are verified.")
                    color: "#abc4b8"; wrapMode: Text.Wrap
                }
                Label { text: qsTr("Email"); color: "#f3f9f0" }
                CredentialField {
                    id: email
                    objectName: "robinNativeEmail"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    text: RobinSessions.nativeLoginEmail
                    enabled: root.editable
                    Accessible.name: qsTr("Robinhood email")
                    inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoPredictiveText
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(email)
                    KeyNavigation.tab: password
                    onAccepted: password.forceActiveFocus()
                }
                Label { text: qsTr("Password"); color: "#f3f9f0" }
                CredentialField {
                    id: password
                    objectName: "robinNativePassword"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    enabled: root.editable
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                    Accessible.name: qsTr("Robinhood password")
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(password)
                    KeyNavigation.tab: authenticator
                    onAccepted: if (signIn.enabled) signIn.clicked()
                }
                Label { text: qsTr("Authenticator code (optional)"); color: "#f3f9f0" }
                CredentialField {
                    id: authenticator
                    objectName: "robinNativeAuthenticator"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    enabled: root.editable
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhDigitsOnly
                    Accessible.name: qsTr("Optional authenticator code")
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(authenticator)
                    KeyNavigation.tab: signIn
                    onAccepted: if (signIn.enabled) signIn.clicked()
                }
                Button {
                    id: signIn
                    objectName: "signInRobinNative"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    text: qsTr("Sign in")
                    enabled: root.editable && email.text.trim().length > 0 && password.text.length > 0
                    onClicked: {
                        if (enabled && RobinSessions.signIn(email.text.trim(), password.text, authenticator.text))
                            root.clearSecrets()
                    }
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(signIn)
                    KeyNavigation.tab: cancelButton
                    background: Rectangle { radius: 12; color: signIn.enabled ? "#a3e635" : "#31403a" }
                    contentItem: Label { text: signIn.text; color: signIn.enabled ? "#0b1514" : "#abc4b8"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.bold: true }
                }
                Label {
                    objectName: "robinNativeStatus"
                    Layout.fillWidth: true
                    text: !RobinSessions.nativeLoginCancellable ? qsTr("Saving securely… Please wait.") : RobinSessions.nativeLoginStatus
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: "#55cbb4"
                    Accessible.name: text
                }
                Label {
                    objectName: "robinNativeError"
                    Layout.fillWidth: true
                    text: RobinSessions.error
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: "#ffe4dc"
                    Accessible.name: text
                }
                Label { visible: RobinSessions.nativeCodeRequired; text: qsTr("SMS or email verification code"); color: "#f3f9f0"; Layout.fillWidth: true; wrapMode: Text.Wrap }
                CredentialField {
                    id: verification
                    objectName: "robinNativeVerificationCode"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    visible: RobinSessions.nativeCodeRequired
                    enabled: visible && !RobinSessions.nativeLoginWorking && RobinSessions.nativeLoginCancellable
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhDigitsOnly
                    Accessible.name: qsTr("SMS or email verification code")
                    onVisibleChanged: {
                        if (!visible) clear()
                        else forceActiveFocus()
                    }
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(verification)
                    KeyNavigation.tab: submitCode
                    onAccepted: if (submitCode.enabled) submitCode.clicked()
                }
                ActionButton {
                    id: submitCode
                    objectName: "submitRobinNativeCode"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    visible: RobinSessions.nativeCodeRequired
                    text: qsTr("Submit code")
                    enabled: verification.enabled && verification.text.trim().length > 0
                    onClicked: {
                        if (!enabled) return
                        RobinSessions.submitNativeVerificationCode(verification.text.trim())
                        verification.clear()
                    }
                    onActiveFocusChanged: if (activeFocus) scroller.reveal(submitCode)
                    KeyNavigation.tab: cancelButton
                }
            }
        }
        ActionButton {
            id: cancelButton
            objectName: "cancelRobinNativeLogin"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            text: qsTr("Cancel sign-in")
            enabled: RobinSessions.nativeLoginCancellable
            onClicked: root.cancel()
            KeyNavigation.tab: scroller
        }
    }
}
