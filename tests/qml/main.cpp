#include <QtQuickTest/quicktest.h>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QCoreApplication>

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)
Q_IMPORT_QML_PLUGIN(RobinHoodInterfacePlugin)
Q_IMPORT_QML_PLUGIN(TradingLauncherPlugin)
#ifdef PUBLICS_HAS_TELEGRAM_BOTS
Q_IMPORT_QML_PLUGIN(TelegramBotsInterfacePlugin)
#endif

// No production backends are linked: these objects cannot access a brokerage or keychain.
class MockRobinTrades final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available MEMBER available NOTIFY availableChanged)
    Q_PROPERTY(QString unavailableReason MEMBER unavailableReason NOTIFY unavailableReasonChanged)
    Q_PROPERTY(QVariantList tradeResults MEMBER tradeResults NOTIFY tradeResultsChanged)
    Q_PROPERTY(QString currentTransactionTitle MEMBER currentTransactionTitle NOTIFY currentTransactionTitleChanged)
    Q_PROPERTY(bool tradeResultsVisible MEMBER tradeResultsVisible NOTIFY tradeResultsVisibleChanged)
    Q_PROPERTY(bool tradeResultsComplete MEMBER tradeResultsComplete NOTIFY tradeResultsCompleteChanged)
    Q_PROPERTY(bool tradeResultsBusy MEMBER tradeResultsBusy NOTIFY tradeResultsBusyChanged)
    Q_PROPERTY(bool liveOrdersEnabled MEMBER liveOrdersEnabled NOTIFY liveOrdersEnabledChanged)
    Q_PROPERTY(QString error MEMBER error NOTIFY errorChanged)
    Q_PROPERTY(QString lastInput MEMBER lastInput NOTIFY lastInputChanged)
    Q_PROPERTY(QString lastSide MEMBER lastSide NOTIFY lastSideChanged)
    Q_PROPERTY(int submitCalls MEMBER submitCalls NOTIFY submitCallsChanged)
    Q_PROPERTY(int cancelCalls MEMBER cancelCalls NOTIFY cancelCallsChanged)
    Q_PROPERTY(int dismissCalls MEMBER dismissCalls NOTIFY dismissCallsChanged)
public:
    bool available{};
    QString unavailableReason{};
    QVariantList tradeResults{};
    QString currentTransactionTitle{};
    bool tradeResultsVisible{};
    bool tradeResultsComplete{};
    bool tradeResultsBusy{};
    bool liveOrdersEnabled{};
    QString error{};
    QString lastInput{};
    QString lastSide{};
    int submitCalls{};
    int cancelCalls{};
    int dismissCalls{};
signals:
    void availableChanged();
    void unavailableReasonChanged();
    void tradeResultsChanged();
    void currentTransactionTitleChanged();
    void tradeResultsVisibleChanged();
    void tradeResultsCompleteChanged();
    void tradeResultsBusyChanged();
    void liveOrdersEnabledChanged();
    void errorChanged();
    void lastInputChanged();
    void lastSideChanged();
    void submitCallsChanged();
    void cancelCallsChanged();
    void dismissCallsChanged();
public:
    using QObject::QObject;
    Q_INVOKABLE void typeText(QObject *target, const QString &text) {
        QInputMethodEvent event;
        event.setCommitString(text);
        QCoreApplication::sendEvent(target, &event);
    }
    Q_INVOKABLE void reset() {
        setProperty("available", true);
        setProperty("unavailableReason", QString());
        setProperty("tradeResults", QVariantList());
        setProperty("currentTransactionTitle", QString());
        setProperty("tradeResultsVisible", false);
        setProperty("tradeResultsComplete", true);
        setProperty("tradeResultsBusy", false);
        setProperty("liveOrdersEnabled", false);
        setProperty("error", QString());
        setProperty("lastInput", QString());
        setProperty("lastSide", QString());
        setProperty("submitCalls", 0);
        setProperty("cancelCalls", 0);
        setProperty("dismissCalls", 0);
    }
    Q_INVOKABLE bool submit(const QString &input, const QString &side) {
        if (!available || tradeResultsBusy) return false;
        setProperty("lastInput", input);
        setProperty("lastSide", side);
        setProperty("submitCalls", submitCalls + 1);
        setProperty("currentTransactionTitle", side == "buy" ? "Buy 1 each" : "Sell All");
        setProperty("tradeResultsComplete", false);
        setProperty("tradeResultsBusy", true);
        setProperty("tradeResultsVisible", true);
        return true;
    }
    Q_INVOKABLE void cancel() {
        setProperty("cancelCalls", cancelCalls + 1);
        setProperty("tradeResultsBusy", false);
        setProperty("tradeResultsComplete", true);
    }
    Q_INVOKABLE void dismissTradeResults() {
        if (tradeResultsBusy || !tradeResultsComplete) return;
        setProperty("dismissCalls", dismissCalls + 1);
        setProperty("tradeResultsVisible", false);
    }
};
class QmlTestSetup : public QObject
{
    Q_OBJECT

