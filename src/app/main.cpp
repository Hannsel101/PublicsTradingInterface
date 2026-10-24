#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExtensionPlugin>

#include "auth/publicauthclient.h"
#include "api/publicapiworker.h"
#include "trading/autotradecontroller.h"

Q_IMPORT_QML_PLUGIN(PublicsTradingInterfacePlugin)

/*
 * Main entry point into the application
 * */
int main(int argc, char *argv[])
{
#if defined(Q_OS_ANDROID)
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif
    QGuiApplication app(argc, argv);
#if defined(Q_OS_WIN) || (defined(Q_OS_MACOS) && !defined(Q_OS_IOS))
    app.setWindowIcon(QIcon(QStringLiteral(":/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")));
#endif
    QQmlApplicationEngine engine;

    /*
     * Grab an initial set of Public Brokerage Api Key labels from the platform keychain index.
     *
     * these will persist between runs unless removed by the user
     * */
    PublicAuthClient authClient("");
    authClient.listStoredApiKeys();


    PublicApiWorker publicsApiWorker;


    // Connect the token received signal to processing the token
    QObject::connect(&authClient, &PublicAuthClient::tokenReceived,
                     &publicsApiWorker, &PublicApiWorker::processToken);
    QObject::connect(&authClient, &PublicAuthClient::selectedApiKeyLabelChanged,
                     &publicsApiWorker, [&authClient, &publicsApiWorker]() {
        if (authClient.selectedApiKeyLabel().isEmpty()) {
            publicsApiWorker.clearSubAccountsList();
        }
    });

    AutoTradeController autoTrader(&authClient, &publicsApiWorker);


    engine.rootContext()->setContextProperty("AuthClient", &authClient);
    engine.rootContext()->setContextProperty("ApiWorker", &publicsApiWorker);
    engine.rootContext()->setContextProperty("AutoTrader", &autoTrader);

    /*
     * Release compiler flag used primarily so only preflight trades are performed
     * from a debug relase. And actual market trades are performed from a release
     * build
     * */
    #ifdef RELEASE
        engine.rootContext()->setContextProperty("isDebugMode", false);
    #else
        engine.rootContext()->setContextProperty("isDebugMode", true);
    #endif

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("PublicsTradingInterface", "Main");

    return QGuiApplication::exec();
}
