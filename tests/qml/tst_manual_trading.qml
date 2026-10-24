import QtQuick
import QtTest
import PublicsTradingInterface

TestCase {
    id: testCase
    name: "ManualTrading"
    when: windowShown
    visible: true
    width: 1000
    height: 700

    Component {
        id: tradeComponent
        StockTrade { width: 640; height: 320 }
    }

    SignalSpy { id: buySpy; signalName: "performBuy" }
    SignalSpy { id: sellSpy; signalName: "performSell" }

    function test_manualButtonsPreserveTickerList() {
        const trade = createTemporaryObject(tradeComponent, testCase)
        verify(trade !== null)
        trade.inputFieldText = "AAPL, TSLA, GM"
        buySpy.target = trade
        sellSpy.target = trade
        const buyButton = findChild(trade, "buyButton")
        const sellButton = findChild(trade, "sellButton")
        tryVerify(() => buyButton.visible && buyButton.enabled && buyButton.width > 0 && buyButton.height > 0)
        mouseClick(buyButton)
        compare(buySpy.count, 1)
        mouseClick(sellButton)
        compare(sellSpy.count, 1)
        compare(trade.inputFieldText, "AAPL, TSLA, GM")
        buySpy.target = null
        sellSpy.target = null
    }

    function test_manualCommaSeparatedTickers() {
        const trade = createTemporaryObject(tradeComponent, testCase)
        verify(trade !== null)
        const field = findChild(trade, "tickerInput")
        verify(field !== null)
        trade.inputFieldText = "AAPL, TSLA, GM"
        compare(field.text, "AAPL, TSLA, GM")
        field.text = "msft, aapl,"
        compare(trade.inputFieldText, "msft, aapl,")
    }

    function test_noAutocompleteDependency() {
        const trade = createTemporaryObject(tradeComponent, testCase)
        verify(trade !== null)
        verify(!("searchController" in trade), "Manual ticker entry must not require a search controller")
    }
}
