#include <QtQuickTest/quicktest.h>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)

// No production backends are linked: these objects cannot access a brokerage or keychain.
class QmlTestSetup : public QObject
{
    Q_OBJECT

    static QObject *mock(QQmlEngine *engine, const QByteArray &body)
    {
        QQmlComponent component(engine);
        component.setData("import QtQml\nQtObject {\n" + body + "\n}", QUrl());
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
    }
};

QUICK_TEST_MAIN_WITH_SETUP(publics_qml, QmlTestSetup)
#include "main.moc"
