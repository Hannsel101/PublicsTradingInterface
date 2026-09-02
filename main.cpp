#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "publicauthclient.h"
#include "publicapiworker.h"
#include "stocksearchcontroller.h"

/*
 * Main entry point into the application
 * */
int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#if defined(Q_OS_ANDROID)
    qputenv("ANDROID_OPENSSL_SUFFIX", "_3");
#endif
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    /*
     * Grab an initial set of Public Brokerage Api Key labels from the platform keychain index.
     *
     * these will persist between runs unless removed by the user
     * */
    PublicAuthClient authClient("");
    authClient.listStoredApiKeys();


    PublicApiWorker publicsApiWorker;
    StockSearchController stockSearchController;


    // Connect the token received signal to processing the token
    QObject::connect(&authClient, &PublicAuthClient::tokenReceived,
                     &publicsApiWorker, &PublicApiWorker::processToken);
    QObject::connect(&authClient, &PublicAuthClient::tokenReceived,
                     &stockSearchController, &StockSearchController::storeAuthorizationToken);


    engine.rootContext()->setContextProperty("AuthClient", &authClient);
    engine.rootContext()->setContextProperty("ApiWorker", &publicsApiWorker);
    engine.rootContext()->setContextProperty("StockSearchController", &stockSearchController);

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
