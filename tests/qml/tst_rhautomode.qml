import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtTest
import RobinHoodInterface as RH

TestCase {
    name: "RobinAutoMode"
    when: windowShown
    visible: true
    width: 1000
    height: 740
    Component { id: core; RH.CoreView {} }
    function init() {
        RobinAutoTrades.reset(); RobinTrades.reset(); RobinSessions.clear()
        RobinSessions.accounts.clear(); RobinSessions.sessionActive = false
        RobinSessions.nativeLoginPending = false; RobinSessions.busy = false
        RobinSessions.lastStarted = -1; RobinSessions.beginNativeCalls = 0
        RobinSessions.reauthenticateCalls = 0; RobinSessions.error = ""
        failOnWarning(/ReferenceError|TypeError|Cannot assign|Binding loop/)
    }
    function sizes() { return [{tag: "compact", width: 360, height: 620}, {tag: "desktop", width: 1000, height: 700}] }
    function openView(data) {
        const view = createTemporaryObject(core, this, {width: data.width, height: data.height})
        verify(view !== null)
        const button = findChild(view, "robinAutoMode")
        compare(button.enabled, true)
        const scroll = findChild(view, "robinSessionScroller")
        waitForPolish(view.Window.window)
        scroll.contentY = Math.max(0, scroll.contentHeight - scroll.height)
        button.forceActiveFocus(); keyClick(Qt.Key_Space)
        tryVerify(() => findChild(view, "robinAutoModeView") !== null)
        return view
    }
    function test_readOnlySessions_data() { return sizes() }
    function test_readOnlySessions(data) {
        for (let i = 0; i < 3; ++i)
            RobinSessions.append({sessionIndex: i, accountName: "Login " + i, accountId: "PRIVATE-" + i,
                expiresAt: i === 0 ? "2099-01-01T00:00:00Z" : i === 1 ? "2000-01-01T00:00:00Z" : "invalid", isExpired: false})
        const view = openView(data)
        const list = findChild(view, "robinAutoSessions")
        verify(list !== null)
        compare(list.count, 3)
        tryVerify(() => list.itemAtIndex(2) !== null)
        for (let i = 0; i < 3; ++i) {
            const row = list.itemAtIndex(i)
            compare(findChild(row, "robinAutoSessionName_" + i).text, "Login " + i)
            const status = findChild(row, "robinAutoSessionStatus_" + i)
            compare(status.text, i === 0 ? "Active" : "Re-Authentication Required")
            compare(status.parent.color, i === 0 ? "#a3e635" : "#ef8175")
            mouseClick(row)
        }
        compare(RobinSessions.lastStarted, -1); compare(RobinSessions.beginNativeCalls, 0)
        compare(RobinSessions.reauthenticateCalls, 0)
        const auto = findChild(view, "robinAutoModeView")
        auto.presentationTimeMs = Date.parse("2099-01-01T00:00:00Z")
        tryCompare(findChild(list.itemAtIndex(0), "robinAutoSessionStatus_0"), "text", "Re-Authentication Required")
        RobinSessions.setProperty(0, "expiresAt", "2100-01-01T00:00:00Z")
        tryCompare(findChild(list.itemAtIndex(0), "robinAutoSessionStatus_0"), "text", "Active")
        auto.presentationTimeMs = Date.parse("2100-01-01T00:00:00Z")
        const timer = findChild(auto, "robinAutoExpiryTimer")
        timer.interval = 20
        tryCompare(findChild(list.itemAtIndex(0), "robinAutoSessionStatus_0"), "text", "Active")
    }
    function test_submission_data() { return sizes() }
    function test_submission(data) {
        const view = openView(data)
        const input = findChild(view, "robinAutoTickers")
        verify(input !== null)
        const buy = findChild(view, "robinAutoBuy")
        const sell = findChild(view, "robinAutoSell")
        compare(buy.enabled, false)
        input.forceActiveFocus()
        keyClick(Qt.Key_A); keyClick(Qt.Key_B); keyClick(Qt.Key_C)
        compare(input.text, "ABC")
        input.cursorPosition = 1; keyClick(Qt.Key_Z)
        compare(input.text, "AZBC"); compare(input.cursorPosition, 2)
        input.selectAll(); RobinTrades.typeText(input, "aapl, brk.b, ßé")
        compare(input.text, "AAPL, BRK.B, ßé"); compare(buy.enabled, false)
        input.selectAll(); RobinTrades.typeText(input, " aapl, brk.b ")
        compare(input.text, " AAPL, BRK.B ")
        compare(input.Accessible.name, "Ticker symbols, comma-separated")
        for (const button of [buy, sell]) {
            verify(button.mapToItem(view, 0, 0).x >= 0)
            verify(button.mapToItem(view, button.width, button.height).x <= view.width)
            verify(button.mapToItem(view, 0, button.height).y <= view.height)
            verify(button.contentItem.implicitWidth <= button.availableWidth + 1)
        }
        verify(buy.mapToItem(view, buy.width, 0).x <= sell.mapToItem(view, 0, 0).x)
        input.forceActiveFocus(); keyClick(Qt.Key_Tab)
        tryCompare(buy, "activeFocus", true)
        keyClick(Qt.Key_Space)
        compare(RobinAutoTrades.lastInput, " AAPL, BRK.B "); compare(RobinAutoTrades.lastSide, "buy")
        compare(buy.enabled, false); compare(sell.enabled, false); compare(input.enabled, false)
        compare(findChild(view, "robinAutoBack").enabled, false)
        RobinSessions.sessionActive = true; RobinSessions.sessionSaved(0)
        tryVerify(() => findChild(view, "robinAutoModeView") !== null)
        compare(view.view, 4)
        compare(findChild(view, "robinSessionScroller").visible, false)
        compare(findChild(view, "switchRobinAccount").enabled, false)
        compare(findChild(view, "addRobinAccount").enabled, false)
        keyClick(Qt.Key_Escape)
        compare(RobinAutoTrades.cancelCalls, 0)
        mouseClick(findChild(view, "robinAutoCancel"))
        compare(RobinAutoTrades.cancelCalls, 1)
        mouseClick(sell)
        compare(RobinAutoTrades.lastSide, "sell")
        RobinAutoTrades.cancel()
        mouseClick(findChild(view, "robinAutoBack"))
        tryCompare(findChild(view, "robinAutoModeLoader"), "active", false)
        compare(RobinSessions.sessionActive, false)
    }
    function test_results_data() { return sizes() }
    function test_results(data) {
        const view = openView(data)
        const rows = [
            {status: "sent", success: true, preview: false},
            {status: "preview", success: true, preview: true},
            {status: "network_error", success: false, preview: false},
            {status: "not_submitted", success: false, preview: false},
            {status: "rejected", success: false, preview: false},
            {status: "pending", success: false, preview: false}
        ].map(row => Object.assign(row, {sessionName: "<b>Login</b>", profileId: "PRIVATE-ID", accountNumber: "100", symbol: "AAPL", side: "buy", message: "<b>Diagnostic</b>"}))
        RobinAutoTrades.tradeResults = rows
        const list = findChild(view, "robinAutoResults")
        verify(list !== null)
        compare(list.count, rows.length)
        const mode = findChild(view, "robinAutoExecutionMode")
        verify(waitForPolish(view.Window.window))
        verify(list.height > 80)
        verify(list.mapToItem(view, list.width, list.height).y <= view.height)
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(view).save(qmlTestArtifactDirectory + "/robin-auto-results-" + data.tag + ".png")
        compare(mode.text, "Debug preview · No orders sent")
        RobinAutoTrades.liveOrdersEnabled = true
        compare(mode.text, "LIVE ORDERS")
        for (let i = 0; i < rows.length; ++i) {
            list.positionViewAtIndex(i, ListView.Contain)
            tryVerify(() => list.itemAtIndex(i) !== null)
            const row = list.itemAtIndex(i)
            const status = findChild(row, "robinAutoResultStatus_" + i)
            compare(status.text, i === 0 ? "Sent successfully · Not fill-confirmed"
                : i === 1 ? "Debug preview · No order sent"
                : rows[i].status === "pending" ? "Waiting / Submitting" : "Not sent successfully · " + rows[i].status)
            compare(status.color, i === 0 ? "#a3e635" : i === 1 ? "#f6d365"
                : rows[i].status === "pending" ? "#abc4b8" : "#ef8175")
            compare(findChild(row, "robinAutoResultIdentity_" + i).text, "<b>Login</b> · 100 · AAPL · buy")
            compare(findChild(row, "robinAutoResultIdentity_" + i).textFormat, Text.PlainText)
            compare(findChild(row, "robinAutoResultMessage_" + i).textFormat, Text.PlainText)
        }
        RobinAutoTrades.error = "<b>Network unavailable</b>"
        compare(findChild(view, "robinAutoError").textFormat, Text.PlainText)
        compare(findChild(view, "robinAutoError").text, RobinAutoTrades.error)
        mouseClick(findChild(view, "robinAutoClearResults"))
        tryCompare(list, "count", 0)
    }
    function labels(item) {
        let result = item.text !== undefined ? [item.text] : []
        for (const child of item.children) result = result.concat(labels(child))
        return result
    }
    function test_expirationTimerAndAllSessions() {
        const view = openView({width: 360, height: 620})
        const list = findChild(view, "robinAutoSessions")
        for (let i = 0; i < 40; ++i)
            RobinSessions.append({sessionIndex: i, accountName: "Login " + i, accountId: "PRIVATE-ID-" + i,
                expiresAt: "2099-01-01T00:00:00Z", isExpired: true})
        tryCompare(list, "count", 40)
        list.positionViewAtIndex(39, ListView.End)
        tryVerify(() => list.itemAtIndex(39) !== null)
        compare(findChild(list.itemAtIndex(39), "robinAutoSessionName_39").text, "Login 39")
        const texts = labels(list.itemAtIndex(39)).filter(text => text.length > 0)
        compare(texts.length, 2)
        compare(texts[0], "Login 39"); compare(texts[1], "Active")
        list.positionViewAtIndex(0, ListView.Beginning)
        tryVerify(() => list.itemAtIndex(0) !== null)
        const status = findChild(list.itemAtIndex(0), "robinAutoSessionStatus_0")
        RobinSessions.setProperty(0, "expiresAt", new Date(Date.now() + 150).toISOString())
        compare(status.text, "Active")
        findChild(view, "robinAutoExpiryTimer").interval = 20
        tryCompare(status, "text", "Re-Authentication Required")
        compare(RobinSessions.lastStarted, -1)
    }
    function test_navigation_data() { return sizes() }
    function test_navigation(data) {
        const view = openView(data)
        const loader = findChild(view, "robinAutoModeLoader")
        tryVerify(() => loader.item.width === view.width && loader.item.height === view.height)
        compare(findChild(view, "robinSessionScroller").visible, false)
        compare(RobinSessions.lastStarted, -1)
        compare(RobinSessions.beginNativeCalls, 0)
        const back = findChild(view, "robinAutoBack")
        mouseClick(back)
        tryCompare(loader, "active", false)
        compare(loader.visible, false)
        tryCompare(findChild(view, "robinSessionScroller"), "visible", true)
    }
}
