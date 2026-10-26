pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    objectName: "robinHoodView"
    implicitWidth: 420
    implicitHeight: 640
    readonly property color canvas: "#0b1514"
    readonly property color surface: "#142522"
    readonly property color edge: "#36534b"
    readonly property color accent: "#a3e635"
    readonly property color secondary: "#55cbb4"
    readonly property color ink: "#f3f9f0"
    readonly property color muted: "#abc4b8"
    property int page: 0
    property int selectedIndex: -1
    property bool selectedExpired: false
    property string selectedExpiresAt: ""
    readonly property int view: RobinSessions.sessionActive ? 2 : page

    Connections {
        target: RobinSessions
        function onSessionSaved() {
            root.selectedIndex = -1
            root.selectedExpired = false
            root.selectedExpiresAt = ""
            root.page = 0
        }
        function onReauthenticationRequested(accountName) {
            root.selectedIndex = -1
            root.page = 1
            emailField.text = accountName
        }
    }
    Timer {
        interval: 30000
        repeat: true
        running: root.selectedIndex >= 0
        onTriggered: root.selectedExpired = Date.now() >= Date.parse(root.selectedExpiresAt)
    }

    Rectangle { anchors.fill: parent; color: root.canvas }

    Flickable {
        id: scroller
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: pageColumn.implicitHeight + 48
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: pageColumn
            x: Math.max(18, (scroller.width - 780) / 2)
            y: 24
            width: Math.min(scroller.width - 36, 780)
            spacing: 20

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 6
                radius: 3
                color: root.accent
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        text: qsTr("ROBIN HOOD  /  SESSION DESK")
                        color: root.secondary
                        font.pixelSize: 11
                        font.letterSpacing: 2
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.view === 2 ? qsTr("Account overview")
                              : root.view === 1 ? qsTr("Add an account") : qsTr("Your sessions")
                        color: root.ink
                        font.pixelSize: 28
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    radius: 14
                    color: root.surface
                    border.color: root.edge
                    Label {
                        anchors.centerIn: parent
                        text: "↗"
                        font.pixelSize: 22
                        color: root.accent
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.view === 0
                text: qsTr("Choose an account to continue. Expired sessions need a fresh sign-in.")
                color: root.muted
                wrapMode: Text.Wrap
                font.pixelSize: 14
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                visible: RobinSessions.error !== ""
                radius: 12
                color: "#54302e"
                border.color: "#e8907f"
                Label {
                    anchors.fill: parent
                    anchors.margins: 12
                    verticalAlignment: Text.AlignVCenter
                    text: RobinSessions.error
                    color: "#ffe4dc"
                    wrapMode: Text.Wrap
                    font.pixelSize: 13
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.view === 0
                spacing: 14
                Label {
                    text: qsTr("SAVED ACCOUNTS")
                    color: root.secondary
                    font.pixelSize: 12
                    font.bold: true
                    font.letterSpacing: 1.5
                }
                ListView {
                    id: savedList
                    objectName: "robinSessionList"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(Math.max(contentHeight, 130), Math.max(190, root.height * 0.5))
                    clip: true
                    spacing: 10
                    model: RobinSessions
                    delegate: Rectangle {
                        id: sessionCard
                        objectName: "robinSessionCard_" + sessionIndex
                        required property int sessionIndex
                        required property string accountName
                        required property string expiresAt
                        required property bool isExpired
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ListItem
                        Accessible.name: accountName
                        Accessible.description: isExpired ? qsTr("Expired session") : qsTr("Active session")
                        function choose() {
                            root.selectedIndex = sessionIndex
                            root.selectedExpiresAt = expiresAt
                            root.selectedExpired = isExpired
                        }
                        Keys.onReturnPressed: choose()
                        Keys.onSpacePressed: choose()
                        width: savedList.width
                        height: 110
                        radius: 16
                        color: isExpired ? "#49302d" : root.selectedIndex === sessionIndex ? "#244536" : root.surface
                        border.color: root.selectedIndex === sessionIndex ? root.accent
                                      : isExpired ? "#e8907f" : root.edge
                        border.width: root.selectedIndex === sessionIndex ? 2 : 1
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 5
                            Label {
                                Layout.fillWidth: true
                                text: sessionCard.accountName
                                color: root.ink
                                elide: Text.ElideRight
                                font.pixelSize: 16
                                font.bold: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: sessionCard.isExpired ? qsTr("Expired · Reauthentication required")
                                      : qsTr("Active · Ready to start")
                                color: sessionCard.isExpired ? "#ffb6a4" : root.accent
                                font.pixelSize: 13
                            }
                            Label {
                                text: qsTr("Expires %1").arg(Qt.formatDateTime(new Date(sessionCard.expiresAt), "MMM d, yyyy · h:mm AP"))
                                color: root.muted
                                font.pixelSize: 12
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: sessionCard.choose()
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: savedList.count === 0
                    text: qsTr("No sessions saved yet. Add your first account below.")
                    color: root.muted
                    wrapMode: Text.Wrap
                }
                Button {
                    id: startButton
                    objectName: "startRobinSession"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    text: RobinSessions.busy ? qsTr("Working…") : qsTr("Start Session")
                    enabled: root.selectedIndex >= 0 && !root.selectedExpired && !RobinSessions.busy
                    onClicked: RobinSessions.startSession(root.selectedIndex)
                    contentItem: Label {
                        text: startButton.text
                        color: startButton.enabled ? root.canvas : root.muted
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 14
                        color: startButton.enabled ? (startButton.down ? "#83bd2c" : root.accent) : "#31403a"
                    }
                }
                Button {
                    id: reauthButton
                    objectName: "reauthenticateRobinSession"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    visible: root.selectedIndex >= 0 && root.selectedExpired
                    enabled: !RobinSessions.busy
                    text: qsTr("Reauthenticate")
                    onClicked: RobinSessions.reauthenticate(root.selectedIndex)
                }
                Button {
                    objectName: "addRobinAccount"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    text: qsTr("+ Add another account")
                    enabled: !RobinSessions.busy
                    onClicked: {
                        emailField.clear()
                        root.page = 1
                        scroller.contentY = 0
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.view === 1
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Credentials are submitted only for sign-in. Your password is not saved; the returned session token is stored in the system keychain.")
                    color: root.muted
                    wrapMode: Text.Wrap
                    font.pixelSize: 14
                }
                TextField {
                    id: emailField
                    objectName: "robinEmail"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    placeholderText: qsTr("Account email")
                    inputMethodHints: Qt.ImhEmailCharactersOnly | Qt.ImhNoAutoUppercase
                    enabled: !RobinSessions.approvalPending
                    color: root.ink
                }
                TextField {
                    id: passwordField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    placeholderText: qsTr("Password")
                    echoMode: TextInput.Password
                    enabled: !RobinSessions.approvalPending
                    color: root.ink
                }
                TextField {
                    id: mfaField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 50
                    placeholderText: qsTr("MFA code (if needed)")
                    inputMethodHints: Qt.ImhDigitsOnly
                    enabled: !RobinSessions.approvalPending
                    color: root.ink
                }
                Label {
                    Layout.fillWidth: true
                    visible: RobinSessions.approvalPending
                    text: qsTr("Please check your phone and approve the login")
                    color: root.secondary
                    wrapMode: Text.Wrap
                    font.bold: true
                }
                Label {
                    objectName: "robinApprovalCountdown"
                    Layout.fillWidth: true
                    visible: RobinSessions.approvalPending
                    text: qsTr("%1:%2 remaining")
                          .arg(Math.floor(RobinSessions.approvalSecondsRemaining / 60))
                          .arg((RobinSessions.approvalSecondsRemaining % 60 < 10 ? "0" : "")
                               + RobinSessions.approvalSecondsRemaining % 60)
                    color: root.accent
                    font.pixelSize: 22
                }
                Button {
                    objectName: "cancelRobinApproval"
                    visible: RobinSessions.approvalPending
                    text: qsTr("Cancel device approval")
                    onClicked: RobinSessions.cancelSignIn()
                }
                Button {
                    id: signInButton
                    objectName: "robinSignIn"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    enabled: !RobinSessions.busy && emailField.text.trim() !== "" && passwordField.text !== ""
                    text: RobinSessions.approvalPending ? qsTr("Awaiting device approval…")
                          : RobinSessions.busy ? qsTr("Signing in…") : qsTr("Sign in & save session")
                    onClicked: {
                        RobinSessions.signIn(emailField.text, passwordField.text, mfaField.text)
                        passwordField.clear()
                        mfaField.clear()
                    }
                    contentItem: Label {
                        text: signInButton.text
                        color: signInButton.enabled ? root.canvas : root.muted
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 14
                        color: signInButton.enabled ? root.accent : "#31403a"
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Device approval is supported. SMS and email verification challenges still require another sign-in method.")
                    color: root.muted
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                }
                Button {
                    objectName: "backToRobinSessions"
                    text: qsTr("← Saved sessions")
                    enabled: !RobinSessions.busy || RobinSessions.approvalPending
                    onClicked: {
                        if (RobinSessions.approvalPending)
                            RobinSessions.cancelSignIn()
                        root.page = 0
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.view === 2
                spacing: 14
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Signed in as %1").arg(RobinSessions.activeAccount)
                    color: root.muted
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: RobinSessions.accounts
                    delegate: Rectangle {
                        id: accountTile
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 82
                        radius: 16
                        color: root.surface
                        border.color: root.edge
                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 14
                            Label {
                                text: qsTr("ACCOUNT  /  %1").arg(accountTile.modelData.accountNumber || qsTr("Unavailable"))
                                color: root.accent
                                font.bold: true
                            }
                            Label {
                                text: accountTile.modelData.type || qsTr("Brokerage")
                                color: root.muted
                            }
                        }
                    }
                }
                Label {
                    visible: RobinSessions.accounts.length === 0
                    text: qsTr("No brokerage accounts were returned for this session.")
                    color: root.muted
                }
                Button {
                    objectName: "switchRobinAccount"
                    text: qsTr("Switch account")
                    enabled: !RobinSessions.busy
                    onClicked: {
                        RobinSessions.clearActiveSession()
                        root.page = 0
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 68
                radius: 14
                color: "#29352f"
                border.color: "#4c5b52"
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Auto Mode")
                        color: "#9da9a0"
                        font.bold: true
                    }
                    Button {
                        objectName: "robinAutoMode"
                        text: qsTr("Unavailable")
                        enabled: false
                        Layout.preferredWidth: 120
                    }
                }
            }
        }
    }
}
