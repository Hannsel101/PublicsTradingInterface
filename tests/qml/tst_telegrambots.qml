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
    Component {
        id: delayedStopController
        QtObject {
            property bool busy: false
            property bool hasRunningBots: true
            property var bots: []
            property string selectedId: ""
            property string error: ""
            property bool canStart: false
            property bool canStop: true
            property bool canDelete: false
            signal missingChatIds(string id)
            signal passwordRequested(string id)
            signal passwordSetupRequired(string id)
            property int stopCalls: 0
            signal inspectorPortConflict(string botId)
            signal botSaved(string id)
            function clearError() { error = "" }
            function stopAll() { stopCalls++; return false }
            function finishStop() { hasRunningBots = false }
        }
    }

    Component {
        id: inspectorController
        QtObject {
            property bool busy: false
            property bool hasRunningBots: true
            property var bots: [{id: "one", nickname: "First", starting: true, active: false}]
            property string selectedId: "one"
            property string error: ""
            property bool canStart: false
            property bool canStop: true
            property bool canDelete: false
            property string lastId: ""
            property string lastAction: ""
            property int choices: 0
            signal botSaved(string id)
            signal missingChatIds(string id)
            signal passwordRequested(string id)
            signal passwordSetupRequired(string id)
            signal inspectorPortConflict(string botId)
            function clearError() { error = "" }
            function resolveInspectorConflict(id, action) {
                if (action === "restart" && busy) return false
                lastId = id; lastAction = action; choices++
                if (action === "cancel") {
                    bots = bots.filter(row => row.id !== id)
                    hasRunningBots = bots.some(row => row.starting || row.active)
                }
                return true
            }
        }
    }
    function test_inspectorConflictActions_data() {
        const rows = []
        for (const size of test_actionsFollowManageContainer_data())
            for (const action of ["restart", "cancel", "escape"])
                rows.push({tag: size.tag + "-" + action, width: size.width, height: size.height, action: action})
        return rows
    }
    function test_inspectorConflictActions(data) {
        const view = openConsole(data.width, data.height)
        const controller = createTemporaryObject(inspectorController, testCase)
        view.controller = controller
        controller.error = "Telegram network or service request failed."
        compare(findChild(view, "inspectorConflictDialog"), null)
        controller.inspectorPortConflict("one")
        tryVerify(() => findChild(view, "inspectorConflictDialog") !== null)
        const dialog = findChild(view, "inspectorConflictDialog")
        tryCompare(dialog, "visible", true)
        compare(findChild(dialog, "inspectorConflictMessage").text,
            "Ngrok inspector port is already in use! Close the inspector port and try again?")
        compare(dialog.palette.window, "#17181c")
        const ownership = findChild(dialog, "inspectorOwnershipNotice")
        const risk = findChild(dialog, "inspectorMacSignalNotice")
        verify(ownership !== null && risk !== null)
        verify(ownership.text.indexOf("app-recorded orphan") >= 0)
        compare(risk.visible, Qt.platform.os === "osx")
        if (risk.visible) verify(risk.text.indexOf("Confirm accepts") >= 0)
        const confirm = findChild(dialog, "confirmInspectorConflict")
        const cancel = findChild(dialog, "cancelInspectorConflict")
        compare(confirm.text, "Confirm"); compare(cancel.text, "Cancel")
        verify(waitForPolish(appWindow))
        for (const button of [confirm, cancel]) {
            const point = button.mapToItem(appWindow.contentItem, 0, 0)
            verify(point.x >= 0 && point.y >= 0)
            verify(point.x + button.width <= appWindow.width && point.y + button.height <= appWindow.height)
        }
        controller.inspectorPortConflict("one")
        controller.selectedId = "two"
        controller.busy = true
        compare(confirm.enabled, false)
        compare(cancel.enabled, true)
        if (data.action === "restart") controller.busy = false
        if (data.action === "escape") { dialog.forceActiveFocus(); keyClick(Qt.Key_Escape) }
        else mouseClick(data.action === "restart" ? confirm : cancel)
        tryCompare(controller, "choices", 1)
        compare(controller.lastId, "one")
        compare(controller.lastAction, data.action === "restart" ? "restart" : "cancel")
        tryVerify(() => findChild(view, "inspectorConflictDialog") === null)
        compare(controller.choices, 1)
        // Failed recovery followed by a fresh native conflict must open a new choice.
        controller.error = "Inspector recovery failed."
        controller.busy = false
        controller.bots = [{id: "one", nickname: "First", starting: true, active: false}]
        controller.hasRunningBots = true
        controller.inspectorPortConflict("one")
        tryVerify(() => findChild(view, "inspectorConflictDialog") !== null)
        mouseClick(findChild(view, "cancelInspectorConflict"))
        tryCompare(controller, "choices", 2)
        tryVerify(() => findChild(view, "inspectorConflictDialog") === null)
    }

    function test_inspectorConflictQueueDiscardsStoppedBots_data() { return test_actionsFollowManageContainer_data() }
    function test_inspectorConflictQueueDiscardsStoppedBots(data) {
        const view = openConsole(data.width, data.height)
        const controller = createTemporaryObject(inspectorController, testCase)
        controller.bots = [
            {id: "one", nickname: "First", starting: true, active: false},
            {id: "two", nickname: "Second", starting: true, active: false},
            {id: "three", nickname: "Third", starting: true, active: false}]
        view.controller = controller
        for (const id of ["one", "two", "two", "missing", "three"]) controller.inspectorPortConflict(id)
        tryVerify(() => findChild(view, "inspectorConflictDialog") !== null)
        compare(findChild(view, "inspectorConflictDialog").botId, "one")
        mouseClick(findChild(view, "cancelInspectorConflict"))
        tryVerify(() => {
            const dialog = findChild(view, "inspectorConflictDialog")
            return dialog !== null && dialog.botId === "two" && dialog.visible
        })
        compare(controller.choices, 1)
        controller.bots = controller.bots.filter(row => row.id !== "two")
        tryVerify(() => {
            const dialog = findChild(view, "inspectorConflictDialog")
            return dialog !== null && dialog.botId === "three" && dialog.visible
        })
        compare(controller.choices, 1)
        mouseClick(findChild(view, "cancelInspectorConflict"))
        tryCompare(controller, "choices", 2)
        compare(controller.lastId, "three")
        tryVerify(() => findChild(view, "inspectorConflictDialog") === null)
        compare(controller.bots.length, 0)
    }
    function test_exitWaitsForAsynchronousTunnelShutdown() {
        const view = openConsole()
        const controller = createTemporaryObject(delayedStopController, testCase)
        verify(controller !== null)
        view.controller = controller
        view.requestLeave()
        let dialog = findChild(view, "stopBotsExitDialog")
        verify(dialog !== null)
        tryCompare(dialog, "visible", true)
        mouseClick(findChild(dialog, "confirmStopBotsExit"))
        tryCompare(controller, "stopCalls", 1)
        compare(Launcher.activeAppletId, "telegram_bots")
        tryVerify(() => {
            const pending = findChild(view, "stopBotsExitDialog")
            return pending !== null && pending.visible
        }, 1000)
        dialog = findChild(view, "stopBotsExitDialog")
        compare(findChild(dialog, "confirmStopBotsExit").enabled, false)
        compare(findChild(dialog, "cancelStopBotsExit").enabled, false)
        controller.finishStop()
        tryCompare(Launcher, "activeAppletId", "")
        compare(controller.stopCalls, 1)
    }

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
    function fillPasswords(view) {
        findChild(view, "botPasswordInput").text = "  test password 123  "
        findChild(view, "botPasswordConfirmationInput").text = "  test password 123  "
    }
    function verifySecurityDialog(view, name, tag) {
        const dialog = findChild(view, name)
        verify(dialog !== null)
        tryCompare(dialog, "visible", true)
        verify(dialog.width <= view.width && dialog.height <= view.height)
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(appWindow.contentItem).save(qmlTestArtifactDirectory + "/" + name + "-" + tag + ".png")
    }
    function submitStartPassword(view) {
        tryVerify(() => findChild(view, "startBotPasswordInput") !== null)
        findChild(view, "startBotPasswordInput").text = "  test password 123  "
        findChild(view, "confirmStartBotPassword").clicked()
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
        RobinTrades.typeText(token, "123456789:synthetic")
        fillPasswords(view)
        tryCompare(save, "enabled", true)
        TelegramBots.holdSave = true
        save.clicked()
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
        TelegramBots.bots = [{id: "one", nickname: "Desk <Bot>", active: false, starting: false, passwordProtected: true, hasAllowedChatIds: true},
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
        submitStartPassword(view)
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

    function test_allowedChatIdsVisibleValidatedAndSaved() {
        const view = openConsole()
        mouseClick(findChild(view, "addNewBot"))
        tryVerify(() => findChild(view, "botTokenInput") !== null)
        findChild(view, "botNicknameInput").text = "Policy bot"
        findChild(view, "botTokenInput").text = "123:synthetic"
        const input = findChild(view, "allowedTelegramChatIdsInput")
        verify(input !== null, "Allowed Telegram Chat IDs editor is visible")
        compare(input.Accessible.name, "Allowed Telegram Chat IDs")
        const save = findChild(view, "saveBot")
        for (const invalid of ["0", "-42", "4503599627370496", "9223372036854775807", "42,4503599627370496", "9223372036854775808", "42,,43", "42,", "４２"]) {
            input.text = invalid
            compare(save.enabled, false, invalid)
            const validation = findChild(view, "chatPolicyValidationError")
            verify(validation !== null && validation.visible)
            verify(validation.text.indexOf("4503599627370495") >= 0)
            compare(TelegramBots.saveCalls, 0)
        }
        fillPasswords(view)
        input.text = "42,\n1234567890123,4503599627370495"
        compare(save.enabled, true)
        // Invoke the form action without depending on scroll position.
        save.clicked()
        tryCompare(TelegramBots, "saveCalls", 1)
        compare(TelegramBots.pendingPolicy.join(","), "42,1234567890123,4503599627370495")
        compare(TelegramBots.bots[0].allowedChatIds.length, 0)
    }
    function test_editRestoresSavedPolicyWithoutExposingToken() {
        TelegramBots.bots = [{id: "one", nickname: "Desk", active: false, starting: false, allowedChatIds: ["42", "123"]}]
        TelegramBots.selectBot("one")
        const view = openConsole()
        const edit = findChild(view, "editBotPolicy")
        verify(edit !== null)
        mouseClick(edit)
        tryVerify(() => findChild(view, "allowedTelegramChatIdsInput") !== null)
        compare(findChild(view, "allowedTelegramChatIdsInput").text, "42\n123")
        compare(findChild(view, "botTokenInput").text, "")
        findChild(view, "allowedTelegramChatIdsInput").text = ""
        fillPasswords(view)
        compare(findChild(view, "saveBot").enabled, true)
        findChild(view, "saveBot").clicked()
        tryCompare(TelegramBots, "saveCalls", 1)
        compare(TelegramBots.bots.length, 1)
        compare(TelegramBots.bots[0].allowedChatIds.length, 0)
    }
    function test_editorAcceptsNativeTokenShape() {
        const view = openConsole()
        mouseClick(findChild(view, "addNewBot"))
        tryVerify(() => findChild(view, "botTokenInput") !== null)
        findChild(view, "botNicknameInput").text = "Draft"
        findChild(view, "botTokenInput").text = "123456:synthetic"
        fillPasswords(view)
        compare(findChild(view, "saveBot").enabled, true)
        if (qmlTestArtifactDirectory.length > 0)
            grabImage(appWindow.contentItem).save(qmlTestArtifactDirectory + "/telegram-bot-editor.png")
    }

    function test_passwordMismatchRetryAndNoTrimming() {
        const view = openConsole()
        mouseClick(findChild(view, "addNewBot"))
        tryVerify(() => findChild(view, "botPasswordInput") !== null)
        findChild(view, "botNicknameInput").text = "Protected"
        findChild(view, "botTokenInput").text = "123456:synthetic"
        fillPasswords(view)
        const first = findChild(view, "botPasswordInput")
        const second = findChild(view, "botPasswordConfirmationInput")
        compare(first.echoMode, TextInput.Password); compare(second.echoMode, TextInput.Password)
        second.text = "different password 123"
        findChild(view, "saveBot").clicked()
        compare(findChild(view, "botSaveError").text, "Passwords do not match.")
        compare(TelegramBots.saveCalls, 0)
        second.text = first.text
        findChild(view, "saveBot").clicked()
        tryCompare(TelegramBots, "saveCalls", 1)
        compare(TelegramBots.passwordMatched, true)
        compare(TelegramBots.bots[0].hasAllowedChatIds, false)
    }
    function test_missingChatIdShowsEditPopupWithoutPassword_data() { return test_actionsFollowManageContainer_data() }
    function test_missingChatIdShowsEditPopupWithoutPassword(data) {
        TelegramBots.bots = [{id: "one", nickname: "Empty", passwordProtected: true, hasAllowedChatIds: false, active: false, starting: false, stopping: false}]
        TelegramBots.selectBot("one")
        const view = openConsole(data.width, data.height)
        compare(findChild(view, "startBot").enabled, true)
        mouseClick(findChild(view, "startBot"))
        tryVerify(() => findChild(view, "missingChatIdDialog") !== null)
        verifySecurityDialog(view, "missingChatIdDialog", data.tag)
        compare(TelegramBots.startCalls, 0)
        compare(findChild(view, "startBotPasswordDialog"), null)
        findChild(view, "editMissingChatSettings").clicked()
        tryVerify(() => findChild(view, "unlockBotSettingsPassword") !== null)
        compare(findChild(view, "allowedTelegramChatIdsInput").enabled, false)
    }
    function test_passwordStartCapturedIdRetryAndCancel() {
        TelegramBots.bots = [{id: "one", nickname: "First", passwordProtected: true, hasAllowedChatIds: true, active: false, starting: false, stopping: false},
            {id: "two", nickname: "Second", passwordProtected: true, hasAllowedChatIds: true, active: false, starting: false, stopping: false}]
        TelegramBots.selectBot("one")
        const view = openConsole()
        mouseClick(findChild(view, "startBot"))
        tryVerify(() => findChild(view, "startBotPasswordDialog") !== null)
        TelegramBots.selectBot("two")
        TelegramBots.failPassword = true
        submitStartPassword(view)
        compare(TelegramBots.lastStartId, "one")
        compare(findChild(view, "startBotPasswordInput").text, "")
        verify(findChild(view, "startBotPasswordDialog").visible)
        TelegramBots.failPassword = false
        submitStartPassword(view)
        tryVerify(() => findChild(view, "startBotPasswordDialog") === null)
        compare(TelegramBots.passwordMatched, true)
        compare(TelegramBots.bots[0].active, true)
        mouseClick(findChild(view, "startBot"))
        tryVerify(() => findChild(view, "startBotPasswordInput") !== null)
        compare(findChild(view, "startBotPasswordInput").text, "")
        findChild(view, "cancelBotSecurityDialog").clicked()
        tryVerify(() => findChild(view, "startBotPasswordDialog") === null)
        compare(TelegramBots.startCalls, 2)
    }
    function test_managementActionsShareRow_data() { return test_actionsFollowManageContainer_data() }
    function test_managementActionsShareRow(data) {
        TelegramBots.bots = [{id: "one", nickname: "First", active: false, starting: false, stopping: false}]
        TelegramBots.selectBot("one")
        const view = openConsole(data.width, data.height)
        const remove = findChild(view, "deleteBot")
        const edit = findChild(view, "editBotPolicy")
        const title = findChild(view, "telegramManageTitle")
        const list = findChild(view, "telegramBotList")
        verify(remove !== null && edit !== null && title !== null && list !== null)
        verify(waitForPolish(appWindow))
        const left = remove.mapToItem(appWindow.contentItem, 0, remove.height / 2)
        const right = edit.mapToItem(appWindow.contentItem, 0, edit.height / 2)
        verify(Math.abs(left.y - right.y) <= 1)
        verify(left.x + remove.width <= right.x)
        verify(left.x >= 0 && right.x + edit.width <= appWindow.width)
        verify(title.contentWidth <= title.width + 1)
        const heading = title.mapToItem(appWindow.contentItem, 0, title.height)
        const listTop = list.parent.mapToItem(appWindow.contentItem, 0, 0).y
        const actionsBottom = Math.max(left.y + remove.height / 2, right.y + edit.height / 2, heading.y)
        verify(listTop - actionsBottom >= 0 && listTop - actionsBottom <= 19)
        compare(remove.enabled, true)
        compare(edit.enabled, true)
    }
    function test_deleteCancelFailureCommit_data() { return test_actionsFollowManageContainer_data() }
    function test_deleteCancelFailureCommit(data) {
        TelegramBots.bots = [{id: "one", nickname: "First", active: false, starting: false, stopping: false}]
        TelegramBots.selectBot("one")
        const view = openConsole(data.width, data.height)
        mouseClick(findChild(view, "deleteBot"))
        tryVerify(() => findChild(view, "deleteBotDialog") !== null)
        verifySecurityDialog(view, "deleteBotDialog", data.tag)
        findChild(view, "cancelBotSecurityDialog").clicked()
        tryVerify(() => findChild(view, "deleteBotDialog") === null)
        compare(TelegramBots.deleteCalls, 0)
        mouseClick(findChild(view, "deleteBot"))
        tryVerify(() => findChild(view, "confirmDeleteBot") !== null)
        TelegramBots.holdDelete = true
        findChild(view, "confirmDeleteBot").clicked()
        compare(TelegramBots.bots.length, 1)
        compare(findChild(view, "confirmDeleteBot").enabled, false)
        TelegramBots.finishDelete(false)
        compare(TelegramBots.bots.length, 1)
        findChild(view, "confirmDeleteBot").clicked()
        TelegramBots.finishDelete(true)
        tryCompare(TelegramBots, "bots", [])
        tryVerify(() => findChild(view, "deleteBotDialog") === null)
    }
    function test_protectedEditRequiresUnlockAndExplicitPassword() {
        TelegramBots.bots = [{id: "one", nickname: "Locked", passwordProtected: true, hasAllowedChatIds: true, active: false, starting: false, stopping: false, allowedChatIds: []}]
        TelegramBots.selectBot("one")
        const view = openConsole()
        mouseClick(findChild(view, "editBotPolicy"))
        tryVerify(() => findChild(view, "unlockBotSettingsPassword") !== null)
        compare(findChild(view, "saveBot").enabled, false)
        compare(findChild(view, "allowedTelegramChatIdsInput").enabled, false)
        findChild(view, "unlockBotSettingsPassword").text = "wrong password"
        TelegramBots.failPassword = true
        findChild(view, "unlockBotSettings").clicked()
        compare(findChild(view, "allowedTelegramChatIdsInput").enabled, false)
        compare(findChild(view, "unlockBotSettingsPassword").text, "")
        TelegramBots.failPassword = false
        findChild(view, "unlockBotSettingsPassword").text = "test password 123"
        findChild(view, "unlockBotSettings").clicked()
        compare(findChild(view, "allowedTelegramChatIdsInput").text, "42\n123")
        compare(findChild(view, "saveBot").enabled, false)
        fillPasswords(view)
        findChild(view, "saveBot").clicked()
        tryCompare(TelegramBots, "saveCalls", 1)
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

    function test_passwordVaultNoticeRemoved_data() {
        return test_actionsFollowManageContainer_data()
    }
    function test_passwordVaultNoticeRemoved(data) {
        const view = openConsole(data.width, data.height)
        compare(findChild(view, "telegramVaultNotice"), null)
        verify(findChild(view, "startBot") !== null)
        const stop = findChild(view, "stopBot")
        const error = findChild(view, "telegramRuntimeError")
        const page = findChild(view, "telegramPageScroller")
        verify(stop !== null && error !== null && page !== null)
        compare(error.textFormat, Text.PlainText)
        verify(waitForPolish(appWindow))
        tryVerify(() => Math.abs(page.contentHeight
            - stop.mapToItem(page.contentItem, 0, stop.height).y - view.pageMargin) <= 1)
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
