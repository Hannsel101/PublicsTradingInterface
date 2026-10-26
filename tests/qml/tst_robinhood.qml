import QtQuick
import QtQuick.Controls
import QtTest
import RobinHoodInterface as RH

TestCase {
    name: "RobinHoodSessions"
    when: windowShown
    width: 1000
    height: 740
    visible: true

    function init() {
        RobinSessions.clear()
        RobinSessions.accounts.clear()
        RobinSessions.sessionActive = false
        RobinSessions.busy = false
        RobinSessions.reauthenticating = false
        RobinSessions.browserLoginPending = false
        RobinSessions.lastPreparedAccountId = ""
        RobinSessions.prepareCalls = 0
        RobinSessions.reauthenticateCalls = 0
        RobinSessions.beginBrowserCalls = 0
        RobinSessions.lastBrowserAccountId = ""
        RobinSessions.cancelCalls = 0
        RobinSessions.error = ""
        RobinSessions.lastStarted = -1
        RobinBrowser.available = true
        RobinBrowser.active = false
        RobinBrowser.backend = "test"
        RobinBrowser.viewSource = Qt.resolvedUrl("MockWebLogin.qml")
        RobinBrowser.error = ""
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign|Binding loop/)
    }
    function createView(width = 430, height = 700) {
        const component = Qt.createComponent("qrc:/qt/qml/RobinHoodInterface/CoreView.qml")
        compare(component.status, Component.Ready, component.errorString())
        const view = component.createObject(this, {width: width, height: height})
        verify(view !== null)
        return view
    }
    function addSaved(index, expired) {
        RobinSessions.append({sessionIndex: index, accountName: "Saved account " + index,
                              accountId: "stable-" + index, isExpired: expired,
                              expiresAt: expired ? "2000-01-01T00:00:00.000Z" : "2099-01-01T00:00:00.000Z"})
    }
    function sizes() {
        return [{tag: "compact", width: 360, height: 620},
                {tag: "desktop", width: 1000, height: 700}]
    }
    function noCredentials(item) {
        verify(!(item instanceof TextInput))
        for (const child of item.children)
            noCredentials(child)
    }
    function findText(item, text) {
        if (item.text === text)
            return item
        for (const child of item.children) {
            const match = findText(child, text)
            if (match)
                return match
        }
        return null
    }
    function browserGeometry(view) {
        const panel = findChild(view, "robinWebLoginPanel")
        const loader = findChild(view, "robinWebLoginLoader")
        const cancel = findChild(view, "cancelRobinBrowserLogin")
        const origin = findChild(view, "robinBrowserOrigin")
        verify(panel !== null && loader !== null && cancel !== null && origin !== null)
        tryCompare(loader, "status", Loader.Ready)
        tryVerify(() => loader.item !== null && loader.item.width >= view.width - 2
                    && loader.item.height > view.height * 0.6)
        compare(loader.item.objectName, "mockRobinWebLogin")
        compare(panel.width, view.width)
        compare(panel.height, view.height)
        compare(findChild(view, "robinSessionScroller").visible, false)
        const originEnd = origin.mapToItem(view, origin.width, origin.height)
        const cancelStart = cancel.mapToItem(view, 0, 0)
        const cancelEnd = cancel.mapToItem(view, cancel.width, cancel.height)
        const webStart = loader.item.mapToItem(view, 0, 0)
        const webEnd = loader.item.mapToItem(view, loader.item.width, loader.item.height)
        verify(originEnd.x <= cancelStart.x)
        verify(cancelStart.y >= 0 && cancelEnd.x <= view.width)
        verify(webStart.y >= Math.max(originEnd.y, cancelEnd.y))
        verify(webEnd.y <= view.height + 1)
        compare(origin.text, "https://robinhood.com/")
        noCredentials(view)
    }
    function test_emptyPickerAndDisabledAutoMode() {
        const view = createView()
        try {
            compare(findChild(view, "robinAutoMode").enabled, false)
            compare(findChild(view, "startRobinSession").enabled, false)
            compare(findChild(view, "addRobinAccount").enabled, true)
            noCredentials(view)
            compare(RobinSessions.beginBrowserCalls, 0)
        } finally { view.destroy() }
    }
    function test_addOpensOfficialBrowser_data() { return sizes() }
    function test_addOpensOfficialBrowser(data) {
        const view = createView(data.width, data.height)
        try {
            mouseClick(findChild(view, "addRobinAccount"))
            tryCompare(RobinSessions, "beginBrowserCalls", 1)
            compare(RobinSessions.lastBrowserAccountId, "")
            tryCompare(RobinBrowser, "active", true)
            browserGeometry(view)
            compare(findChild(view, "cancelRobinBrowserLogin").enabled, true)
            mouseClick(findChild(view, "cancelRobinBrowserLogin"))
            tryCompare(RobinSessions, "cancelCalls", 1)
            tryCompare(RobinBrowser, "active", false)
            tryCompare(findChild(view, "robinSessionScroller"), "visible", true)
            tryVerify(() => findChild(view, "robinWebLoginPanel") === null)
        } finally { view.destroy() }
    }
    function test_expiredReauthenticationPreservesSavedRow_data() { return sizes() }
    function test_expiredReauthenticationPreservesSavedRow(data) {
        addSaved(7, true)
        const view = createView(data.width, data.height)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            mouseClick(findChild(view, "reauthenticateRobinSession"))
            tryCompare(RobinSessions, "prepareCalls", 1)
            compare(RobinSessions.lastPreparedAccountId, "stable-7")
            compare(RobinSessions.lastBrowserAccountId, "stable-7")
            compare(RobinSessions.reauthenticateCalls, 0)
            compare(RobinSessions.count, 1)
            browserGeometry(view)
            mouseClick(findChild(view, "cancelRobinBrowserLogin"))
            tryCompare(RobinSessions, "cancelCalls", 1)
            compare(RobinSessions.count, 1)
            compare(RobinSessions.get(0).accountId, "stable-7")
            compare(view.view, 0)
        } finally { view.destroy() }
    }
    function test_verificationCancellableButCommitIsNot() {
        const view = createView()
        try {
            RobinSessions.beginBrowserLogin()
            tryVerify(() => findChild(view, "cancelRobinBrowserLogin") !== null)
            const cancel = findChild(view, "cancelRobinBrowserLogin")
            RobinBrowser.active = false
            tryCompare(cancel, "enabled", true)
            compare(findChild(view, "robinSessionScroller").visible, false)
            RobinSessions.browserLoginPending = false
            tryCompare(cancel, "enabled", false)
            mouseClick(cancel)
            compare(RobinSessions.cancelCalls, 0)
            compare(findChild(view, "robinSessionScroller").visible, false)
            RobinSessions.sessionActive = true
            tryCompare(view, "view", 2)
            tryCompare(findChild(view, "robinSessionScroller"), "visible", true)
            tryVerify(() => findChild(view, "robinWebLoginPanel") === null)
            compare(findChild(view, "switchRobinAccount").visible, true)
        } finally { view.destroy() }
    }
    function test_sessionActiveTakesPrecedenceOverBrowser() {
        const view = createView()
        try {
            RobinSessions.beginBrowserLogin()
            tryVerify(() => findChild(view, "robinWebLoginPanel") !== null)
            RobinSessions.sessionActive = true
            tryCompare(view, "view", 2)
            tryVerify(() => findChild(view, "robinWebLoginPanel") === null)
            compare(findChild(view, "switchRobinAccount").visible, true)
        } finally { view.destroy() }
    }
    function test_unavailableHasNoHeadlessFallback() {
        RobinBrowser.available = false
        RobinBrowser.backend = "unavailable"
        RobinBrowser.viewSource = "qrc:/qt/qml/RobinHoodInterface/UnavailableWebLogin.qml"
        addSaved(1, true)
        const view = createView()
        try {
            const add = findChild(view, "addRobinAccount")
            compare(add.enabled, false)
            const label = findChild(view, "robinBrowserUnavailable")
            verify(label !== null)
            compare(label.visible, true)
            verify(label.text.indexOf("QtWebEngineQuick") >= 0 && label.text.indexOf("QtWebView") >= 0)
            verify(label.text.indexOf("pre-navigation") >= 0)
            mouseClick(add)
            compare(RobinSessions.beginBrowserCalls, 0)
            compare(RobinBrowser.active, false)
            noCredentials(view)
            tryVerify(() => findChild(view, "robinSessionCard_1") !== null)
            mouseClick(findChild(view, "robinSessionCard_1"))
            tryCompare(findChild(view, "reauthenticateRobinSession"), "enabled", false)
        } finally { view.destroy() }
    }
    function test_browserErrorAndProgressAreAboveNativeView_data() { return sizes() }
    function test_browserErrorAndProgressAreAboveNativeView(data) {
        const view = createView(data.width, data.height)
        try {
            RobinSessions.beginBrowserLogin()
            RobinBrowser.error = "The page could not be loaded. Check your connection and try again."
            RobinSessions.error = "Session verification failed. No session token was saved."
            tryVerify(() => findChild(view, "robinBrowserError") !== null)
            const error = findChild(view, "robinBrowserError")
            const progress = findChild(view, "robinBrowserProgress")
            const loader = findChild(view, "robinWebLoginLoader")
            tryCompare(loader, "status", Loader.Ready)
            tryVerify(() => error.height >= error.implicitHeight && loader.item.height > 0)
            verify(error.text.indexOf(RobinBrowser.error) >= 0)
            verify(error.text.indexOf(RobinSessions.error) >= 0)
            verify(error.mapToItem(view, 0, error.height).y <= loader.mapToItem(view, 0, 0).y)
            verify(progress.mapToItem(view, 0, progress.height).y <= loader.mapToItem(view, 0, 0).y)
            compare(findChild(view, "cancelRobinBrowserLogin").enabled, true)
        } finally { view.destroy() }
    }
    function test_failedBrowserVerificationPreservesSavedAccount() {
        addSaved(7, true)
        const view = createView()
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            mouseClick(findChild(view, "reauthenticateRobinSession"))
            tryCompare(view, "view", 3)
            RobinSessions.error = "Session verification failed. No token was saved."
            RobinBrowser.active = false
            RobinSessions.browserLoginPending = false
            RobinSessions.reauthenticating = false
            RobinSessions.busy = false
            tryCompare(view, "view", 0)
            compare(RobinSessions.count, 1)
            compare(RobinSessions.get(0).accountId, "stable-7")
            compare(findText(view, RobinSessions.error).visible, true)
            compare(findChild(view, "reauthenticateRobinSession").enabled, true)
            noCredentials(view)
        } finally { view.destroy() }
    }
    function test_longAuthenticationDiagnosticFitsPanel_data() { return sizes() }
    function test_longAuthenticationDiagnosticFitsPanel(data) {
        RobinSessions.error = "Sign-in failed. Check your account access and try again. "
                + "The session verification request was rejected; no token has been saved. "
                + "If this continues, confirm your account access with Robinhood before retrying. "
                + "(HTTP 403; OAuth invalid_grant)"
        const view = createView(data.width, data.height)
        try {
            const label = findText(view, RobinSessions.error)
            verify(label !== null)
            const panel = label.parent
            const nextContent = findChild(view, "robinSessionList").parent
            tryVerify(() => label.width > 0 && label.implicitHeight > 28)
            compare(label.visible, true)
            tryVerify(() => label.height >= label.implicitHeight)
            verify(label.mapToItem(panel, 0, 0).y >= 12)
            verify(label.mapToItem(panel, label.width, label.implicitHeight).y <= panel.height - 12)
            verify(label.mapToItem(view, 0, 0).x >= 0)
            verify(label.mapToItem(view, label.width, label.implicitHeight).x <= view.width)
            tryVerify(() => nextContent.mapToItem(view, 0, 0).y
                      >= panel.mapToItem(view, 0, panel.height).y + 20)
        } finally { view.destroy() }
    }
    function test_activeAndExpiredSelection() {
        addSaved(0, false)
        addSaved(1, true)
        const view = createView()
        try {
            tryVerify(() => findChild(view, "robinSessionCard_1") !== null)
            const active = findChild(view, "robinSessionCard_0")
            const expired = findChild(view, "robinSessionCard_1")
            verify(active.color !== expired.color)
            mouseClick(active)
            const start = findChild(view, "startRobinSession")
            tryCompare(start, "enabled", true)
            mouseClick(start)
            tryCompare(RobinSessions, "lastStarted", 0)
            mouseClick(expired)
            tryCompare(start, "visible", false)
            compare(findChild(view, "reauthenticateRobinSession").visible, true)
        } finally { view.destroy() }
    }
    function test_deleteSavedAccountRequiresConfirmation() {
        addSaved(0, false)
        addSaved(1, true)
        const view = createView(360, 620)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_1") !== null)
            const remove = findChild(view, "deleteRobinSession")
            compare(remove.enabled, false)
            mouseClick(findChild(view, "robinSessionCard_1"))
            tryCompare(remove, "enabled", true)
            tryVerify(() => findChild(view, "deleteRobinSessionDialog") !== null)
            const confirmation = findChild(view, "deleteRobinSessionDialog")
            mouseClick(remove)
            tryCompare(confirmation, "visible", true)
            compare(RobinSessions.count, 2)
            mouseClick(confirmation.standardButton(Dialog.Cancel))
            tryCompare(confirmation, "visible", false)
            compare(RobinSessions.count, 2)
            mouseClick(remove)
            tryCompare(confirmation, "visible", true)
            mouseClick(confirmation.standardButton(Dialog.Yes))
            tryCompare(RobinSessions, "count", 1)
            tryCompare(view, "selectedIndex", -1)
            compare(view.selectedAccountId, "")
            compare(RobinSessions.get(0).accountId, "stable-0")
        } finally { view.destroy() }
    }
    function test_accountOverviewShowsNamesBalancesAndRefreshes() {
        RobinSessions.accounts.append({accountNumber: "100", displayName: "Individual Investing", currentBalance: "$10,245.50"})
        RobinSessions.accounts.append({accountNumber: "200", displayName: "Roth IRA", currentBalance: "$501.23"})
        RobinSessions.accounts.append({accountNumber: "300", displayName: "Traditional IRA", currentBalance: "Unavailable"})
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            compare(findChild(view, "robinAccountName_100").text, "Individual Investing")
            compare(findChild(view, "robinAccountBalance_200").text, "Balance: $501.23")
            compare(findChild(view, "robinAccountTile_300").currentBalance, "Unavailable")
            RobinSessions.accounts.clear()
            tryVerify(() => findChild(view, "robinAccountTile_100") === null)
            RobinSessions.accounts.append({accountNumber: "400", displayName: "Cash Account", currentBalance: "$0.00"})
            tryVerify(() => findChild(view, "robinAccountTile_400") !== null)
        } finally { view.destroy() }
    }
    function test_pickerFitsMobileAndDesktop_data() { return sizes() }
    function test_pickerFitsMobileAndDesktop(data) {
        addSaved(3, false)
        const view = createView(data.width, data.height)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_3") !== null)
            const card = findChild(view, "robinSessionCard_3")
            const start = findChild(view, "startRobinSession")
            verify(card.mapToItem(view, 0, 0).x >= 0)
            verify(card.mapToItem(view, card.width, 0).x <= data.width)
            verify(start.mapToItem(view, start.width, 0).x <= data.width)
            verify(start.width >= 300)
        } finally { view.destroy() }
    }
}
