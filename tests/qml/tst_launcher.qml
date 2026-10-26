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
