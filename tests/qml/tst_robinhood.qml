import QtQuick
import QtQuick.Controls
import QtTest
import QtQuick.Window
import RobinHoodInterface as RH

TestCase {
    name: "RobinHoodSessions"
    when: windowShown
    width: 1000
    height: 740
    visible: true

    function init() {
        RobinTrades.reset()
        RobinSessions.clear()
        RobinSessions.mockCachedAccounts = ({})
        RobinSessions.accounts.clear()
        RobinSessions.sessionActive = false
        RobinSessions.activeAccount = ""
        RobinSessions.busy = false
        RobinSessions.reauthenticating = false
        RobinSessions.nativeLoginPending = false
        RobinSessions.nativeRequestStarted = false
        RobinSessions.nativeCodeRequired = false
        RobinSessions.nativeLoginCancellable = true
        RobinSessions.nativeLoginStatus = ""
        RobinSessions.nativeLoginEmail = ""
        RobinSessions.beginNativeCalls = 0
        RobinSessions.signInCalls = 0
        RobinSessions.codeCalls = 0
        RobinSessions.acceptSignIn = true
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
    function addSaved(index, expired, canRefresh = false) {
        RobinSessions.append({sessionIndex: index, accountName: "Saved account " + index,
                              accountId: "stable-" + index, isExpired: expired, canRefresh: canRefresh,
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
    function account(number, name, holdings = [], status = "fresh", updated = "2026-10-26T12:00:00Z", error = "") {
        return {accountNumber: number, displayName: name, currentBalance: "$501.23",
                holdings: holdings, holdingsStatus: status, holdingsUpdatedAt: updated, holdingsError: error}
    }
    Component { id: iconProbe; Image { width: 18; height: 18; sourceSize: Qt.size(18, 18) } }
    function test_accountInformationButton() {
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            const info = findChild(view, "robinAccountInfo_100")
            verify(info !== null)
            compare(info.Accessible.name, "Account information for Standard Brokerage 100")
            verify(info.width >= 40 && info.height >= 40)
            verify(info.icon.source.toString().endsWith("account-info.svg"))
            const image = createTemporaryObject(iconProbe, this, {source: info.icon.source})
            verify(image !== null)
            tryCompare(image, "status", Image.Ready)
            info.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            tryCompare(dialog, "visible", true)
            keyClick(Qt.Key_Escape)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
        } finally { view.destroy() }
    }
    function test_selectedAccountExactFractionalHoldings() {
        RobinSessions.accounts.append(account("100", "Standard Brokerage", [{symbol: "AAPL", quantity: "0.000000123456789"}]))
        RobinSessions.accounts.append(account("200", "Roth IRA", [{symbol: "MSFT", quantity: "12.340000000001"}, {symbol: "VOO", quantity: "3.5"}]))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            mouseClick(findChild(view, "robinAccountInfo_200"))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            const heading = findChild(dialog, "robinInfoIdentity")
            verify(heading !== null)
            compare(heading.text, "Roth IRA · 200")
            const holdings = findChild(dialog, "robinHoldingsList")
            verify(holdings !== null)
            tryCompare(holdings, "count", 2)
            tryVerify(() => holdings.itemAtIndex(1) !== null)
            compare(findChild(holdings.itemAtIndex(0), "robinHoldingQuantity_0").text, "12.340000000001")
            compare(findChild(holdings.itemAtIndex(0), "robinHoldingSymbol_0").text, "MSFT")
            compare(findChild(holdings.itemAtIndex(1), "robinHoldingQuantity_1").text, "3.5")
            compare(findChild(dialog, "robinInfoSnapshot").text, "Stored snapshot · Not live data")
            compare(RobinSessions.lastStarted, -1)
            compare(RobinSessions.beginBrowserCalls, 0)
        } finally { view.destroy() }
    }
    function reveal(view, item) {
        verify(waitForPolish(view.Window.window))
        const scroller = findChild(view, "robinSessionScroller")
        scroller.contentY = Math.max(0, Math.min(scroller.contentHeight - scroller.height,
                                item.mapToItem(scroller.contentItem, 0, 0).y - 20))
        verify(waitForPolish(view.Window.window))
        tryVerify(() => item.mapToItem(view, 0, 0).y >= 0
                    && item.mapToItem(view, 0, item.height).y <= view.height)
    }
    function test_brokerageAccountsOnlyOnOverview_data() {
        const rows = []
        for (const size of sizes())
            for (const state of ["active", "expired", "uncached"])
                rows.push({tag: size.tag + "-" + state, width: size.width, height: size.height, state: state})
        return rows
    }
    function test_brokerageAccountsOnlyOnOverview(data) {
        addSaved(7, data.state === "expired")
        const stored = account("700", "IRA", [{symbol: "F", quantity: "0.125000001"}], "cached")
        RobinSessions.mockCachedAccounts = ({7: data.state === "uncached" ? [] : [stored]})
        const view = createView(data.width, data.height)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            compare(view.view, 0)
            compare(findText(view, "Cached brokerage accounts"), null)
            compare(findChild(view, "robinAccountTile_700"), null)
            compare(findChild(view, "robinAccountInfo_700"), null)
            compare(findChild(view, "robinCacheUnavailable"), null)
            compare(findChild(view, "robinAccountInfoDialog"), null)
            compare(RobinSessions.lastStarted, -1)
            compare(RobinSessions.beginNativeCalls, 0)
            RobinSessions.accounts.append(stored)
            RobinSessions.sessionActive = true
            tryCompare(view, "view", 2)
            const tile = findChild(view, "robinAccountTile_700")
            verify(tile !== null)
            tryCompare(tile, "visible", true)
            const info = findChild(view, "robinAccountInfo_700")
            verify(info !== null)
            reveal(view, info)
            mouseClick(info)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            tryCompare(dialog, "visible", true)
            compare(findChild(dialog, "robinInfoIdentity").text, "IRA · 700")
            keyClick(Qt.Key_Escape)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
            const back = findChild(view, "switchRobinAccount")
            verify(back !== null)
            reveal(view, back)
            mouseClick(back)
            tryCompare(view, "view", 0)
            compare(findChild(view, "robinAccountTile_700"), null)
            compare(findText(view, "Cached brokerage accounts"), null)
            compare(RobinSessions.cachedAccounts(7).length, data.state === "uncached" ? 0 : 1)
        } finally { view.destroy() }
    }
    function test_switchAccountCancelsHoldingsRefresh_data() { return sizes() }
    function test_switchAccountCancelsHoldingsRefresh(data) {
        addSaved(7, false)
        RobinSessions.mockCachedAccounts = ({7: [account("100", "IRA", [{symbol: "F", quantity: "0.125"}], "cached")]})
        RobinSessions.accounts.append(account("100", "IRA"))
        RobinSessions.sessionActive = true
        RobinSessions.busy = true
        const view = createView(data.width, data.height)
        view.selectedIndex = 7
        try {
            const switchAccount = findChild(view, "switchRobinAccount")
            verify(switchAccount.visible)
            compare(switchAccount.enabled, true)
            reveal(view, switchAccount)
            mouseClick(switchAccount)
            tryCompare(RobinSessions, "sessionActive", false)
            tryCompare(RobinSessions, "busy", false)
            compare(RobinSessions.accounts.count, 0)
            compare(RobinSessions.cachedAccounts(7)[0].holdings[0].quantity, "0.125")
            compare(view.view, 0)
            compare(RobinSessions.beginBrowserCalls, 0)
        } finally { view.destroy() }
    }
    function test_accountInformationRendersLiteralMarkup() {
        RobinSessions.accounts.append(account("100", "IRA", [{symbol: "<b>XYZ</b>", quantity: "1.2500"}]))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            mouseClick(findChild(view, "robinAccountInfo_100"))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            const list = findChild(dialog, "robinHoldingsList")
            tryVerify(() => list.itemAtIndex(0) !== null)
            const symbol = findChild(list.itemAtIndex(0), "robinHoldingSymbol_0")
            compare(symbol.text, "<b>XYZ</b>")
            compare(symbol.textFormat, Text.PlainText)
            compare(findChild(list.itemAtIndex(0), "robinHoldingQuantity_0").textFormat, Text.PlainText)
            compare(findChild(dialog, "robinInfoIdentity").textFormat, Text.PlainText)
            compare(findChild(dialog, "robinInfoError").textFormat, Text.PlainText)
        } finally { view.destroy() }
    }

    function test_snapshotStates_data() {
        return [
            {tag: "fresh-empty", status: "fresh", updated: "2026-10-26T12:00:00Z", error: "", state: "Latest successful sync", empty: "No stock holdings in this stored snapshot"},
            {tag: "old-cache-empty", status: "cached", updated: "2001-01-01T00:00:00Z", error: "", state: "Cached snapshot · May be out of date", empty: "No stock holdings in this stored snapshot"},
            {tag: "never", status: "unavailable", updated: "", error: "", state: "Holdings unavailable", empty: "Account information available after first successful sync"},
            {tag: "failed-first-sync", status: "unavailable", updated: "", error: "Offline: request failed", state: "Refresh failed · No stored holdings snapshot", empty: "Account information available after first successful sync"},
            {tag: "failed-refresh", status: "stale", updated: "2001-01-01T00:00:00Z", error: "Offline: request failed", state: "Refresh failed · Showing stored snapshot", empty: "No stock holdings in this stored snapshot"}
        ]
    }
    function test_snapshotStates(data) {
        RobinSessions.accounts.append(account("100", "Standard Brokerage", [], data.status, data.updated, data.error))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            mouseClick(findChild(view, "robinAccountInfo_100"))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            const status = findChild(dialog, "robinInfoStatus")
            verify(status !== null)
            compare(status.text, data.state)
            compare(findChild(dialog, "robinInfoEmpty").text, data.empty)
            compare(findChild(dialog, "robinInfoError").visible, data.error !== "")
            compare(findChild(dialog, "robinInfoError").text, data.error)
            const updated = findChild(dialog, "robinInfoUpdated")
            compare(updated.Accessible.description, data.updated)
            if (data.updated)
                verify(/^Last updated: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/.test(updated.text))
            else
                compare(updated.text, "Never downloaded")
            compare(findChild(dialog, "robinHoldingsList").count, 0)
        } finally { view.destroy() }
    }
    function test_noCacheDoesNotDisplayAccountPlaceholder() {
        addSaved(7, true)
        const view = createView()
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            compare(findChild(view, "robinCacheUnavailable"), null)
            compare(findText(view, "Cached brokerage accounts"), null)
            compare(RobinSessions.lastStarted, -1)
        } finally { view.destroy() }
    }
    function test_profileCacheChangesResetPrivateDialog_data() {
        return [{tag: "updated", action: "update"}, {tag: "deleted", action: "delete"},
                {tag: "other-profile-deleted", action: "deleteOther"}, {tag: "selected", action: "select"}]
    }
    function test_profileCacheChangesResetPrivateDialog(data) {
        addSaved(0, true)
        addSaved(1, true)
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.sessionActive = true
        const view = createView()
        view.selectedIndex = 0
        try {
            tryVerify(() => findChild(view, "robinAccountInfo_100") !== null)
            const info = findChild(view, "robinAccountInfo_100")
            reveal(view, info)
            mouseClick(info)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            if (data.action === "update") {
                RobinSessions.accounts.clear()
                RobinSessions.accounts.append(account("300", "Roth IRA"))
                RobinSessions.accountCacheChanged()
            } else if (data.action === "delete") {
                RobinSessions.deleteSavedSession(0)
            } else if (data.action === "deleteOther") {
                RobinSessions.deleteSavedSession(1)
            } else {
                view.selectedIndex = 1
            }
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
            compare(view.informationAccount, null)
            if (data.action === "update") {
                tryVerify(() => findChild(view, "robinAccountInfo_300") !== null)
                compare(findChild(view, "robinAccountInfo_100"), null)
            } else {
                compare(findChild(view, "robinAccountInfo_200"), null)
                compare(view.selectedIndex, data.action === "delete" ? -1 : data.action === "select" ? 1 : 0)
            }
            compare(RobinSessions.lastStarted, -1)
        } finally { view.destroy() }
    }
    function test_activeSwitchClosesInformation() {
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            mouseClick(findChild(view, "robinAccountInfo_100"))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            RobinSessions.clearActiveSession()
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
            compare(view.informationAccount, null)
        } finally { view.destroy() }
    }
    function test_informationViewportAndLargeHoldingsKeyboard_data() { return sizes() }
    function test_informationViewportAndLargeHoldingsKeyboard(data) {
        const rows = []
        for (let i = 0; i < 1000; ++i)
            rows.push({symbol: i === 0 ? "EXTREMELY-LONG-SYNTHETIC-STOCK-SYMBOL-ABCDEFGHIJKLMNOPQRSTUVWXYZ" : "SYN" + i,
                       quantity: i === 0 ? "123456789012345678901234567890.123456789012345678901234567890123456789" : "0.000000001"})
        RobinSessions.accounts.append(account("12345678901234567890", "Standard Brokerage", rows))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            const info = findChild(view, "robinAccountInfo_12345678901234567890")
            tryVerify(() => info.width >= 40 && info.height >= 40)
            reveal(view, info)
            info.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            const list = findChild(dialog, "robinHoldingsList")
            tryVerify(() => list.itemAtIndex(0) !== null)
            const start = dialog.contentItem.mapToItem(view, 0, 0)
            const end = dialog.contentItem.mapToItem(view, dialog.contentItem.width, dialog.contentItem.height)
            verify(start.x >= 0 && start.y >= 0 && end.x <= view.width && end.y <= view.height)
            verify(list.height > 100 && list.width > 200)
            const quantity = findChild(list.itemAtIndex(0), "robinHoldingQuantity_0")
            compare(quantity.text, rows[0].quantity)
            verify(quantity.Accessible.name.indexOf(rows[0].quantity) >= 0)
            tryVerify(() => quantity.height >= quantity.implicitHeight)
            verify(quantity.mapToItem(list.itemAtIndex(0), quantity.width, quantity.height).x <= list.width)
            compare(dialog.header.color, "#f3f9f0")
            compare(dialog.footer.background.color, "#142522")
            verify(dialog.standardButton(Dialog.Close).height >= 40)
            // Tab must reach the scrollable holdings, not trap focus on the modal.
            for (let i = 0; i < 8 && !list.activeFocus; ++i)
                keyClick(Qt.Key_Tab)
            compare(list.activeFocus, true)
            const focusFrame = findChild(list, "robinHoldingsFocus")
            verify(focusFrame !== null)
            compare(focusFrame.visible, true)
            compare(findChild(dialog, "robinInfoHoldingsHeading").text, "Stock holdings · Shares")
            keyClick(Qt.Key_End)
            tryVerify(() => list.atYEnd)
            tryVerify(() => list.itemAtIndex(999) !== null)
            compare(findChild(list.itemAtIndex(999), "robinHoldingQuantity_999").text, "0.000000001")
            keyClick(Qt.Key_Home)
            tryVerify(() => list.atYBeginning)
            mouseClick(dialog.standardButton(Dialog.Close))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
        } finally { view.destroy() }
    }
    function test_tradeEntryUppercasesAsciiAndPreservesSeparators_data() {
        return [{tag: "buy-compact", width: 360, height: 620, side: "buy"},
                {tag: "sell-desktop", width: 1000, height: 700, side: "sell"}]
    }
    function test_tradeEntryUppercasesAsciiAndPreservesSeparators(data) {
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.accounts.append(account("200", "Roth IRA"))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            const field = findChild(view, "robinTickerInput")
            verify(field !== null)
            const buy = findChild(view, "robinBuyButton")
            const sell = findChild(view, "robinSellButton")
            verify(buy !== null && sell !== null)
            compare(buy.enabled, false)
            compare(sell.enabled, false)
            reveal(view, field)
            field.forceActiveFocus()
            RobinTrades.typeText(field, " aapl, BRK.B, TSLA ")
            compare(field.text, " AAPL, BRK.B, TSLA ")
            tryCompare(buy, "enabled", true)
            tryCompare(sell, "enabled", true)
            const button = data.side === "buy" ? buy : sell
            reveal(view, button)
            verify(button.mapToItem(view, button.width, 0).x <= view.width)
            snapshot(view, "robin-before-click-" + data.tag)
            mouseClick(button)
            tryCompare(RobinTrades, "submitCalls", 1)
            compare(RobinTrades.lastInput, " AAPL, BRK.B, TSLA ")
            compare(RobinTrades.lastSide, data.side)
            compare(buy.enabled, false)
            compare(sell.enabled, false)
            compare(findChild(view, "switchRobinAccount").enabled, false)
        } finally { view.destroy() }
    }
    function test_tickerUppercasePreservesCaretAndInvalidCharacters_data() { return sizes() }
    function test_tickerUppercasePreservesCaretAndInvalidCharacters(data) {
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            const field = findChild(view, "robinTickerInput")
            reveal(view, field)
            field.forceActiveFocus()
            keyClick(Qt.Key_A)
            keyClick(Qt.Key_B)
            keyClick(Qt.Key_C)
            compare(field.text, "ABC")
            compare(field.cursorPosition, 3)
            field.cursorPosition = 1
            keyClick(Qt.Key_Z)
            compare(field.text, "AZBC")
            compare(field.cursorPosition, 2)
            field.select(1, 3)
            RobinTrades.typeText(field, "xy")
            compare(field.text, "AXYC")
            compare(field.cursorPosition, 3)
            field.selectAll()
            RobinTrades.typeText(field, "aapl, brk.b, x-y, ßé")
            compare(field.text, "AAPL, BRK.B, X-Y, ßé")
        } finally { view.destroy() }
    }
    function test_accountOverviewOmitsTickerDescriptions_data() { return sizes() }
    function test_accountOverviewOmitsTickerDescriptions(data) {
        RobinSessions.accounts.append(account("100", "Standard Brokerage"))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            compare(findChild(view, "robinTradeScope"), null)
            compare(findChild(view, "robinOrderPolicy"), null)
            compare(findChild(view, "robinTradeAccountCount"), null)
            const field = findChild(view, "robinTickerInput")
            verify(field !== null)
            const trade = field.parent
            for (const child of trade.children)
                if (child.text !== undefined) verify(child.text !== "Ticker symbols")
            const mode = findChild(view, "robinExecutionMode")
            verify(mode !== null && mode.visible)
            compare(field.Accessible.name, "Ticker symbols, comma-separated")
            tryVerify(() => field.y >= mode.y + mode.height
                      && field.y - mode.y - mode.height <= trade.spacing + 1)
            const buy = findChild(view, "robinBuyButton")
            tryVerify(() => buy.mapToItem(trade, 0, 0).y >= field.y + field.height
                      && buy.mapToItem(trade, 0, 0).y - field.y - field.height <= trade.spacing + 1)
            reveal(view, field)
            snapshot(view, "robin-clean-entry-" + data.tag)
        } finally { view.destroy() }
    }
    function result(status, message = "Brokerage result") {
        return {accountNumber: "<b>100</b>", accountLabel: "<b>IRA</b>",
                symbol: "<b>BRK.B</b>", quantity: "0.000000123456789", side: "sell",
                status: status, success: status === "filled", message: message,
                orderId: "<b>order-id</b>", refId: "<b>ref-id</b>"}
    }
    function test_resultsStopRemainingAndFinalCooldown_data() { return sizes() }
    function test_resultsStopRemainingAndFinalCooldown(data) {
        RobinSessions.accounts.append(account("100", "IRA"))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            RobinTrades.submit("AAPL", "buy")
            RobinTrades.tradeResults = [result("submitted")]
            tryVerify(() => findChild(view, "robinTradeResults") !== null)
            const done = findChild(view, "robinTradeDone")
            const stop = findChild(view, "robinStopRemaining")
            verify(done !== null && stop !== null)
            compare(findChild(view, "robinSessionScroller").visible, false)
            compare(done.enabled, false)
            compare(stop.enabled, true)
            keyClick(Qt.Key_Escape)
            compare(RobinTrades.dismissCalls, 0)
            compare(RobinTrades.cancelCalls, 0)
            // Last response arrived, but the controller has not finished the final cooldown.
            RobinTrades.tradeResultsComplete = true
            compare(done.enabled, false)
            mouseClick(done)
            tryCompare(RobinTrades, "dismissCalls", 0)
            compare(stop.mapToItem(view, 0, 0).y >= 0, true)
            verify(stop.mapToItem(view, stop.width, stop.height).y <= view.height)
            const warning = findChild(view, "robinStopExplanation")
            verify(warning !== null)
            verify(warning.text.indexOf("not cancel already submitted") >= 0)
            mouseClick(stop)
            tryCompare(RobinTrades, "cancelCalls", 1)
            tryCompare(done, "enabled", true)
            mouseClick(done)
            tryCompare(RobinTrades, "dismissCalls", 1)
            tryVerify(() => findChild(view, "robinTradeResults") === null)
            compare(RobinSessions.sessionActive, true)
            compare(findChild(view, "robinSessionScroller").visible, true)
        } finally { view.destroy() }
    }
    function test_accountCountUpdatesAndCompletedResultsKeyboard_data() { return sizes() }
    function test_accountCountUpdatesAndCompletedResultsKeyboard(data) {
        RobinSessions.accounts.append(account("100", "IRA"))
        RobinSessions.sessionActive = true
        const view = createView(data.width, data.height)
        try {
            verify(findChild(view, "robinAccountTile_100") !== null)
            compare(findChild(view, "robinAccountTile_200"), null)
            RobinSessions.accounts.append(account("200", "Roth IRA"))
            tryVerify(() => findChild(view, "robinAccountTile_200") !== null)
            RobinTrades.tradeResultsVisible = true
            const scroller = findChild(view, "robinResultsScroller")
            verify(scroller !== null)
            tryCompare(scroller, "activeFocus", true)
            keyClick(Qt.Key_Tab)
            const done = findChild(view, "robinTradeDone")
            verify(done !== null)
            tryCompare(done, "activeFocus", true)
            keyClick(Qt.Key_Escape)
            tryCompare(RobinTrades, "dismissCalls", 1)
            tryVerify(() => findChild(view, "robinTradeResults") === null)
        } finally { view.destroy() }
    }
    function test_resultStatusesAndLiteralValues_data() {
        return [{tag: "pending", status: "pending", label: "Pending"},
                {tag: "submitted", status: "submitted", label: "Submitted · Accepted/queued, not filled"},
                {tag: "filled", status: "filled", label: "Filled"},
                {tag: "failed", status: "failed", label: "Failed"},
                {tag: "unknown", status: "unknown", label: "Unknown · Check Robinhood before retrying"},
                {tag: "cancelled", status: "cancelled", label: "Cancelled · Not submitted"},
                {tag: "skipped", status: "skipped", label: "Skipped · No sellable shares"},
                {tag: "preview", status: "preview", label: "Preview · No order sent"}]
    }
    function test_resultStatusesAndLiteralValues(data) {
        RobinTrades.currentTransactionTitle = "<b>Batch</b>"
        RobinTrades.tradeResults = [result(data.status, "<b>Not enough buying power</b>")]
        RobinTrades.tradeResultsVisible = true
        const view = createView(360, 620)
        try {
            const status = findChild(view, "robinResultStatus_0")
            verify(status !== null)
            compare(status.text, data.label)
            const expected = {Account: "<b>IRA</b> · <b>100</b>",
                              Symbol: "<b>BRK.B</b>", Quantity: "0.000000123456789",
                              Message: "<b>Not enough buying power</b>",
                              OrderId: "Order ID: <b>order-id</b>", RefId: "Reference ID: <b>ref-id</b>"}
            for (const key in expected) {
                const label = findChild(view, "robinResult" + key + "_0")
                verify(label !== null)
                compare(label.text, expected[key])
                compare(label.textFormat, Text.PlainText)
                compare(label.elide, Text.ElideNone)
            }
            compare(findChild(view, "robinTransactionTitle").textFormat, Text.PlainText)
            compare(findChild(view, "robinTradeDone").enabled, true)
        } finally { view.destroy() }
    }
    function test_resultsKeepControllerOrderAndUpdateRows() {
        const rows = []
        for (const symbol of ["AAPL", "TSLA"])
            for (const number of ["100", "200"])
                rows.push(Object.assign(result("pending"), {symbol: symbol, accountNumber: number, accountLabel: "IRA"}))
        RobinTrades.tradeResults = rows
        RobinTrades.tradeResultsVisible = true
        const view = createView()
        try {
            for (let i = 0; i < rows.length; ++i) {
                const symbol = findChild(view, "robinResultSymbol_" + i)
                const identity = findChild(view, "robinResultAccount_" + i)
                verify(symbol !== null && identity !== null)
                compare(symbol.text, rows[i].symbol)
                compare(identity.text, "IRA · " + rows[i].accountNumber)
            }
            rows[0] = Object.assign({}, rows[0], {status: "submitted"})
            RobinTrades.tradeResults = rows.slice()
            tryVerify(() => findChild(view, "robinResultStatus_0").text === "Submitted · Accepted/queued, not filled")
            compare(findChild(view, "robinResultStatus_1").text, "Pending")
            compare(findChild(view, "robinResultSymbol_2").text, "TSLA")
            compare(findChild(view, "robinResultAccount_2").text, "IRA · 100")
        } finally { view.destroy() }
    }
    function cleanup() {
        RobinTrades.reset()
    }
    function snapshot(view, name) {
        if (qmlTestArtifactDirectory.length > 0) {
            waitForRendering(view)
            grabImage(view).save(qmlTestArtifactDirectory + "/" + name + ".png")
        }
    }
    function test_readOnlyAuthKeepsInformationAndDisablesOrders_data() { return sizes() }
    function test_readOnlyAuthKeepsInformationAndDisablesOrders(data) {
        RobinSessions.accounts.append(account("100", "IRA"))
        RobinSessions.sessionActive = true
        RobinTrades.available = false
        RobinTrades.unavailableReason = "Reauthenticate this login to enable orders. Stored session is read-only. <b>Sign in again</b>"
        const view = createView(data.width, data.height)
        try {
            const input = findChild(view, "robinTickerInput")
            verify(input !== null)
            input.text = "AAPL"
            const buy = findChild(view, "robinBuyButton")
            const sell = findChild(view, "robinSellButton")
            verify(buy !== null && sell !== null)
            compare(buy.enabled, false)
            compare(sell.enabled, false)
            const reason = findChild(view, "robinTradesUnavailable")
            verify(reason !== null)
            compare(reason.visible, true)
            compare(reason.text, RobinTrades.unavailableReason)
            compare(reason.textFormat, Text.PlainText)
            reveal(view, buy)
            mouseClick(buy)
            tryCompare(RobinTrades, "submitCalls", 0)
            reveal(view, findChild(view, "robinAccountInfo_100"))
            mouseClick(findChild(view, "robinAccountInfo_100"))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") !== null)
            const dialog = findChild(view, "robinAccountInfoDialog")
            mouseClick(dialog.standardButton(Dialog.Close))
            tryVerify(() => findChild(view, "robinAccountInfoDialog") === null)
            compare(findChild(view, "switchRobinAccount").enabled, true)
            RobinTrades.available = true
            input.text = "   "
            compare(buy.enabled, false)
            input.text = "123,?"
            compare(buy.enabled, true)
            input.text = "AAPL"
            compare(sell.enabled, true)
            compare(findChild(view, "robinExecutionMode").text, "PREVIEW ONLY · Debug build · No orders sent")
            RobinTrades.liveOrdersEnabled = true
            compare(findChild(view, "robinExecutionMode").text, "LIVE ORDERS · Release build")
            reveal(view, buy)
            snapshot(view, "robin-entry-" + data.tag)
            compare(RobinTrades.submitCalls, 0)
        } finally { view.destroy() }
    }
    function test_longResultReasonRemainsScrollable_data() { return sizes() }
    function test_longResultReasonRemainsScrollable(data) {
        const message = "<b>Order rejected</b>: " + "Detailed rejection reason. ".repeat(180) + "END OF REASON"
        const row = result("failed", message)
        row.symbol = "LONGSYMBOL".repeat(15)
        row.quantity = "0." + "123456789".repeat(12)
        RobinTrades.tradeResults = [row, result("unknown"), result("preview"), result("filled"), result("submitted")]
        RobinTrades.tradeResultsVisible = true
        const view = createView(data.width, data.height)
        try {
            const scroller = findChild(view, "robinResultsScroller")
            const reason = findChild(view, "robinResultMessage_0")
            const symbol = findChild(view, "robinResultSymbol_0")
            verify(scroller !== null && reason !== null && symbol !== null)
            tryVerify(() => reason.height >= reason.implicitHeight && reason.height > 250)
            verify(scroller.contentHeight > scroller.height)
            compare(reason.text, message)
            compare(reason.elide, Text.ElideNone)
            compare(reason.maximumLineCount, 2147483647)
            verify(symbol.mapToItem(view, symbol.width, 0).x <= view.width)
            const done = findChild(view, "robinTradeDone")
            verify(done !== null)
            verify(done.mapToItem(view, done.width, done.height).y <= view.height)
            verify(scroller.mapToItem(view, 0, scroller.height).y <= done.mapToItem(view, 0, 0).y)
            snapshot(view, "robin-results-" + data.tag)
            scroller.forceActiveFocus()
            keyClick(Qt.Key_End)
            tryVerify(() => scroller.atYEnd)
            snapshot(view, "robin-results-end-" + data.tag)
            const last = findChild(view, "robinResultStatus_4")
            verify(last !== null)
            verify(last.mapToItem(view, 0, last.height).y <= done.mapToItem(view, 0, 0).y)
            keyClick(Qt.Key_Home)
            tryVerify(() => scroller.atYBeginning)
            compare(reason.text, message)
        } finally { view.destroy() }
    }
    function test_externalSessionLabelsStayLiteral_data() {
        return [{tag: "active", active: true}, {tag: "saved", active: false}]
    }
    function test_externalSessionLabelsStayLiteral(data) {
        const name = "<b>Account</b>"
        RobinSessions.error = "<b>Session error</b>"
        RobinSessions.activeAccount = name
        if (data.active) {
            RobinSessions.accounts.append(account("100", "IRA"))
            RobinSessions.sessionActive = true
        } else {
            RobinSessions.append({sessionIndex: 0, accountName: name, accountId: "stable-0", isExpired: false,
                                  expiresAt: "2099-01-01T00:00:00.000Z"})
        }
        const view = createView()
        try {
            const error = findText(view, RobinSessions.error)
            verify(error !== null)
            compare(error.textFormat, Text.PlainText)
            tryVerify(() => findText(view, data.active ? "Signed in as " + name : name) !== null)
            const identity = findText(view, data.active ? "Signed in as " + name : name)
            compare(identity.textFormat, Text.PlainText)
            if (!data.active) {
                mouseClick(findChild(view, "robinSessionCard_0"))
                const remove = findChild(view, "deleteRobinSession")
                verify(remove !== null)
                tryCompare(remove, "enabled", true)
                reveal(view, remove)
                mouseClick(remove)
                const dialog = findChild(view, "deleteRobinSessionDialog")
                verify(dialog !== null)
                tryCompare(dialog, "visible", true)
                compare(dialog.contentItem.textFormat, Text.PlainText)
                verify(dialog.contentItem.text.indexOf(name) >= 0)
                keyClick(Qt.Key_Escape)
                tryCompare(dialog, "visible", false)
            }
        } finally { view.destroy() }
    }
    function test_reauthenticateExpiryWindow_data() {
        const rows = []
        const hour = 60 * 60 * 1000
        for (const size of sizes())
            for (const scenario of [
                {name: "unselected", offset: hour, selected: false, eligible: false},
                {name: "later", offset: 3 * hour, selected: true, eligible: false},
                {name: "outside", offset: 2 * hour + 1, selected: true, eligible: false},
                {name: "boundary", offset: 2 * hour, selected: true, eligible: true},
                {name: "soon", offset: hour, selected: true, eligible: true},
                {name: "expired", offset: -hour, selected: true, eligible: true},
                {name: "expired-refreshable", offset: -hour, selected: true, eligible: true, refreshable: true}
            ]) rows.push(Object.assign({}, size, scenario, {tag: size.tag + "-" + scenario.name}))
        return rows
    }
    function test_reauthenticateExpiryWindow(data) {
        const now = Date.now()
        RobinSessions.append({sessionIndex: 7, accountName: "Saved account 7", accountId: "stable-7",
                              isExpired: data.offset <= 0, canRefresh: Boolean(data.refreshable),
                              expiresAt: new Date(now + data.offset).toISOString()})
        const view = createView(data.width, data.height)
        try {
            const reauth = findChild(view, "reauthenticateRobinSession")
            verify(reauth !== null)
            compare(reauth.visible, false)
            if (data.selected) {
                tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
                mouseClick(findChild(view, "robinSessionCard_7"))
                tryCompare(view, "selectedIndex", 7)
            }
            if (view.sessionTimeMs !== undefined) view.sessionTimeMs = now
            tryCompare(reauth, "visible", data.eligible)
            compare(RobinSessions.reauthenticateCalls, 0)
            if (data.eligible) {
                RobinSessions.busy = true
                tryCompare(reauth, "enabled", false)
                RobinSessions.busy = false
                reveal(view, reauth)
                mouseClick(reauth)
                tryCompare(RobinSessions, "reauthenticateCalls", 1)
                compare(RobinSessions.lastNativeAccountId, "stable-7")
            }
        } finally { view.destroy() }
    }
    function test_reauthenticateTracksClockAndRenewedExpiry_data() { return sizes() }
    function test_reauthenticateTracksClockAndRenewedExpiry(data) {
        const now = Date.now()
        const hour = 60 * 60 * 1000
        RobinSessions.append({sessionIndex: 7, accountName: "Saved account 7", accountId: "stable-7",
                              isExpired: false, canRefresh: true, expiresAt: new Date(now + 3 * hour).toISOString()})
        const view = createView(data.width, data.height)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            const reauth = findChild(view, "reauthenticateRobinSession")
            verify(reauth !== null)
            verify(view.sessionTimeMs !== undefined)
            view.sessionTimeMs = now
            tryCompare(reauth, "visible", false)
            view.sessionTimeMs = now + hour
            tryCompare(reauth, "visible", true)
            const renewed = new Date(now + 6 * hour).toISOString()
            RobinSessions.setProperty(0, "expiresAt", renewed)
            tryCompare(view, "selectedExpiresAt", renewed)
            tryCompare(reauth, "visible", false)
            view.sessionTimeMs = now + 4 * hour
            tryCompare(reauth, "visible", true)
            RobinSessions.setProperty(0, "expiresAt", new Date(now + hour).toISOString())
            view.sessionTimeMs = now - 2 * hour
            tryCompare(reauth, "visible", false)
            const timer = findChild(view, "robinSessionExpiryTimer")
            verify(timer !== null)
            compare(timer.interval, 30000)
            timer.interval = 20
            tryCompare(reauth, "visible", true)
            verify(view.sessionTimeMs >= now)
            compare(RobinSessions.reauthenticateCalls, 0)
        } finally { view.destroy() }
    }
    function test_sessionActionTypography_data() { return sizes() }
    function test_sessionActionTypography(data) {
        addSaved(7, false)
        RobinSessions.setProperty(0, "expiresAt", new Date(Date.now() + 60 * 60 * 1000).toISOString())
        const view = createView(data.width, data.height)
        try {
            verify(findText(view, "Your Sessions") !== null)
            compare(findChild(view, "deleteRobinSession").text, "Delete Saved Account")
            compare(findChild(view, "addRobinAccount").text, "+ Add Another Account")
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            verify(waitForPolish(view.Window.window))
            for (const name of ["startRobinSession", "reauthenticateRobinSession", "deleteRobinSession", "addRobinAccount", "robinAutoMode"]) {
                const button = findChild(view, name)
                verify(button !== null)
                compare(button.font.pixelSize, 18)
                compare(button.font.bold, true)
                compare(button.contentItem.font.pixelSize, 18)
                compare(button.contentItem.font.bold, true)
                tryVerify(() => button.contentItem.implicitWidth <= button.availableWidth + 1)
                verify(button.mapToItem(view, button.width, 0).x <= view.width)
            }
            const autoLabel = findText(view, "Auto Mode")
            verify(autoLabel !== null)
            compare(autoLabel.font.pixelSize, 18)
            compare(autoLabel.font.bold, true)
            const autoButton = findChild(view, "robinAutoMode")
            compare(autoButton.enabled, false)
            verify(autoLabel.implicitWidth <= autoLabel.width + 1)
            verify(autoLabel.mapToItem(view, autoLabel.width, 0).x <= autoButton.mapToItem(view, 0, 0).x)
            if (qmlTestArtifactDirectory.length > 0) {
                reveal(view, autoButton)
                grabImage(view).save(qmlTestArtifactDirectory + "/session-actions-" + data.tag + ".png")
            }
        } finally { view.destroy() }
    }
    function test_overviewHeadingAndPageScopedAutoMode_data() { return sizes() }
    function test_overviewHeadingAndPageScopedAutoMode(data) {
        const view = createView(data.width, data.height)
        try {
            const autoButton = findChild(view, "robinAutoMode")
            verify(autoButton !== null)
            compare(autoButton.visible, true)
            compare(autoButton.enabled, false)
            verify(findText(view, "Your Sessions") !== null)
            RobinSessions.accounts.append(account("100", "Standard Brokerage"))
            RobinSessions.sessionActive = true
            tryCompare(view, "view", 2)
            tryCompare(autoButton, "visible", false)
            compare(findText(view, "Auto Mode").visible, false)
            verify(findText(view, "Account Overview") !== null)
            compare(findText(view, "Account overview"), null)
            const back = findChild(view, "switchRobinAccount")
            verify(back !== null)
            const scroller = findChild(view, "robinSessionScroller")
            verify(scroller !== null)
            tryVerify(() => Math.abs(scroller.contentHeight
                      - back.mapToItem(scroller.contentItem, 0, back.height).y - 24) <= 1)
            reveal(view, back)
            mouseClick(back)
            tryCompare(view, "view", 0)
            tryCompare(autoButton, "visible", true)
            compare(autoButton.enabled, false)
            verify(findText(view, "Your Sessions") !== null)
        } finally { view.destroy() }
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
    function test_addOpensNativeForm_data() { return sizes() }
    function test_addOpensNativeForm(data) {
        const view = createView(data.width, data.height)
        try {
            mouseClick(findChild(view, "addRobinAccount"))
            tryCompare(RobinSessions, "beginNativeCalls", 1)
            tryCompare(view, "view", 3)
            verify(findChild(view, "robinNativeLoginForm") !== null)
            compare(findChild(view, "robinWebLoginPanel"), null)
            compare(RobinSessions.beginBrowserCalls, 0)
            mouseClick(findChild(view, "cancelRobinNativeLogin"))
            tryCompare(RobinSessions, "cancelCalls", 1)
            tryCompare(view, "view", 0)
        } finally { view.destroy() }
    }
    function nativeField(view, name) {
        const field = findChild(view, name)
        verify(field !== null)
        return field
    }
    function nativeReveal(view, item) {
        verify(waitForPolish(view.Window.window))
        findChild(view, "robinNativeLoginScroller").reveal(item)
        verify(waitForPolish(view.Window.window))
    }
    function test_nativeCredentialsAndTheme_data() { return sizes() }
    function test_nativeCredentialsAndTheme(data) {
        RobinSessions.beginNativeLogin()
        const view = createView(data.width, data.height)
        try {
            const email = nativeField(view, "robinNativeEmail")
            const password = nativeField(view, "robinNativePassword")
            const mfa = nativeField(view, "robinNativeAuthenticator")
            const button = nativeField(view, "signInRobinNative")
            compare(RobinSessions.busy, true)
            compare(RobinSessions.nativeLoginWorking, false)
            compare(email.enabled, true)
            compare(password.enabled, true)
            compare(password.echoMode, TextInput.Password)
            compare(mfa.echoMode, TextInput.Password)
            compare(password.background.color, "#142522")
            compare(button.enabled, false)
            email.forceActiveFocus()
            RobinTrades.typeText(email, "  synthetic+密碼@example.invalid  ")
            password.forceActiveFocus()
            RobinTrades.typeText(password, "  päss+密碼  ")
            mfa.forceActiveFocus()
            RobinTrades.typeText(mfa, "123456")
            compare(button.enabled, true)
            RobinSessions.acceptSignIn = false
            nativeReveal(view, button)
            mouseClick(button)
            tryCompare(RobinSessions, "signInCalls", 0)
            compare(password.text, "  päss+密碼  ")
            RobinSessions.acceptSignIn = true
            mouseClick(button)
            tryCompare(RobinSessions, "signInCalls", 1)
            compare(RobinSessions.lastNativeEmail, "synthetic+密碼@example.invalid")
            compare(RobinSessions.passwordMatched, true)
            compare(RobinSessions.mfaMatched, true)
            compare(password.text, "")
            compare(mfa.text, "")
            compare(button.enabled, false)
            compare(email.enabled, false)
            compare(findChild(view, "robinNativeVerificationCode").visible, false)
            compare(findChild(view, "cancelRobinNativeLogin").enabled, true)
            compare(findChild(view, "robinNativeStatus").text, RobinSessions.nativeLoginStatus)
        } finally { view.destroy() }
    }
    function test_nativeVerificationCommitAndSavedEmail_data() { return sizes() }
    function test_nativeVerificationCommitAndSavedEmail(data) {
        RobinSessions.beginNativeLogin()
        const view = createView(data.width, data.height)
        try {
            RobinSessions.nativeRequestStarted = true
            RobinSessions.nativeCodeRequired = true
            compare(RobinSessions.busy, true)
            compare(RobinSessions.nativeLoginWorking, false)
            const code = nativeField(view, "robinNativeVerificationCode")
            const submit = nativeField(view, "submitRobinNativeCode")
            compare(code.echoMode, TextInput.Password)
            compare(code.enabled, true)
            compare(submit.enabled, false)
            code.forceActiveFocus()
            RobinTrades.typeText(code, "654321")
            nativeReveal(view, submit)
            mouseClick(submit)
            tryCompare(RobinSessions, "codeCalls", 1)
            compare(RobinSessions.codeMatched, true)
            compare(code.text, "")
            compare(submit.visible, false)
            RobinSessions.nativeLoginCancellable = false
            const cancel = nativeField(view, "cancelRobinNativeLogin")
            compare(cancel.visible, true)
            compare(cancel.enabled, false)
            compare(findChild(view, "robinNativeStatus").text, "Saving securely… Please wait.")
            keyClick(Qt.Key_Escape)
            compare(RobinSessions.cancelCalls, 0)
            compare(view.view, 3)
            RobinSessions.append({sessionIndex: 0, accountName: "synthetic+密碼@example.invalid", accountId: "stable-0", isExpired: false, expiresAt: "2099-01-01T00:00:00.000Z"})
            RobinSessions.busy = false
            RobinSessions.nativeLoginPending = false
            RobinSessions.sessionSaved(0)
            tryCompare(view, "view", 0)
            tryVerify(() => findChild(view, "robinSessionCard_0") !== null)
            compare(findChild(view, "robinSessionCard_0").Accessible.name, "synthetic+密碼@example.invalid")
            compare(findText(view, "synthetic+密碼@example.invalid").textFormat, Text.PlainText)
        } finally { view.destroy() }
    }
    function test_nativeReauthenticateCancelKeepsRow_data() {
        return [{tag: "valid", expired: false}, {tag: "expired", expired: true}]
    }
    function test_nativeReauthenticateCancelKeepsRow(data) {
        addSaved(7, data.expired)
        if (!data.expired)
            RobinSessions.setProperty(0, "expiresAt", new Date(Date.now() + 60 * 60 * 1000).toISOString())
        RobinSessions.setProperty(0, "accountName", "synthetic@example.invalid")
        const view = createView(360, 620)
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            tryCompare(view, "selectedIndex", 7)
            const reauth = findChild(view, "reauthenticateRobinSession")
            reveal(view, reauth)
            mouseClick(reauth)
            tryCompare(RobinSessions, "reauthenticateCalls", 1)
            compare(RobinSessions.lastNativeAccountId, "stable-7")
            compare(findChild(view, "robinNativeEmail").text, "synthetic@example.invalid")
            compare(findChild(view, "robinNativePassword").text, "")
            mouseClick(findChild(view, "cancelRobinNativeLogin"))
            tryCompare(view, "view", 0)
            compare(view.selectedIndex, 7)
            compare(RobinSessions.count, 1)
            compare(RobinSessions.get(0).accountId, "stable-7")
        } finally { view.destroy() }
    }
    function test_nativeSecretsClearedOnHideAndSave() {
        RobinSessions.beginNativeLogin()
        const view = createView()
        try {
            const password = nativeField(view, "robinNativePassword")
            const mfa = nativeField(view, "robinNativeAuthenticator")
            password.text = "synthetic"
            mfa.text = "123456"
            view.visible = false
            compare(password.text, "")
            compare(mfa.text, "")
            view.visible = true
            password.text = "synthetic"
            RobinSessions.sessionSaved(0)
            compare(password.text, "")
        } finally { view.destroy() }
    }
    function test_nativeLongPlaintextStatusAndKeyboard_data() { return sizes() }
    function test_nativeLongPlaintextStatusAndKeyboard(data) {
        RobinSessions.beginNativeLogin()
        RobinSessions.error = "<b>Safe diagnostic</b> " + "Connection unavailable. ".repeat(80)
        RobinSessions.nativeLoginStatus = "<b>Waiting for mobile approval</b>"
        const view = createView(data.width, data.height)
        try {
            const error = nativeField(view, "robinNativeError")
            compare(error.textFormat, Text.PlainText)
            compare(findChild(view, "robinNativeStatus").textFormat, Text.PlainText)
            tryVerify(() => error.height >= error.implicitHeight && error.height > 100)
            verify(error.mapToItem(view, error.width, 0).x <= view.width)
            const scroller = nativeField(view, "robinNativeLoginScroller")
            scroller.forceActiveFocus()
            keyClick(Qt.Key_End)
            tryVerify(() => scroller.atYEnd)
            keyClick(Qt.Key_Home)
            tryVerify(() => scroller.atYBeginning)
            const email = nativeField(view, "robinNativeEmail")
            const password = nativeField(view, "robinNativePassword")
            email.forceActiveFocus()
            keyClick(Qt.Key_Tab)
            tryCompare(password, "activeFocus", true)
            keyClick(Qt.Key_Tab)
            tryCompare(findChild(view, "robinNativeAuthenticator"), "activeFocus", true)
            const cancel = nativeField(view, "cancelRobinNativeLogin")
            cancel.forceActiveFocus()
            keyClick(Qt.Key_Space)
            tryCompare(view, "view", 0)
        } finally { view.destroy() }
    }
    function test_refreshableSessionStillRenewsWithoutNativeLogin() {
        addSaved(7, true, true)
        const view = createView()
        try {
            tryVerify(() => findChild(view, "robinSessionCard_7") !== null)
            mouseClick(findChild(view, "robinSessionCard_7"))
            const renew = findChild(view, "startRobinSession")
            tryCompare(renew, "text", "Renew Session")
            reveal(view, renew)
            mouseClick(renew)
            tryCompare(RobinSessions, "lastStarted", 7)
            compare(RobinSessions.beginNativeCalls, 0)
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
        RobinSessions.accounts.append(Object.assign(account("100", "Standard Brokerage"), {currentBalance: "$10,245.50"}))
        RobinSessions.accounts.append(account("200", "Roth IRA"))
        RobinSessions.accounts.append(Object.assign(account("300", "IRA"), {currentBalance: "Unavailable"}))
        RobinSessions.sessionActive = true
        const view = createView()
        try {
            compare(findChild(view, "robinAccountName_100").text, "Standard Brokerage")
            compare(findChild(view, "robinAccountBalance_200").text, "Balance: $501.23")
            compare(findChild(view, "robinAccountTile_300").currentBalance, "Unavailable")
            RobinSessions.accounts.clear()
            tryVerify(() => findChild(view, "robinAccountTile_100") === null)
            RobinSessions.accounts.append(Object.assign(account("400", "Standard Brokerage"), {currentBalance: "$0.00"}))
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
