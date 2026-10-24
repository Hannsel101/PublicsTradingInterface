import QtQuick
import QtTest
import PublicsTradingInterface

TestCase {
    id: testCase
    name: "ApplicationComponents"
    when: windowShown
    width: 1000
    height: 700

    Component { id: mainComponent; Main { visible: false } }
    Component { id: timerComponent; SessionTimer {} }
    Component { id: switchComponent; SwitchKeyButton {} }
    Component { id: autoComponent; AutoTradeScreen { width: 640 } }

    function init() {
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign/)
        AuthClient.sessionActive = false
        AuthClient.apiKeyReady = false
        AuthClient.secretKeys = []
        AuthClient.selectedApiKeyLabel = ""
        ApiWorker.tradeResultsVisible = false
    }

    function cleanup() {
        AuthClient.sessionActive = false
        AuthClient.apiKeyReady = false
        AuthClient.secretKeys = []
        AuthClient.selectedApiKeyLabel = ""
        ApiWorker.tradeResultsVisible = false
    }

    function test_applicationModuleLoads_data() {
        return [{tag: "desktop", width: 1000}, {tag: "compact", width: 390}]
    }

    function test_applicationModuleLoads(data) {
        AuthClient.secretKeys = ["test-key"]
        const window = createTemporaryObject(mainComponent, null, {width: data.width})
        verify(window !== null)
        compare(window.autoMode, false)
        compare(window.keySelected, false)
        AuthClient.selectedApiKeyLabel = "test-key"
        AuthClient.apiKeyReady = true
        compare(window.canStartSession, true)
        AuthClient.sessionActive = true
        compare(window.canStartSession, false)
    }

    function findText(item, text) {
        if ("text" in item && item.text === text)
            return item
        const children = item.children || []
        for (let i = 0; i < children.length; ++i) {
            const match = findText(children[i], text)
            if (match !== null)
                return match
        }
        return null
    }

    function containsText(item, text) {
        return findText(item, text) !== null
    }

    function test_autoModeHeaderLayout_data() {
        return [{tag: "desktop", width: 1000}, {tag: "compact", width: 390}]
    }

    function test_autoModeHeaderLayout(data) {
        const window = createTemporaryObject(mainComponent, null,
                                             {width: data.width, visible: true, autoMode: true})
        verify(window !== null)
        const title = findText(window.contentItem, "All-Key Brokerage Console")
        const button = findText(window.contentItem, "Main menu")
        const keys = findText(window.contentItem, "Saved API keys · " + AutoTrader.keyLabels.length)
        verify(title !== null && button !== null && keys !== null)
        tryVerify(() => title.width > 0 && button.width > 0 && keys.width > 0)
        tryVerify(() => Math.abs(title.mapToItem(window.contentItem, 0, title.height / 2).y
                                - button.mapToItem(window.contentItem, 0, button.height / 2).y) <= 1,
                  1000, "Main menu must align vertically with the console title")
        const titleRight = title.mapToItem(window.contentItem, title.width, 0).x
        const buttonLeft = button.mapToItem(window.contentItem, 0, 0).x
        verify(buttonLeft >= titleRight, "Main menu must sit to the right without overlap")
        const titleBottom = title.mapToItem(window.contentItem, 0, title.height).y
        const keysTop = keys.mapToItem(window.contentItem, 0, 0).y
        verify(keysTop >= titleBottom)
        verify(keysTop - titleBottom <= (window.compactLayout ? 18 : 24) + 18 + 1,
               "Saved keys must follow the header without an extra navigation row")
        button.clicked()
        compare(window.autoMode, false)
        compare(button.visible, false)
    }

    function test_redundantInstructionsRemoved_data() {
        return [
            {tag: "session-description", text: "Store Public.com API keys once, choose a saved key, then start a short-lived session that loads every eligible brokerage account for that key"},
            {tag: "key-editing-description", text: "Use the pencil to edit a key nickname or the trash button to remove a key after confirmation. API secrets stay in the OS-backed keychain."}
        ]
    }

    function test_redundantInstructionsRemoved(data) {
        const window = createTemporaryObject(mainComponent, null)
        verify(window !== null)
        verify(!containsText(window.contentItem, data.text), "Redundant instruction label must be removed")
    }

    function test_sessionTimeoutUsesAuthState() {
        const timer = createTemporaryObject(timerComponent, testCase)
        verify(timer !== null)
        AuthClient.sessionActive = true
        compare(timer.secondsRemaining, 300)
        timer.performTimeoutAction()
        compare(AuthClient.sessionActive, false)
        compare(timer.secondsRemaining, 0)
    }

    function test_switchKeyEndsSession() {
        AuthClient.sessionActive = true
        ApiWorker.tradeResultsVisible = true
        const button = createTemporaryObject(switchComponent, testCase)
        verify(button !== null)
        button.clicked()
        compare(AuthClient.sessionActive, false)
        compare(ApiWorker.tradeResultsVisible, false)
    }

    function test_autoModeHeaderRemoved_data() {
        return [
            {tag: "title", text: "Auto Mode"},
            {tag: "description", text: "One action across every saved key and its eligible brokerage accounts. No key selection required."}
        ]
    }

    function test_autoModeHeaderRemoved(data) {
        const screen = createTemporaryObject(autoComponent, testCase)
        verify(screen !== null)
        verify(!containsText(screen, data.text), "Redundant Auto Mode header must be removed")
    }

    function test_autoModeLoadsWithManualEntry() {
        const screen = createTemporaryObject(autoComponent, testCase)
        verify(screen !== null)
        const field = findChild(screen, "tickerInput")
        verify(field !== null)
        field.text = "AAPL, TSLA, GM"
        compare(field.text, "AAPL, TSLA, GM")
    }
}
