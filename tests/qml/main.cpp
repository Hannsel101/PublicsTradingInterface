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
            property int saveCalls: 0
            property int startCalls: 0
            property int stopAllCalls: 0
            readonly property bool hasRunningBots: bots.some(row => row.active || row.starting || row.stopping)
            readonly property var selected: bots.find(row => row.id === selectedId)
            readonly property bool canStart: !busy && !!selected && !selected.active && !selected.starting && !selected.stopping
            readonly property bool canStop: !busy && !!selected && !selected.stopping && (selected.active || selected.starting)
            signal botSaved(string id)
            function reset() {
                bots = []; selectedId = ""; busy = false; error = ""; holdSave = false
                pendingNickname = ""; saveCalls = 0; startCalls = 0; stopAllCalls = 0
            }
            function clearError() { error = "" }
            function selectBot(id) { selectedId = bots.some(row => row.id === id) ? id : "" }
            function saveBot(nickname, token) {
                if (busy || !nickname.trim() || !token) return false
                saveCalls++; pendingNickname = nickname.trim(); busy = true
                if (!holdSave) finishSave(true)
                return true
            }
            function finishSave(ok) {
                if (!busy) return
                busy = false
                if (!ok) { error = "Secure storage failed."; return }
                const id = "bot-" + saveCalls
                bots = bots.concat([{id: id, nickname: pendingNickname, active: false, starting: false}])
                pendingNickname = ""; botSaved(id)
            }
            function startBot() {
                if (!canStart) return false
                startCalls++
                bots = bots.map(row => ({id: row.id, nickname: row.nickname,
                    active: row.id === selectedId ? true : row.active, starting: false}))
                return true
            }
            function stopBot() {
                if (!canStop) return false
                bots = bots.map(row => ({id: row.id, nickname: row.nickname,
                    active: row.id === selectedId ? false : row.active, starting: false}))
                return true
            }
            function stopAll() {
                if (busy) return false
                stopAllCalls++
                bots = bots.map(row => ({id: row.id, nickname: row.nickname, active: false, starting: false}))
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
