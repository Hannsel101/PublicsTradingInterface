#include <QtQuickTest/quicktest.h>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QFileInfo>

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)
Q_IMPORT_QML_PLUGIN(RobinHoodInterfacePlugin)
Q_IMPORT_QML_PLUGIN(TradingLauncherPlugin)

// No production backends are linked: these objects cannot access a brokerage or keychain.
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
        context->setContextProperty("isDebugMode", true);
        context->setContextProperty("Launcher", mock(engine, R"(
            property string activeAppletId: ""
            property bool canReturnHome: activeAppletId !== ""
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
                        prepareReauthentication(get(i).accountId)
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
            function clearActiveSession() { sessionActive = false }
        )", "ListModel");
        sessions->setProperty("accounts", QVariant::fromValue(mock(engine, {}, "ListModel")));
        context->setContextProperty("RobinSessions", sessions);
    }
};

QUICK_TEST_MAIN_WITH_SETUP(publics_qml, QmlTestSetup)
#include "main.moc"
