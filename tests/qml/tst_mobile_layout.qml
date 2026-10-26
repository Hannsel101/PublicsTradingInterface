import QtQuick
import QtCore
import QtQuick.Controls.Basic
import QtTest
import PublicsTradingInterface

TestCase {
    id: testCase
    name: "MobileLayout"
    when: windowShown
    visible: true
    width: 480
    height: 900

    Component {
        id: mainComponent
        ApplicationWindow {
            width: 480
            height: 900
            property alias autoMode: core.autoMode
            CoreView { id: core; anchors.fill: parent }
        }
    }

    function findItem(item, predicate) {
        if (predicate(item))
            return item
        const children = item.children || []
        for (let i = 0; i < children.length; ++i) {
            const match = findItem(children[i], predicate)
            if (match !== null)
                return match
        }
        return null
    }

    function verifyInside(item, container, description) {
        const position = item.mapToItem(container, 0, 0)
        verify(position.x >= -1 && position.y >= -1
               && position.x + item.width <= container.width + 1
               && position.y + item.height <= container.height + 1,
               description + ": item=" + [position.x, position.y, item.width, item.height]
               + " container=" + [container.width, container.height])
    }

    function init() {
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign|Binding loop|Detected recursive rearrange|Invalid arguments/)
        AuthClient.sessionActive = false
        AuthClient.selectedApiKeyLabel = "PublicsApiKey0"
        AuthClient.apiKeyReady = true
        AuthClient.apiKeyIndexLoading = false
        AuthClient.secretKeys = ["PublicsApiKey0", "PublicsApiKey1"]
        AuthClient.apiKeyNicknames = ({PublicsApiKey0: "Long brokerage nickname " + "W".repeat(80)})
        ApiWorker.tradeResultsVisible = false
        AutoTrader.tradeResultsVisible = false
        AutoTrader.keyLabels = AuthClient.secretKeys
    }

    function cleanup() {
        AuthClient.sessionActive = false
        AuthClient.selectedApiKeyLabel = ""
        AuthClient.apiKeyReady = false
        AuthClient.secretKeys = []
        AuthClient.apiKeyNicknames = ({})
        AutoTrader.keyLabels = []
        AutoTrader.tradeResultsVisible = false
        ApiWorker.tradeResultsVisible = false
    }

    function verifyTextFits(item) {
        if (!item.visible)
            return
        if ("contentWidth" in item && "elide" in item && item.width > 0)
            verify(item.elide !== Text.ElideNone || item.contentWidth <= item.width + 1,
                   "Text must wrap or elide within its container: " + item.text
                   + " contentWidth=" + item.contentWidth + " width=" + item.width)
        const children = item.children || []
        for (let i = 0; i < children.length; ++i)
            verifyTextFits(children[i])
    }

    function test_keyDialogsFitViewport_data() {
        return [
            {tag: "edit-portrait", action: "Edit nickname", title: "Edit API key nickname", width: 360, height: 780},
            {tag: "delete-portrait", action: "Delete ", title: "Delete saved API key?", width: 360, height: 780},
            {tag: "edit-landscape", action: "Edit nickname", title: "Edit API key nickname", width: 780, height: 360},
            {tag: "delete-landscape", action: "Delete ", title: "Delete saved API key?", width: 780, height: 360},
            {tag: "edit-short-viewport", action: "Edit nickname", title: "Edit API key nickname", width: 780, height: 220}
        ]
    }

    function test_keyDialogsFitViewport(data) {
        const window = createTemporaryObject(mainComponent, null,
                                             {width: data.width, height: data.height,
                                              minimumHeight: 0, visible: true})
        verify(window !== null)
        window.font.pixelSize = 26
        const keys = findItem(window.contentItem, item => "apiKeysModel" in item)
        const list = findItem(keys, item => "itemAtIndex" in item)
        tryVerify(() => list.itemAtIndex(0) !== null)
        const action = findItem(list.itemAtIndex(0),
                                item => "clicked" in item && item.Accessible.name.startsWith(data.action))
        verify(action !== null)
        action.clicked()
        const overlay = window.Overlay.overlay
        tryVerify(() => findItem(overlay, item => "text" in item && item.text === data.title) !== null)
        wait(100)
        const title = findItem(overlay, item => "text" in item && item.text === data.title)
        verifyTextFits(overlay)
        // Popup visual items are direct overlay children; content may become scrollable.
        let popup = title
        while (popup.parent !== overlay)
            popup = popup.parent
        verifyInside(popup, overlay, "Key dialog must fit the viewport")
        const cancel = findItem(popup, item => "clicked" in item && item.text === "Cancel")
        verify(cancel !== null)
        const scroll = findItem(popup, item => "contentY" in item && "contentHeight" in item)
        verify(scroll !== null)
        if (scroll.contentHeight > scroll.height) {
            scroll.contentY = scroll.contentHeight - scroll.height
            wait(50)
        }
        verifyInside(cancel, scroll, "Dialog actions must remain reachable by scrolling")
        cancel.clicked()
        tryCompare(popup, "visible", false)
    }

    function test_tradeControlsFitCards_data() {
        return [
            {tag: "narrow-short", width: 360, height: 360},
            {tag: "portrait", width: 480, height: 780},
            {tag: "desktop", width: 1000, height: 700}
        ]
    }

    function test_tradeControlsFitCards(data) {
        AuthClient.sessionActive = true
        const window = createTemporaryObject(mainComponent, null,
                                             {width: data.width, height: data.height, minimumHeight: 0, visible: true})
        verify(window !== null)
        wait(100)
        const stock = findItem(window.contentItem, item => item.visible && "inputFieldText" in item)
        verify(stock !== null)
        const description = findItem(stock, item => "text" in item && item.text.startsWith("Buy trades one share"))
        verify(description !== null)
        verifyInside(description, description.parent.parent, "Trading description must fit its card")
        verifyInside(findChild(stock, "tickerInput"), stock, "Ticker input must fit its card")
        verifyInside(findChild(stock, "buyButton"), stock, "Buy action must fit its card")
        verifyInside(findChild(stock, "sellButton"), stock, "Sell action must fit its card")
        const timer = findItem(window.contentItem, item => item.visible && "secondsRemaining" in item)
        verify(timer !== null)
        const expiry = findItem(timer, item => "text" in item && item.text.startsWith("Public.com access tokens"))
        verify(expiry !== null)
        verifyInside(expiry, timer, "Session explanation must fit its card")
    }

    function test_textStaysInsideCards_data() {
        return [
            {tag: "manual", autoMode: false, sessionActive: false},
            {tag: "auto", autoMode: true, sessionActive: false},
            {tag: "active-workspace", autoMode: false, sessionActive: true}
        ]
    }

    function test_textStaysInsideCards(data) {
        AuthClient.sessionActive = data.sessionActive
        const window = createTemporaryObject(mainComponent, null,
                                             {width: 360, height: 780, visible: true, autoMode: data.autoMode})
        verify(window !== null)
        window.font.pixelSize = 26
        wait(100)
        if (data.sessionActive) {
            const heading = findItem(window.contentItem, item => "text" in item && item.text === "Trade workspace")
            verify(heading !== null)
            const header = heading.parent.parent
            verifyInside(header, header.parent, "Trading header must fit its card")
            for (let i = 0; i < header.children.length; ++i) {
                if (header.children[i].visible)
                    verifyInside(header.children[i], header.parent, "Trading header control must fit its card")
            }
        }
        verifyTextFits(window.contentItem)
        if (Qt.platform.os === "android") {
            const capturePath = decodeURIComponent(StandardPaths.writableLocation(StandardPaths.CacheLocation)
                                                    .toString().replace(/^file:\/\//, ""))
                              + "/mobile-" + (data.sessionActive ? "active" : data.autoMode ? "auto" : "manual") + ".png"
            grabImage(window.contentItem).save(capturePath)
            console.info("Layout capture:", capturePath, "viewport:", window.width, window.height)
        }
    }

    function test_keyRowsStayInsideContainers_data() {
        return [
            {tag: "narrow", width: 360, height: 780, fontSize: 16},
            {tag: "portrait", width: 412, height: 915, fontSize: 16},
            {tag: "s24-emulator", width: 480, height: 1040, fontSize: 16},
            {tag: "zoomed-large-font", width: 360, height: 780, fontSize: 26},
            {tag: "landscape", width: 915, height: 412, fontSize: 20},
            {tag: "desktop", width: 1000, height: 700, fontSize: 16}
        ]
    }

    function test_keyRowsStayInsideContainers(data) {
        const window = createTemporaryObject(mainComponent, null,
                                             {width: data.width, height: data.height, visible: true})
        verify(window !== null)
        window.font.pixelSize = data.fontSize
        const keys = findItem(window.contentItem, item => "apiKeysModel" in item)
        verify(keys !== null)
        const list = findItem(keys, item => "itemAtIndex" in item)
        verify(list !== null)
        tryVerify(() => list.count === 2 && list.itemAtIndex(0) !== null)
        wait(50)
        verifyInside(keys, keys.parent, "API key list must fit its card")
        for (let ancestor = keys.parent; ancestor.parent !== null; ancestor = ancestor.parent) {
            if (ancestor.parent.width > 0)
                verify(ancestor.x >= -1 && ancestor.x + ancestor.width <= ancestor.parent.width + 1,
                       "Key card ancestor exceeds its parent: " + ancestor
                       + " x=" + ancestor.x + " width=" + ancestor.width
                       + " parentWidth=" + ancestor.parent.width)
        }
        const row = list.itemAtIndex(0)
        const layout = row.children[0]
        for (let i = 0; i < layout.children.length; ++i) {
            const child = layout.children[i]
            if (child.visible)
                verifyInside(child, row, "API key row child " + i)
        }
        const nickname = findItem(row, item => "text" in item
                                  && item.text === AuthClient.apiKeyNicknames.PublicsApiKey0)
        verify(nickname !== null)
        verifyInside(nickname, row, "Long nickname must stay inside its row")
        const actions = layout.children
        for (let i = 1; i < actions.length; ++i)
            verify(actions[i].x >= actions[i - 1].x + actions[i - 1].width - 1,
                   "API key row controls must not overlap")
    }
}
