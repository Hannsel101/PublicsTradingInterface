import QtQuick
import QtTest
import PublicsTradingInterface

TestCase {
    id: testCase
    name: "PublicResults"
    when: windowShown
    width: 720
    height: 600
    visible: true

    QtObject {
        id: controller
        property string currentTransactionTitle: "Market Buy of AAPL"
        property bool tradeResultsBusy: false
        property bool tradeResultsComplete: true
        property var tradeResults: [
            {accountId: "A", status: "uncertain", message: "Check order before resubmitting"},
            {accountId: "B", status: "not_submitted", message: "Holdings unavailable"}
        ]
    }
    Component {
        id: screenComponent
        TradeResultScreen { width: 720; height: 600 }
    }
    function containsText(item, text) {
        if (item.text === text) return true
        for (let child of item.children || [])
            if (containsText(child, text)) return true
        return false
    }
    function test_terminalStatesAreNotPending() {
        const screen = createTemporaryObject(screenComponent, testCase, {resultController: controller})
        verify(screen !== null)
        tryVerify(() => containsText(screen, "Outcome uncertain"))
        tryVerify(() => containsText(screen, "Not submitted"))
        verify(!containsText(screen, "Pending"))
    }
}
