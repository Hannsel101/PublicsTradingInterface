import QtQuick
import QtQuick.Controls
import QtTest
import TradingLauncher

TestCase {
    id: testCase
    name: "TelegramBotsView"
    visible: true
    when: windowShown
    width: 1000
    height: 700
    property var appWindow: null

    function initTestCase() {
        if (Qt.platform.os !== "windows" && Qt.platform.os !== "osx")
            skip("Telegram Bots is Windows/macOS only")
    }
    function init() {
        TelegramBots.reset()
        Launcher.activeAppletId = ""
        failOnWarning(/ReferenceError|TypeError|is not defined|Cannot assign|Binding loop/)
    }
    function cleanup() {
        if (appWindow) { appWindow.destroy(); appWindow = null }
    }
    function openConsole(width = 1000, height = 700) {
        const component = Qt.createComponent("qrc:/qt/qml/TradingLauncher/LauncherWindow.qml")
        compare(component.status, Component.Ready, component.errorString())
        appWindow = component.createObject(null, {width: width, height: height, visible: true})
        verify(appWindow !== null)
        verify(waitForPolish(appWindow))
        mouseClick(findChild(appWindow.contentItem, "openTelegramBots"))
        const loader = findChild(appWindow.contentItem, "telegramBotsLoader")
        tryVerify(() => loader.status === Loader.Ready && loader.item !== null)
        verify(waitForPolish(appWindow))
        return loader.item
    }
    function test_saveWaitsForSecureCommit_data() { return test_actionsFollowManageContainer_data() }
    function test_saveWaitsForSecureCommit(data) {
        const view = openConsole(data.width, data.height)
        mouseClick(findChild(view, "addNewBot"))
        tryVerify(() => findChild(view, "botNicknameInput") !== null, 1000, "Editor opens")
        const nickname = findChild(view, "botNicknameInput")
        const token = findChild(view, "botTokenInput")
        const save = findChild(view, "saveBot")
        compare(save.enabled, false)
        compare(token.echoMode, TextInput.Password)
        compare(nickname.Accessible.name, "Bot Nickname")
        compare(token.Accessible.name, "Bot API Token")
        nickname.forceActiveFocus()
        RobinTrades.typeText(nickname, "Desk <Bot>")
        compare(save.enabled, false)
        token.forceActiveFocus()
        RobinTrades.typeText(token, "123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijk")
        tryCompare(save, "enabled", true)
        TelegramBots.holdSave = true
        mouseClick(save)
        compare(TelegramBots.saveCalls, 1)
        compare(TelegramBots.busy, true)
        compare(TelegramBots.bots.length, 0)
        compare(token.text, "")
        compare(save.enabled, false)
        verify(findChild(view, "botNicknameInput") !== null)
        TelegramBots.finishSave(true)
        tryVerify(() => findChild(view, "botNicknameInput") === null)
        compare(TelegramBots.bots.length, 1)
        compare(TelegramBots.bots[0].nickname, "Desk <Bot>")
        compare(TelegramBots.bots[0].active, false)
        compare(TelegramBots.bots[0].token, undefined)
        const list = findChild(view, "telegramBotList")
        tryVerify(() => list.itemAtIndex(0) !== null)
        compare(findChild(list.itemAtIndex(0), "botNickname").text, "Desk <Bot>")
        compare(findChild(view, "startBot").enabled, false)
    }

    function test_selectionAndActivityControlState_data() { return test_actionsFollowManageContainer_data() }
    function test_selectionAndActivityControlState(data) {
        TelegramBots.bots = [{id: "one", nickname: "Desk <Bot>", active: false, starting: false},
                             {id: "two", nickname: "Second", active: true, starting: false}]
        const view = openConsole(data.width, data.height)
        const list = findChild(view, "telegramBotList")
        verify(list !== null, "Bot list exists")
        tryVerify(() => list.itemAtIndex(0) !== null)
        const start = findChild(view, "startBot")
        const stop = findChild(view, "stopBot")
        compare(start.enabled, false)
        compare(stop.enabled, false)
        let row = list.itemAtIndex(0)
        const nickname = findChild(row, "botNickname")
        compare(nickname.text, "Desk <Bot>")
        compare(nickname.textFormat, Text.PlainText)
        mouseClick(row)
        tryCompare(start, "enabled", true)
        compare(stop.enabled, false)
        mouseClick(start)
        tryCompare(stop, "enabled", true)
        compare(start.enabled, false)
        tryVerify(() => list.itemAtIndex(0) !== null && list.itemAtIndex(0).record.active)
        row = list.itemAtIndex(0)
        const activeColor = row.background.color
        let badge = findChild(row, "botActivityBadge")
        compare(findChild(badge, "botActivityText").text, "Active")
        compare(badge.motionRunning, true)
        const phase = badge.phase
        tryVerify(() => badge.phase !== phase, 1200, "Activity updates every half second")
        verify(badge.mapToItem(row, badge.width, 0).x <= row.width)
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(appWindow.contentItem).save(qmlTestArtifactDirectory + "/telegram-running-" + data.tag + ".png")
        mouseClick(stop)
        tryCompare(start, "enabled", true)
        compare(stop.enabled, false)
        tryVerify(() => list.itemAtIndex(0) !== null && !list.itemAtIndex(0).record.active)
        row = list.itemAtIndex(0)
        verify(row.background.color !== activeColor)
        badge = findChild(row, "botActivityBadge")
        compare(findChild(badge, "botActivityText").text, "Not Active")
        compare(badge.motionRunning, false)
        const stoppedPhase = badge.phase
        wait(550)
        compare(badge.phase, stoppedPhase)
        list.positionViewAtIndex(1, ListView.Contain)
        tryVerify(() => list.itemAtIndex(1) !== null)
        mouseClick(list.itemAtIndex(1))
        tryCompare(stop, "enabled", true)
        compare(start.enabled, false)
    }

    function test_exitCancelKeepsBotsAndConfirmStopsEveryBot_data() { return test_actionsFollowManageContainer_data() }
    function test_exitCancelKeepsBotsAndConfirmStopsEveryBot(data) {
        TelegramBots.bots = [{id: "one", nickname: "First", active: true, starting: false},
                             {id: "two", nickname: "Second", active: true, starting: false}]
        const view = openConsole(data.width, data.height)
        const back = findChild(appWindow.contentItem, "returnHome")
        tryCompare(back, "enabled", true)
        mouseClick(back)
        let dialog = findChild(view, "stopBotsExitDialog")
        verify(dialog !== null, "Exit confirmation exists")
        tryCompare(dialog, "visible", true)
        compare(dialog.palette.window, "#17181c")
        compare(findChild(dialog, "stopBotsExitMessage").text,
                'In order to leave all bots must be stopped. Press "Confirm" to continue')
        const cancel = findChild(dialog, "cancelStopBotsExit")
        compare(cancel.text, "Cancel")
        mouseClick(cancel)
        tryVerify(() => findChild(view, "stopBotsExitDialog") === null, 1000, "Canceled popup is unloaded")
        compare(Launcher.activeAppletId, "telegram_bots")
        compare(TelegramBots.stopAllCalls, 0)
        compare(TelegramBots.bots.filter(row => row.active).length, 2)
        mouseClick(back)
        dialog = findChild(view, "stopBotsExitDialog")
        verify(dialog !== null)
        tryCompare(dialog, "visible", true)
        const confirm = findChild(dialog, "confirmStopBotsExit")
        compare(confirm.text, "Confirm")
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(appWindow.contentItem).save(qmlTestArtifactDirectory + "/telegram-exit-" + data.tag + ".png")
        mouseClick(confirm)
        tryCompare(Launcher, "activeAppletId", "")
        compare(TelegramBots.stopAllCalls, 1)
        compare(TelegramBots.hasRunningBots, false)
    }

    function test_editorAcceptsNativeTokenShape() {
        const view = openConsole()
        mouseClick(findChild(view, "addNewBot"))
        tryVerify(() => findChild(view, "botTokenInput") !== null)
        findChild(view, "botNicknameInput").text = "Draft"
        findChild(view, "botTokenInput").text = "123456:synthetic"
        compare(findChild(view, "saveBot").enabled, true)
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(appWindow.contentItem).save(qmlTestArtifactDirectory + "/telegram-bot-editor.png")
    }

    function test_runtimeErrorsAreVisiblePlainText() {
        TelegramBots.error = "Fixture error: <b>not markup</b>"
        const view = openConsole()
        const error = findChild(view, "telegramRuntimeError")
        verify(error !== null, "Runtime failures are visible")
        compare(error.visible, true)
        compare(error.text, TelegramBots.error)
        compare(error.textFormat, Text.PlainText)
    }

    function test_actionsFollowManageContainer_data() {
        return [{tag: "desktop", width: 1000, height: 700},
                {tag: "compact", width: 360, height: 620}]
    }
    function test_actionsFollowManageContainer(data) {
        const view = openConsole(data.width, data.height)
        const add = findChild(view, "addNewBot")
        verify(add !== null, "Add action exists")
        const panel = findChild(view, "telegramManagePanel")
        const start = findChild(view, "startBot")
        const stop = findChild(view, "stopBot")
        compare(add.text, "+ Add New Bot")
        compare(start.text, "Start Bot")
        compare(stop.text, "Stop Bot")
        compare(start.enabled, false)
        compare(stop.enabled, false)
        verify(Math.abs(add.width - panel.width) < 1)
        verify(Math.abs(add.mapToItem(view, 0, 0).x - panel.mapToItem(view, 0, 0).x) < 1)
        verify(add.mapToItem(view, 0, 0).y >= panel.mapToItem(view, 0, panel.height).y)
        verify(start.mapToItem(view, 0, 0).y >= add.mapToItem(view, 0, add.height).y)
        verify(start.mapToItem(view, start.width, 0).x < stop.mapToItem(view, 0, 0).x)
        verify(Math.abs(start.width - stop.width) <= 1, "Equal widths within pixel rounding")
        verify(stop.mapToItem(appWindow.contentItem, 0, stop.height).y <= appWindow.height)
    }
}
