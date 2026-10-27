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
    onSelectedIndexChanged: {
        selectedCanRefresh = false
        sessionTimeMs = Date.now()
        informationAccount = null
    }
    property var informationAccount: null
    Loader {
        active: root.informationAccount !== null
        sourceComponent: AccountInfoDialog {
            parent: root
            account: root.informationAccount
            anchors.centerIn: parent
            width: Math.min(root.width - 24, 560)
            height: Math.min(root.height - 24, 540)
            Component.onCompleted: open()
            onClosed: root.informationAccount = null
        }
    }
    property int page: 0
    property int selectedIndex: -1
    property bool selectedExpired: false
    property bool selectedCanRefresh: false
    property string selectedExpiresAt: ""
    property double sessionTimeMs: Date.now()
    readonly property bool selectedNeedsReauthentication: selectedIndex >= 0
        && (selectedExpired || Date.parse(selectedExpiresAt) <= sessionTimeMs + 2 * 60 * 60 * 1000)
    onSelectedExpiresAtChanged: {
        sessionTimeMs = Date.now()
        selectedExpired = sessionTimeMs >= Date.parse(selectedExpiresAt)
    }
    property string selectedAccountName: ""
    property string selectedAccountId: ""
    readonly property int view: RobinSessions.nativeLoginPending ? 3
                                : RobinSessions.sessionActive ? 2 : 0

    Connections {
        target: RobinSessions
        function onAccountCacheChanged() {
            root.informationAccount = null
        }
        function onSessionActiveChanged() {
            root.informationAccount = null

        }
        function onSessionSaved() {
            root.selectedIndex = -1
            root.selectedExpired = false
            root.selectedExpiresAt = ""
            root.selectedAccountName = ""
            root.selectedAccountId = ""
            root.page = 0
        }
        function onSessionDeleted(index) {
            // Discard private presentation state on any profile deletion.
            root.informationAccount = null
            if (root.selectedIndex === index) {
                root.selectedIndex = -1
                root.selectedExpired = false
                root.selectedExpiresAt = ""
                root.selectedAccountName = ""
                root.selectedAccountId = ""
            }
        }
    }
    Timer {
        objectName: "robinSessionExpiryTimer"
        interval: 30000
        repeat: true
        running: root.selectedIndex >= 0
        onTriggered: {
            root.sessionTimeMs = Date.now()
            root.selectedExpired = root.sessionTimeMs >= Date.parse(root.selectedExpiresAt)
        }
    }

    Rectangle { anchors.fill: parent; color: root.canvas }

    Flickable {
        id: scroller
        objectName: "robinSessionScroller"
        visible: root.view !== 3 && !RobinTrades.tradeResultsVisible
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
                        text: root.view === 2 ? qsTr("Account Overview") : qsTr("Your Sessions")
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
                Layout.preferredHeight: Math.max(52, errorLabel.implicitHeight + 24)
                visible: RobinSessions.error !== ""
                radius: 12
                color: "#54302e"
                border.color: "#e8907f"
                Label {
                    id: errorLabel
                    anchors.fill: parent
                    anchors.margins: 12
                    verticalAlignment: Text.AlignVCenter
                    text: RobinSessions.error
                    textFormat: Text.PlainText
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
                        required property string accountId
                        required property string expiresAt
                        required property bool isExpired
                        required property var model
                        onExpiresAtChanged: {
                            if (root.selectedIndex === sessionIndex)
                                root.selectedExpiresAt = expiresAt
                        }
                        readonly property bool canRefresh: Boolean(model.canRefresh)
                        onCanRefreshChanged: {
                            if (root.selectedIndex === sessionIndex)
                                root.selectedCanRefresh = canRefresh
                        }
                        activeFocusOnTab: true
                        Accessible.role: Accessible.ListItem
                        Accessible.name: accountName
                        Accessible.description: isExpired ? (canRefresh ? qsTr("Expired access token · Refresh available") : qsTr("Expired session")) : qsTr("Active session")
                        function choose() {
                            root.selectedIndex = sessionIndex
                            root.selectedExpiresAt = expiresAt
                            root.selectedExpired = isExpired
                            root.selectedCanRefresh = canRefresh
                            root.selectedAccountName = accountName
                            root.selectedAccountId = accountId
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
                                textFormat: Text.PlainText
                                color: root.ink
                                elide: Text.ElideRight
                                font.pixelSize: 16
                                font.bold: true
                            }
                            Label {
                                Layout.fillWidth: true
                                text: sessionCard.isExpired
                                      ? (sessionCard.canRefresh ? qsTr("Access expired · Refresh available") : qsTr("Expired · Reauthentication required"))
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
                    font.pixelSize: 18
                    font.bold: true
                    visible: !root.selectedExpired || root.selectedCanRefresh
                    Layout.fillWidth: true
                    Layout.preferredHeight: 52
                    text: RobinSessions.busy ? qsTr("Working…")
                          : root.selectedExpired && root.selectedCanRefresh ? qsTr("Renew Session") : qsTr("Start Session")
                    enabled: root.selectedIndex >= 0 && (!root.selectedExpired || root.selectedCanRefresh) && !RobinSessions.busy
                    onClicked: RobinSessions.startSession(root.selectedIndex)
                    contentItem: Label {
                        text: startButton.text
                        color: startButton.enabled ? root.canvas : root.muted
                        font: startButton.font
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
                    visible: root.selectedNeedsReauthentication
                    font.pixelSize: 18
                    font.bold: true
                    enabled: !RobinSessions.busy
                    text: qsTr("Reauthenticate")
                    onClicked: {
                        root.page = 0
                        scroller.contentY = 0
                        RobinSessions.reauthenticate(root.selectedIndex)
                    }
                }
                Button {
                    id: deleteButton
                    objectName: "deleteRobinSession"
                    font.pixelSize: 18
                    font.bold: true
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    text: qsTr("Delete Saved Account")
                    enabled: root.selectedIndex >= 0 && !RobinSessions.busy
                    onClicked: {
                        if (deleteDialogLoader.status !== Loader.Ready)
                            return
                        deleteDialogLoader.item.pendingIndex = root.selectedIndex
                        deleteDialogLoader.item.pendingAccountName = root.selectedAccountName
                        deleteDialogLoader.item.open()
                    }
                    contentItem: Label {
                        text: deleteButton.text
                        font: deleteButton.font
                        color: deleteButton.enabled ? "#ffe4dc" : root.muted
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 14
                        color: deleteButton.enabled ? "#54302e" : "#31403a"
                        border.color: deleteButton.enabled ? "#e8907f" : root.edge
                    }
                }
                Button {
                    objectName: "addRobinAccount"
                    font.pixelSize: 18
                    font.bold: true
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    text: qsTr("+ Add Another Account")
                    enabled: !RobinSessions.busy
                    onClicked: RobinSessions.beginNativeLogin()
                }

            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.view === 2
                spacing: 14
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Signed in as %1").arg(RobinSessions.activeAccount)
                    textFormat: Text.PlainText
                    color: root.muted
                    wrapMode: Text.Wrap
                }
                Repeater {
                    id: accountRepeater
                    model: RobinSessions.accounts
                    delegate: AccountCard {
                        required property var model
                        Layout.fillWidth: true
                        displayName: model.displayName
                        accountNumber: model.accountNumber
                        currentBalance: model.currentBalance
                        holdings: model.holdings || []
                        holdingsUpdatedAt: model.holdingsUpdatedAt || ""
                        holdingsStatus: model.holdingsStatus || "unavailable"
                        holdingsError: model.holdingsError || ""
                        onInformationRequested: account => root.informationAccount = account
                    }
                }
                Label {
                    visible: accountRepeater.count === 0
                    text: qsTr("No brokerage accounts were returned for this session.")
                    color: root.muted
                }
                Loader {
                    Layout.fillWidth: true
                    active: root.view === 2
                    visible: active
                    sourceComponent: StockTrade {}
                }
                Button {
                    objectName: "switchRobinAccount"
                    text: qsTr("Switch account")
                    enabled: RobinSessions.sessionActive && !RobinTrades.tradeResultsBusy
                    onClicked: {
                        RobinSessions.clearActiveSession()
                        root.page = 0
                    }
                }
            }

            Rectangle {
                visible: root.view === 0
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
                        font.pixelSize: 18
                        color: "#9da9a0"
                        font.bold: true
                    }
                    Button {
                        objectName: "robinAutoMode"
                        font.pixelSize: 18
                        font.bold: true
                        text: qsTr("Unavailable")
                        enabled: false
                        Layout.preferredWidth: Math.max(120, implicitWidth)
                    }
                }
            }
        }
    }
    Loader {
        anchors.fill: parent
        active: root.view === 3
        visible: active
        sourceComponent: NativeLoginForm {}
    }
    Loader {
        objectName: "robinResultsLoader"
        anchors.fill: parent
        active: RobinTrades.tradeResultsVisible
        visible: active
        sourceComponent: TradeResultScreen {}
    }
    Loader {
        id: deleteDialogLoader
        active: root.selectedIndex >= 0
        sourceComponent: Component {
            Dialog {
                id: deleteDialog
                parent: root
                objectName: "deleteRobinSessionDialog"
                property int pendingIndex: -1
                property string pendingAccountName: ""
                anchors.centerIn: parent
                width: Math.min(root.width - 36, 420)
                modal: true
                focus: true
                title: qsTr("Delete saved account?")
                standardButtons: Dialog.Yes | Dialog.Cancel
                onAccepted: RobinSessions.deleteSavedSession(pendingIndex)
                contentItem: Label {
                    text: qsTr("Remove the saved login for %1 from this device? This does not close or delete your Robinhood brokerage accounts.").arg(deleteDialog.pendingAccountName)
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: root.ink
                    padding: 12
                }
                background: Rectangle {
                    radius: 14
                    color: root.surface
                    border.color: root.edge
                }
            }
        }
    }
}
