#include <QtQuickTest/quicktest.h>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>

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
        context->setContextProperty("RobinSessions", mock(engine, R"(
            property bool busy: false
            property bool approvalPending: false
            property int approvalSecondsRemaining: 0
            property bool sessionActive: false
            property string activeAccount: ""
            property var accounts: []
            property string error: ""
            property int lastStarted: -1
            signal sessionSaved(int index)
            signal reauthenticationRequested(string accountName)
            function startSession(index) { lastStarted = index; return true }
            function reauthenticate(index) {
                for (let i = 0; i < count; ++i) {
                    if (get(i).sessionIndex === index) {
                        const name = get(i).accountName
                        remove(i)
                        reauthenticationRequested(name)
                        return true
                    }
                }
                return false
            }
            function signIn(email, password, code) { return true }
            function cancelSignIn() { approvalPending = false; approvalSecondsRemaining = 0; busy = false }
            function clearActiveSession() { sessionActive = false }
        )", "ListModel"));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(publics_qml, QmlTestSetup)
#include "main.moc"
