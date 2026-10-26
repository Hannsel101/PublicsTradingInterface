import QtQuick
import QtTest
import RobinHoodInterface as RH

TestCase {
    name: "RobinHoodSessions"
    when: windowShown
    width: 430
    height: 700
    visible: true

    function init() {
        RobinSessions.clear()
        RobinSessions.sessionActive = false
        RobinSessions.busy = false
        RobinSessions.approvalPending = false
        RobinSessions.approvalSecondsRemaining = 0
        RobinSessions.error = ""
        RobinSessions.lastStarted = -1
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign|Binding loop/)
    }
    function createView(width = 430, height = 700) {
        const component = Qt.createComponent("qrc:/qt/qml/RobinHoodInterface/CoreView.qml")
        compare(component.status, Component.Ready, component.errorString())
        const view = component.createObject(this, {width: width, height: height})
        verify(view !== null)
        return view
    }
    function test_emptyPickerAndDisabledAutoMode() {
        const view = createView()
        try {
            const autoMode = findChild(view, "robinAutoMode")
            const start = findChild(view, "startRobinSession")
            verify(findChild(view, "robinSessionList") !== null)
            verify(autoMode !== null)
            compare(autoMode.enabled, false)
            compare(start.enabled, false)
            verify(findChild(view, "sessionTimer") === null)
        } finally {
            view.destroy()
        }
    }
    function test_activeAndExpiredSelection() {
        RobinSessions.append({sessionIndex: 0, accountName: "active@example.test",
                              expiresAt: "2099-01-01T00:00:00.000Z", isExpired: false})
        RobinSessions.append({sessionIndex: 1, accountName: "expired@example.test",
                              expiresAt: "2000-01-01T00:00:00.000Z", isExpired: true})
        const view = createView()
        try {
            tryVerify(() => findChild(view, "robinSessionCard_0") !== null)
            tryVerify(() => findChild(view, "robinSessionCard_1") !== null)
            const active = findChild(view, "robinSessionCard_0")
            const expired = findChild(view, "robinSessionCard_1")
            verify(active !== null)
            verify(expired !== null)
            verify(active.color !== expired.color)
            mouseClick(active)
            tryCompare(view, "selectedIndex", 0)
            tryCompare(view, "selectedExpired", false)
            const start = findChild(view, "startRobinSession")
            tryCompare(start, "enabled", true)
            mouseClick(start)
            tryCompare(RobinSessions, "lastStarted", 0)
            mouseClick(expired)
            tryCompare(start, "enabled", false)
            const reauth = findChild(view, "reauthenticateRobinSession")
            tryCompare(reauth, "visible", true)
            mouseClick(reauth)
            tryCompare(RobinSessions, "count", 1)
            const email = findChild(view, "robinEmail")
            compare(email.text, "expired@example.test")
        } finally {
            view.destroy()
        }
    }
    function test_deviceApprovalCountdownAndCancellation() {
        const view = createView()
        try {
            view.page = 1
            const countdown = findChild(view, "robinApprovalCountdown")
            const cancel = findChild(view, "cancelRobinApproval")
            verify(countdown !== null)
            verify(cancel !== null)
            compare(countdown.visible, false)
            RobinSessions.approvalPending = true
            RobinSessions.approvalSecondsRemaining = 180
            RobinSessions.busy = true
            tryCompare(countdown, "visible", true)
            tryCompare(countdown, "text", "3:00 remaining")
            RobinSessions.approvalSecondsRemaining = 179
            tryCompare(countdown, "text", "2:59 remaining")
            mouseClick(cancel)
            tryCompare(RobinSessions, "approvalPending", false)
            tryCompare(countdown, "visible", false)
            verify(findChild(view, "sessionTimer") === null)
        } finally {
            view.destroy()
        }
    }
    function test_pickerFitsMobileAndDesktop_data() {
        return [{tag: "compact", width: 360, height: 620},
                {tag: "portrait", width: 390, height: 740},
                {tag: "desktop", width: 1000, height: 700}]
    }
    function test_pickerFitsMobileAndDesktop(data) {
        RobinSessions.append({sessionIndex: 3, accountName: "sample@example.test",
                              expiresAt: "2099-01-01T00:00:00.000Z", isExpired: false})
        const view = createView(data.width, data.height)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_3") !== null)
            const card = findChild(view, "robinSessionCard_3")
            const start = findChild(view, "startRobinSession")
            verify(card.mapToItem(view, 0, 0).x >= 0)
            verify(card.mapToItem(view, card.width, 0).x <= data.width)
            verify(start.mapToItem(view, start.width, 0).x <= data.width)
            verify(start.width >= 300)
        } finally {
            view.destroy()
        }
    }
}