    static QObject *mock(QQmlEngine *engine, const QByteArray &body,
                         const QByteArray &type = "QtObject")
    {
        QQmlComponent component(engine);
        component.setData("import QtQml\nimport QtQml.Models\n" + type + " {\n" + body + "\n}", QUrl());
        QObject *object = component.create();
        if (!object)
            qFatal("Cannot create test facade: %s", qPrintable(component.errorString()));
        object->setParent(engine);
        return object;
    }

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        auto *context = engine->rootContext();
        auto *trades = new MockRobinTrades(engine);
        trades->reset();
        context->setContextProperty("RobinTrades", trades);
        context->setContextProperty("isDebugMode", true);
        context->setContextProperty("qmlTestArtifactDirectory", qEnvironmentVariable("QML_TEST_ARTIFACT_DIR"));
        context->setContextProperty("TelegramBots", mock(engine, R"(
            property var bots: []
            property string selectedId: ""
            property bool busy: false
            property string error: ""
            property bool holdSave: false
            property string pendingNickname: ""
            property var pendingPolicy: []
            property int saveCalls: 0
            property int startCalls: 0
            property int stopAllCalls: 0
            readonly property bool hasRunningBots: bots.some(row => row.active || row.starting || row.stopping)
            readonly property var selected: bots.find(row => row.id === selectedId)
            readonly property bool canStart: !busy && !!selected && !selected.active && !selected.starting && !selected.stopping
            readonly property bool canStop: !busy && !!selected && !selected.stopping && (selected.active || selected.starting)
            signal botSaved(string id)
            signal botDeleted(string id)
            signal missingChatIds(string id)
            signal passwordRequested(string id)
            signal passwordSetupRequired(string id)
            signal settingsUnlocked(string id, var ids)
            signal settingsUnlockFinished(string id, bool success)
            signal activationFinished(string id, bool success)
            signal inspectorPortConflict(string botId)
            property string lastStartId: ""
            property bool passwordMatched: false
            property bool failPassword: false
            property bool holdDelete: false
            property string pendingDeleteId: ""
            property int deleteCalls: 0
            readonly property bool canDelete: canStart
            function reset() {
                bots = []; selectedId = ""; busy = false; error = ""; holdSave = false
                pendingNickname = ""; saveCalls = 0; startCalls = 0; stopAllCalls = 0
                lastStartId = ""; passwordMatched = false; failPassword = false; holdDelete = false; pendingDeleteId = ""; deleteCalls = 0
            }
            function clearError() { error = "" }
            function selectBot(id) { selectedId = bots.some(row => row.id === id) ? id : "" }
            function saveBot(nickname, token) {
                if (busy || !nickname.trim() || !token) return false
                saveCalls++; pendingNickname = nickname.trim(); busy = true
                if (!holdSave) finishSave(true)
                return true
            }
            readonly property string maximumAllowedChatId: "4503599627370495"
            function lockBotSettings() {}
            function unlockBotSettings(id, password) {
                if (busy) return false
                if (failPassword) { error = "Unable to unlock bot. Check your password and try again."; settingsUnlockFinished(id, false); return true }
                settingsUnlocked(id, ["42", "123"]); settingsUnlockFinished(id, true); return true
            }
            function saveProtectedBot(id, nickname, token, text, password, confirmation) {
                if (password !== confirmation) { error = "Passwords do not match."; return false }
                if (password.length < 12) return false
                passwordMatched = password === "  test password 123  "
                return saveBotWithAllowedChats(nickname, token, text)
            }
            function deleteBot(id) {
                if (busy || !bots.some(row => row.id === id && !row.active && !row.starting && !row.stopping)) return false
                deleteCalls++; pendingDeleteId = id; busy = true
                if (!holdDelete) finishDelete(true)
                return true
            }
            function finishDelete(ok) {
                if (!pendingDeleteId) return
                const id = pendingDeleteId; pendingDeleteId = ""; busy = false
                if (!ok) { error = "Could not delete the encrypted bot. Please retry."; return }
                bots = bots.filter(row => row.id !== id)
                if (selectedId === id) selectedId = ""
                botDeleted(id)
            }
            function saveBotWithAllowedChats(nickname, token, text) {
                if (busy || !nickname.trim()) return false
                const policy = text.split(/[,\r\n]/).map(id => id.trim()).filter(id => id.length > 0)
                if (text.length > 32768 || ((/(^|,)\s*(,|$)/.test(text)) && text.trim().length > 0)
                    || policy.length > 1024 || !policy.every(id => /^[1-9][0-9]{0,15}$/.test(id)
                        && (id.length < maximumAllowedChatId.length || (id.length === maximumAllowedChatId.length && id <= maximumAllowedChatId)))) {
                    error = "Private Chat IDs must be 1..4503599627370495."; return false
                }
                pendingPolicy = [...new Set(policy)]
                saveCalls++; pendingNickname = nickname; busy = true
                if (!holdSave) finishSave(true)
                return true
            }
            function finishSave(ok) {
                if (!busy) return
                busy = false
                if (!ok) { error = "Secure storage failed."; return }
                const existing = bots.find(row => row.nickname === pendingNickname)
                const id = existing ? existing.id : "bot-" + saveCalls
                const row = {id: id, nickname: pendingNickname, active: false, starting: false, stopping: false, allowedChatIds: [], passwordProtected: true, hasAllowedChatIds: pendingPolicy.length > 0}
                bots = existing ? bots.map(old => old.id === id ? row : old) : bots.concat([row])
                pendingNickname = ""; botSaved(id)
            }
            function startBot() {
                if (!canStart) return false
                if (!selected.hasAllowedChatIds && !(selected.allowedChatIds && selected.allowedChatIds.length)) { missingChatIds(selectedId); return false }
                if (!selected.passwordProtected) { passwordSetupRequired(selectedId); return false }
                passwordRequested(selectedId); return true
            }
            function startBotWithPassword(id, password) {
                if (busy) return false
                startCalls++; lastStartId = id
                passwordMatched = password === "  test password 123  "
                if (failPassword) { error = "Unable to unlock bot. Check your password and try again."; activationFinished(id, false); return true }
                error = ""
                bots = bots.map(row => Object.assign({}, row, {active: row.id === id ? true : row.active, starting: false}))
                activationFinished(id, true); return true
            }
            function stopBot() {
                if (!canStop) return false
                bots = bots.map(row => Object.assign({}, row, {active: row.id === selectedId ? false : row.active, starting: false}))
                return true
            }
            function stopAll() {
                if (busy) return false
                stopAllCalls++
                bots = bots.map(row => Object.assign({}, row, {active: false, starting: false}))
                return true
            }
        )"));
        context->setContextProperty("Launcher", mock(engine, R"(
            property string activeAppletId: ""
            property bool canReturnHome: activeAppletId !== "" && (activeAppletId !== "telegram_bots"
                                        || (!TelegramBots.busy && !TelegramBots.hasRunningBots))
            function activate(id) { activeAppletId = id; return true }
            function returnHome() { activeAppletId = ""; return true }
        )"));
        context->setContextProperty("AuthClient", mock(engine, R"(
            property bool sessionActive: false
            property string selectedApiKeyLabel: ""
            property string secretKey: ""
            property var secretKeys: []
            property var apiKeyNicknames: ({})
            property bool apiKeyReady: false
            property bool apiKeyLoading: false
            property bool apiKeyIndexLoading: false
            property string apiKeyError: ""
            signal apiKeyRemovalFinished(string label, bool succeeded, string message)
            function clearUserSession() { sessionActive = false }
            function requestToken() {}
            function storeNextApiKey(user, key) { return false }
            function removeStoredApiKey(label) { return false }
            function setApiKeyNickname(label, nickname) { return false }
        )"));
        const QByteArray results = R"(
            property var tradeResults: []
            property string currentTransactionTitle: ""
            property bool tradeResultsVisible: false
            property bool tradeResultsComplete: true
            property bool tradeResultsBusy: false
            function dismissTradeResults() { tradeResultsVisible = false }
        )";
        context->setContextProperty("ApiWorker", mock(engine, results + R"(
            function clearSubAccountsList() { tradeResultsVisible = false }
            function executePreflight(symbols, side) {}
            function executeTrade(symbols, side) {}
        )"));
        context->setContextProperty("AutoTrader", mock(engine, results + R"(
            property var keyLabels: []
            function submit(symbols, side) {}
        )"));
        QObject *browser = mock(engine, R"(
            property bool available: true
            property bool active: false
            property string backend: "test"
            property url viewSource
            property url startUrl: "about:blank"
            property url loginUrl
            property string error: ""
            function attachProfile(profile) {}
            function detachProfile(profile) {}
            function navigationChanged(url) {}
            function isAllowedNavigation(url) { return false }
            function pageFailed() { error = "Page failed" }
        )");
        browser->setProperty("viewSource", QUrl::fromLocalFile(
            QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/MockWebLogin.qml"));
        browser->setProperty("loginUrl", QUrl(QStringLiteral("https://robinhood.com/")));
        context->setContextProperty("RobinBrowser", browser);
        QObject *sessions = mock(engine, R"(
            property bool busy: false
            property bool approvalPending: false
            property int approvalSecondsRemaining: 0
            property bool sessionActive: false
            property string activeAccount: ""
            property var accounts: null
            property var mockCachedAccounts: ({})
            signal accountCacheChanged()
            function cachedAccounts(index) { return mockCachedAccounts[index] || [] }
            property string error: ""
            property int lastStarted: -1
            property bool reauthenticating: false
            property string lastPreparedAccountId: ""
            property int prepareCalls: 0
            property int reauthenticateCalls: 0
            property int cancelCalls: 0
            signal sessionSaved(int index)
            signal sessionDeleted(int index)
            signal reauthenticationRequested(string accountName)
            function startSession(index) { lastStarted = index; return true }
            property bool nativeLoginPending: false
            property bool nativeRequestStarted: false
            readonly property bool nativeLoginWorking: nativeLoginPending && nativeRequestStarted && !nativeCodeRequired
            property bool nativeCodeRequired: false
            property bool nativeLoginCancellable: true
            property string nativeLoginStatus: ""
            property string nativeLoginEmail: ""
            property int beginNativeCalls: 0
            property int signInCalls: 0
            property int codeCalls: 0
            property string lastNativeAccountId: ""
            property string lastNativeEmail: ""
            property bool passwordMatched: false
            property bool mfaMatched: false
            property bool codeMatched: false
            property bool acceptSignIn: true
            function beginNativeLogin(accountId = "") {
                if (busy) return false
                ++beginNativeCalls
                nativeRequestStarted = false
                busy = true
                lastNativeAccountId = accountId
                nativeLoginPending = true
                nativeLoginCancellable = true
                nativeCodeRequired = false
                nativeLoginEmail = ""
                nativeLoginStatus = ""
                error = ""
                return true
            }
            function signIn(email, password, mfaCode = "") {
                if (nativeRequestStarted || !nativeLoginPending || !acceptSignIn) return false
                ++signInCalls
                nativeRequestStarted = true
                lastNativeEmail = email
                passwordMatched = password === "  päss+密碼  "
                mfaMatched = mfaCode === "123456"
                error = ""
                busy = true
                nativeLoginStatus = "Approve sign-in in the Robinhood mobile app."
                return true
            }
            function submitNativeVerificationCode(code) {
                if (nativeLoginWorking || !nativeCodeRequired) return
                ++codeCalls
                nativeRequestStarted = true
                codeMatched = code === "654321"
                nativeCodeRequired = false
                busy = true
            }
            property bool browserLoginPending: false
            property int beginBrowserCalls: 0
            property string lastBrowserAccountId: ""
            function beginBrowserLogin(accountId = "") {
                if (!RobinBrowser.available || busy)
                    return false
                ++beginBrowserCalls
                lastBrowserAccountId = accountId
                browserLoginPending = true
                RobinBrowser.active = true
                busy = true
                return true
            }
            function prepareReauthentication(accountId) {
                lastPreparedAccountId = accountId
                ++prepareCalls
                if (beginBrowserLogin(accountId))
                    reauthenticating = true
            }
            function reauthenticate(index) {
                ++reauthenticateCalls
                for (let i = 0; i < count; ++i) {
                    if (get(i).sessionIndex === index) {
                        if (!beginNativeLogin(get(i).accountId)) return false
                        nativeLoginEmail = get(i).accountName
                        reauthenticating = true
                        return true
                    }
                }
                return false
            }
            function deleteSavedSession(index) {
                for (let i = 0; i < count; ++i) {
                    if (get(i).sessionIndex === index) {
                        remove(i)
                        sessionDeleted(index)
                        return true
                    }
                }
                return false
            }
            function cancelSignIn() {
                if (nativeLoginPending) {
                    if (!nativeLoginCancellable) return
                    ++cancelCalls
                    nativeLoginPending = false
                    nativeRequestStarted = false
                    nativeCodeRequired = false
                    nativeLoginEmail = ""
                    nativeLoginStatus = ""
                    reauthenticating = false
                    busy = false
                    return
                }
                if (!browserLoginPending)
                    return
                ++cancelCalls
                browserLoginPending = false
                RobinBrowser.active = false
                approvalPending = false
                approvalSecondsRemaining = 0
                reauthenticating = false
                busy = false
            }
            function clearActiveSession() { sessionActive = false; accounts.clear(); busy = false }
        )", "ListModel");
        sessions->setProperty("accounts", QVariant::fromValue(mock(engine, {}, "ListModel")));
        context->setContextProperty("RobinSessions", sessions);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(publics_qml, QmlTestSetup)
#include "main.moc"
