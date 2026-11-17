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

    function test_manualButtonsPreserveTickerList_data() {
        return [{tag: "spaces", input: " aapl TSLA      GM "},
                {tag: "commas", input: "   aapl, TSLA   ,GM "},
                {tag: "mixed", input: "  aapl  TSLA,GM  "}]
    }
    function test_manualButtonsPreserveTickerList(data) {
        const trade = createTemporaryObject(tradeComponent, testCase)
        verify(trade !== null)
        trade.inputFieldText = data.input
        compare(findChild(trade, "tickerInput").Accessible.name, "Ticker symbols, separated by commas or whitespace")
        buySpy.target = trade
        sellSpy.target = trade
        buySpy.clear()
        sellSpy.clear()
        const buyButton = findChild(trade, "buyButton")
        const sellButton = findChild(trade, "sellButton")
        tryVerify(() => buyButton.visible && buyButton.enabled && buyButton.width > 0 && buyButton.height > 0)
        mouseClick(buyButton)
        compare(buySpy.count, 1)
        mouseClick(sellButton)
        compare(sellSpy.count, 1)
        compare(trade.inputFieldText, data.input)
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
