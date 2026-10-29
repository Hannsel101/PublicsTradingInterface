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

    function test_telegramChoiceHasCyanStripeAndCenteredRobot_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_telegramChoiceHasCyanStripeAndCenteredRobot(data) {
        const window = createLauncher(data.width, data.height)
        if (!window.telegramBotsAvailable) {
            window.destroy()
            skip("Telegram Bots is desktop-only")
            return
        }
        try {
            const button = findChild(window.contentItem, "openTelegramBots")
            verify(button !== null, "Object exists")
            compare(button.appletName, "Telegram Bots")
            compare(button.enabled, true)
            compare(button.stripeColor, "#00d2ff")
            verify(waitForPolish(window))
            const robot = findChild(button, "appletArtwork")
            verify(robot !== null, "Object exists")
            tryCompare(robot, "status", Image.Ready)
            const stripe = button.background.children[0]
            const stripeRight = stripe.mapToItem(button, stripe.width, 0).x
            const center = robot.mapToItem(button, robot.width / 2, robot.height / 2)
            verify(Math.abs(center.x - (stripeRight + button.width) / 2) < 1)
            verify(Math.abs(center.y - button.height / 2) < 1)
            verify(robot.width > 0 && center.x - robot.width / 2 > stripeRight)
            verify(center.x + robot.width / 2 < button.width)
            compare(button.contentItem.truncated, false)
            const label = button.contentItem
            const labelCenter = label.mapToItem(button,
                (label.width + label.leftPadding - label.rightPadding) / 2, label.height / 2)
            verify(Math.abs(labelCenter.x - button.width / 2) < 1, "Label is centered across the button")
            verify(labelCenter.x + label.contentWidth / 2 + 6 < stripe.mapToItem(button, 0, 0).x,
                   "Centered text stays clear of the stripe")
            const position = button.mapToItem(window.contentItem, 0, 0)
            verify(position.y >= 0 && position.y + button.height <= window.height)
        } finally { window.destroy() }
    }

    function test_telegramConsoleIsSeparateAndReturnsHome_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_telegramConsoleIsSeparateAndReturnsHome(data) {
        const window = createLauncher(data.width, data.height)
        if (!window.telegramBotsAvailable) {
            window.destroy()
            skip("Telegram Bots is desktop-only")
            return
        }
        try {
            const selectedKey = AuthClient.selectedApiKeyLabel
            const sessionActive = AuthClient.sessionActive
            mouseClick(findChild(window.contentItem, "openTelegramBots"))
            tryCompare(Launcher, "activeAppletId", "telegram_bots")
            const loader = findChild(window.contentItem, "telegramBotsLoader")
            verify(loader !== null, "Object exists")
            tryVerify(() => loader.status === Loader.Ready && loader.item !== null)
            verify(waitForPolish(window))
            tryVerify(() => loader.item.width >= window.width - 2 && loader.item.height >= window.height - 54)
            compare(findChild(window.contentItem, "activeAppletTitle").text, "Telegram Bots")
            const brand = findChild(loader.item, "telegramBrand")
            compare(brand.text, "Telegram Bots")
            compare(brand.color, "#00d2ff")
            compare(findChild(loader.item, "telegramConsoleTitle").text, "Telegram Bot Console")
            compare(findChild(loader.item, "telegramManageTitle").text, "Manage Telegram Bots")
            compare(findChild(loader.item, "telegramEmptyState").text, "No bots configured")
            tryCompare(findChild(loader.item, "telegramEmptyRobot"), "status", Image.Ready)
            compare(findChild(window.contentItem, "appletLoader").item, null)
            compare(findChild(window.contentItem, "robinHoodLoader").item, null)
            compare(AuthClient.selectedApiKeyLabel, selectedKey)
            compare(AuthClient.sessionActive, sessionActive)
            if (qmlTestArtifactDirectory.length > 0)
                grabImage(window.contentItem).save(qmlTestArtifactDirectory + "/telegram-bots-" + data.tag + ".png")
            mouseClick(findChild(window.contentItem, "returnHome"))
            tryCompare(Launcher, "activeAppletId", "")
            tryCompare(loader, "item", null)
        } finally { window.destroy() }
    }

    function test_telegramUnavailableDoesNotLoadDesktopResources_data() {
        return [{tag: "wide-unavailable", width: 1000, height: 700},
                {tag: "narrow-unavailable", width: 360, height: 620}]
    }
    function test_telegramUnavailableDoesNotLoadDesktopResources(data) {
        const window = createLauncher(data.width, data.height)
        try {
            compare(window.telegramBotsAvailable, Qt.platform.os === "windows" || Qt.platform.os === "osx")
            window.telegramBotsAvailable = false
            verify(waitForPolish(window))
            const button = findChild(window.contentItem, "openTelegramBots")
            verify(button !== null, "Object exists")
            compare(button.visible, false)
            compare(button.enabled, false)
            compare(button.artworkSource.toString(), "")
            Launcher.activeAppletId = "telegram_bots"
            const loader = findChild(window.contentItem, "telegramBotsLoader")
            verify(loader !== null, "Object exists")
            compare(loader.active, false)
            compare(loader.source.toString(), "")
            compare(loader.item, null)
        } finally { window.destroy() }
    }

    function test_chooserHeadingAndButtonTypography_data() {
        const desktopBots = Qt.platform.os === "windows" || Qt.platform.os === "osx"
        return [{tag: "desktop", width: 1000, height: 700, robinAvailable: true, telegramAvailable: desktopBots},
                {tag: "compact", width: 360, height: 620, robinAvailable: true, telegramAvailable: desktopBots},
                {tag: "android-desktop", width: 1000, height: 700, robinAvailable: false, telegramAvailable: false},
                {tag: "android-compact", width: 360, height: 620, robinAvailable: false, telegramAvailable: false},
                {tag: "ios-desktop", width: 1000, height: 700, robinAvailable: true, telegramAvailable: false},
                {tag: "ios-compact", width: 360, height: 620, robinAvailable: true, telegramAvailable: false}]
    }
    function test_chooserHeadingAndButtonTypography(data) {
        const window = createLauncher(data.width, data.height)
        try {
            window.robinHoodAvailable = data.robinAvailable
            window.telegramBotsAvailable = data.telegramAvailable
            verify(waitForPolish(window))
            const publics = findChild(window.contentItem, "openPublics")
            verify(publics !== null)
            compare(publics.parent.children[0].text, "Choose an Applet")
            for (const name of ["openPublics", "openRobinHood", "openWellsFargo", "openFidelity", "openChase"]) {
                const button = findChild(window.contentItem, name)
                verify(button !== null)
                compare(button.font.pixelSize, 18)
                compare(button.font.bold, true)
                compare(button.contentItem.font.pixelSize, 18)
                compare(button.contentItem.font.bold, true)
                tryVerify(() => button.contentItem.implicitWidth <= button.availableWidth + 1)
                verify(button.mapToItem(window.contentItem, 0, 0).x >= 0)
                verify(button.mapToItem(window.contentItem, button.width, 0).x <= window.width)
            }
            compare(publics.enabled, true)
            compare(findChild(window.contentItem, "openRobinHood").enabled, data.robinAvailable)
            compare(findChild(window.contentItem, "openTelegramBots").visible, data.telegramAvailable)
            for (const name of ["openWellsFargo", "openFidelity", "openChase"])
                compare(findChild(window.contentItem, name).enabled, false)
            compare(Launcher.activeAppletId, "")
            if (qmlTestArtifactDirectory.length > 0)
                grabImage(window.contentItem).save(qmlTestArtifactDirectory + "/applet-chooser-" + data.tag + ".png")
        } finally { window.destroy() }
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
            compare(robin.enabled, Qt.platform.os !== "android")
            if (robin.enabled)
                compare(publics.background.color, robin.background.color)
            else
                verify(publics.background.color !== robin.background.color)
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

    function test_robinHoodPlatformAvailability_data() {
        return [{tag: "android-desktop-size", available: false, width: 1000, height: 700},
                {tag: "android-compact", available: false, width: 360, height: 620},
                {tag: "desktop", available: true, width: 1000, height: 700},
                {tag: "desktop-compact", available: true, width: 360, height: 620}]
    }
    function test_robinHoodPlatformAvailability(data) {
        const window = createLauncher(data.width, data.height)
        try {
            compare(window.robinHoodAvailable, Qt.platform.os !== "android")
            window.robinHoodAvailable = data.available
            const robin = findChild(window.contentItem, "openRobinHood")
            const publics = findChild(window.contentItem, "openPublics")
            const loader = findChild(window.contentItem, "robinHoodLoader")
            compare(robin.visible, true)
            compare(robin.enabled, data.available)
            compare(publics.enabled, true)
            tryVerify(() => robin.width > 0 && robin.height > 0)
            const position = robin.mapToItem(window.contentItem, 0, 0)
            verify(position.y >= 0 && position.y + robin.height <= window.height)
            if (!data.available) {
                compare(robin.background.color, "#18191d")
                compare(robin.contentItem.color, "#9299a3")
                compare(robin.background.children[0].opacity, 0.65)
                waitForRendering(window.contentItem)
                const image = grabImage(window.contentItem)
                const scale = image.width / window.width
                compare(image.pixel(Math.round((position.x + 12) * scale),
                                    Math.round((position.y + robin.height / 2) * scale)), "#18191d")
                compare(robin.Accessible.name, "Robin Hood (unavailable on Android)")
                mouseClick(robin)
                compare(Launcher.activeAppletId, "")
                compare(loader.item, null)
                mouseClick(publics)
                tryCompare(Launcher, "activeAppletId", "publics")
            } else {
                compare(robin.background.color, publics.background.color)
                compare(robin.Accessible.name, "Robin Hood")
                mouseClick(robin)
                tryCompare(Launcher, "activeAppletId", "robin_hood")
            }
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
                const scroller = findChild(window.contentItem, "appletChooser").contentItem
                scroller.contentY = Math.max(0, choice.mapToItem(scroller.contentItem, 0, choice.height).y
                                            - scroller.height + 12)
                verify(waitForPolish(window))
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
    function test_robinNativeLoginFillsAppletViewport_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_robinNativeLoginFillsAppletViewport(data) {
        RobinSessions.clear()
        RobinSessions.sessionActive = false
        RobinSessions.busy = false
        RobinSessions.cancelSignIn()
        const window = createLauncher(data.width, data.height)
        window.robinHoodAvailable = true // Exercise the mocked applet on any host.
        try {
            mouseClick(findChild(window.contentItem, "openRobinHood"))
            const applet = findChild(window.contentItem, "robinHoodLoader")
            tryVerify(() => applet.status === Loader.Ready && applet.item !== null
                        && applet.item.width >= window.width - 2 && applet.item.height > 0)
            const add = findChild(applet.item, "addRobinAccount")
            const picker = findChild(applet.item, "robinSessionScroller")
            tryVerify(() => add.width > 0 && add.height > 0 && picker.height > 0)
            picker.contentY = Math.max(0, add.mapToItem(picker.contentItem, 0, add.height).y
                                      - picker.height + 12)
            mouseClick(add)
            tryCompare(RobinSessions, "nativeLoginPending", true)
            const form = findChild(applet.item, "robinNativeLoginForm")
            verify(form !== null)
            tryVerify(() => form.visible && form.width >= applet.width - 2
                        && form.height > applet.height * 0.6)
            verify(waitForPolish(window))
            const scroller = findChild(form, "robinNativeLoginScroller")
            const cancel = findChild(form, "cancelRobinNativeLogin")
            const start = form.mapToItem(window.contentItem, 0, 0)
            const end = form.mapToItem(window.contentItem, form.width, form.height)
            const cancelStart = cancel.mapToItem(window.contentItem, 0, 0)
            const cancelEnd = cancel.mapToItem(window.contentItem, cancel.width, cancel.height)
            verify(start.x >= 0 && start.y >= 0)
            verify(end.x <= window.width + 1 && end.y <= window.height + 1)
            verify(scroller.height > 0 && scroller.contentWidth <= scroller.width + 1)
            verify(cancel.visible && cancel.enabled && cancelStart.y >= start.y)
            verify(cancelEnd.x <= window.width && cancelEnd.y <= window.height + 1)
            compare(findChild(form, "robinNativePassword").echoMode, TextInput.Password)
            compare(findChild(applet.item, "robinWebLoginLoader"), null)
            compare(picker.visible, false)
            mouseClick(cancel)
            tryCompare(RobinSessions, "nativeLoginPending", false)
            mouseClick(findChild(window.contentItem, "returnHome"))
            tryCompare(applet, "item", null)
        } finally {
            RobinSessions.cancelSignIn()
            window.destroy()
        }
    }
    function test_robinAppletUsesSameWindowAndReturnsHome() {
        const window = createLauncher()
        window.robinHoodAvailable = true // Exercise the mocked applet on any host.
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
