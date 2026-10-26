import QtQuick
import QtQuick.Controls
import QtTest
import TradingLauncher

TestCase {
    id: testCase
    name: "LauncherView"
    when: windowShown
    width: 1000
    height: 700

    function init() {
        Launcher.activeAppletId = ""
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign|Binding loop/)
    }

    function createLauncher(width = 1000, height = 700) {
        const component = Qt.createComponent("qrc:/qt/qml/TradingLauncher/LauncherWindow.qml")
        compare(component.status, Component.Ready, component.errorString())
        const window = component.createObject(null, {width: width, height: height, visible: true})
        verify(window !== null, "Component exists")
        return window
    }

    function test_neutralAppletChoicesHaveThemeStripes_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }

    function test_neutralAppletChoicesHaveThemeStripes(data) {
        const window = createLauncher(data.width, data.height)
        try {
            const publics = findChild(window.contentItem, "openPublics")
            const robin = findChild(window.contentItem, "openRobinHood")
            verify(publics !== null, "Object exists")
            verify(robin !== null, "Object exists")
            compare(publics.appletName, qsTr("Publics"))
            compare(robin.appletName, qsTr("Robin Hood"))
            compare(publics.enabled, true)
            compare(robin.enabled, true)
            compare(publics.background.color, robin.background.color)
            compare(publics.stripeColor, "#25285f")
            compare(robin.stripeColor, "#a3e635")
            tryVerify(() => publics.background.width > 0 && robin.background.width > 0)
            const stripe = publics.background.children[0]
            verify(Math.abs(stripe.x + stripe.width / 2 - publics.background.width * 0.7) < 1)
            const robinStripe = robin.background.children[0]
            verify(Math.abs(robinStripe.x + robinStripe.width / 2 - robin.background.width * 0.7) < 1)
            verify(Math.abs(publics.width - (window.width - 48)) <= 1)
            verify(Math.abs(robin.width - (window.width - 48)) <= 1)
        } finally {
            window.destroy()
        }
    }

    function test_upcomingChoicesAreVisibleButDisabled_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_upcomingChoicesAreVisibleButDisabled(data) {
        const window = createLauncher(data.width, data.height)
        try {
            const publics = findChild(window.contentItem, "openPublics")
            const choices = [
                {id: "openWellsFargo", name: "Wells Fargo", stripe: "#c5303e"},
                {id: "openFidelity", name: "Fidelity", stripe: "#14633e"},
                {id: "openChase", name: "Chase", stripe: "#236dc2"}
            ]
            for (const expected of choices) {
                const choice = findChild(window.contentItem, expected.id)
                verify(choice !== null, expected.name + " choice exists")
                compare(choice.appletName, expected.name)
                compare(choice.visible, true)
                compare(choice.enabled, false)
                compare(choice.Accessible.name, expected.name + " (coming soon)")
                compare(choice.stripeColor, expected.stripe)
                compare(choice.background.children[0].color, expected.stripe)
                verify(choice.background.color !== publics.background.color)
                verify(choice.contentItem.color !== publics.contentItem.color)
                verify(Math.abs(choice.width - (window.width - 48)) <= 1)
                const position = choice.mapToItem(window.contentItem, 0, 0)
                verify(position.y >= 0 && position.y + choice.height <= window.height)
                mouseClick(choice)
                compare(Launcher.activeAppletId, "")
            }
        } finally {
            window.destroy()
        }
    }
    function test_embedsAppletInExistingWindowAndReturnsHome_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_embedsAppletInExistingWindowAndReturnsHome(data) {
        const window = createLauncher(data.width, data.height)
        try {
            const publics = findChild(window.contentItem, "openPublics")
            const loader = findChild(window.contentItem, "appletLoader")
            verify(publics !== null, "Object exists")
            verify(loader !== null, "Object exists")
            compare(loader.item, null)
            mouseClick(publics)
            tryCompare(Launcher, "activeAppletId", "publics")
            tryVerify(() => loader.item !== null)
            compare(loader.item.Window.window, window)
            const robinLoader = findChild(window.contentItem, "robinHoodLoader")
            verify(robinLoader !== null, "Object exists")
            tryCompare(robinLoader, "visible", false)
            tryVerify(() => loader.width >= window.width - 2)
            tryVerify(() => loader.height >= window.height - 54)
            tryVerify(() => loader.item.width >= window.width - 2
                        && loader.item.height >= window.height - 54)
            const back = findChild(window.contentItem, "returnHome")
            verify(back !== null, "Object exists")
            mouseClick(back)
            tryCompare(Launcher, "activeAppletId", "")
            tryCompare(loader, "item", null)
        } finally {
            window.destroy()
        }
    }
    function test_robinBrowserFillsAppletViewport_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_robinBrowserFillsAppletViewport(data) {
        RobinSessions.clear()
        RobinSessions.sessionActive = false
        RobinSessions.busy = false
        RobinSessions.browserLoginPending = false
        RobinBrowser.available = true
        RobinBrowser.active = false
        RobinBrowser.viewSource = Qt.resolvedUrl("MockWebLogin.qml")
        const window = createLauncher(data.width, data.height)
        try {
            mouseClick(findChild(window.contentItem, "openRobinHood"))
            const applet = findChild(window.contentItem, "robinHoodLoader")
            tryVerify(() => applet.status === Loader.Ready && applet.item !== null
                        && applet.item.width >= window.width - 2 && applet.item.height > 0)
            const add = findChild(applet.item, "addRobinAccount")
            const scroller = findChild(applet.item, "robinSessionScroller")
            tryVerify(() => add.width > 0 && add.height > 0 && scroller.height > 0)
            scroller.contentY = Math.max(0, add.mapToItem(scroller.contentItem, 0, add.height).y
                                        - scroller.height + 12)
            mouseClick(add)
            tryCompare(RobinBrowser, "active", true)
            tryVerify(() => findChild(applet.item, "robinWebLoginLoader") !== null)
            const browser = findChild(applet.item, "robinWebLoginLoader")
            tryVerify(() => browser.status === Loader.Ready && browser.item !== null
                        && browser.item.width >= applet.width - 2
                        && browser.item.height > applet.height * 0.6)
            compare(browser.item.objectName, "mockRobinWebLogin")
            const origin = findChild(applet.item, "robinBrowserOrigin")
            const cancel = findChild(applet.item, "cancelRobinBrowserLogin")
            const originEnd = origin.mapToItem(window.contentItem, origin.width, origin.height)
            const cancelStart = cancel.mapToItem(window.contentItem, 0, 0)
            const cancelEnd = cancel.mapToItem(window.contentItem, cancel.width, cancel.height)
            const webStart = browser.item.mapToItem(window.contentItem, 0, 0)
            const webEnd = browser.item.mapToItem(window.contentItem, browser.item.width, browser.item.height)
            verify(originEnd.x <= cancelStart.x)
            verify(cancelStart.y >= 0 && cancelEnd.x <= window.width)
            verify(webStart.y >= Math.max(originEnd.y, cancelEnd.y))
            verify(webEnd.y <= window.height + 1)
            compare(findChild(applet.item, "robinSessionScroller").visible, false)
            mouseClick(cancel)
            tryCompare(RobinBrowser, "active", false)
            mouseClick(findChild(window.contentItem, "returnHome"))
            tryCompare(applet, "item", null)
        } finally {
            RobinSessions.cancelSignIn()
            window.destroy()
        }
    }
    function test_robinAppletUsesSameWindowAndReturnsHome() {
        const window = createLauncher()
        try {
            const robin = findChild(window.contentItem, "openRobinHood")
            const loader = findChild(window.contentItem, "robinHoodLoader")
            verify(robin !== null)
            verify(loader !== null)
            mouseClick(robin)
            tryCompare(Launcher, "activeAppletId", "robin_hood")
            tryVerify(() => loader.item !== null)
            compare(loader.item.Window.window, window)
            const publicsLoader = findChild(window.contentItem, "appletLoader")
            verify(publicsLoader !== null, "Object exists")
            tryCompare(publicsLoader, "visible", false)
            tryVerify(() => loader.width >= window.width - 2
                        && loader.height >= window.height - 54)
            tryVerify(() => loader.item.width >= window.width - 2
                        && loader.item.height >= window.height - 54)
            mouseClick(findChild(window.contentItem, "returnHome"))
            tryCompare(loader, "item", null)
        } finally {
            window.destroy()
        }
    }
}
