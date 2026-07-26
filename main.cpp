#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "accountfetcher.h"
#include "publicauthclient.h"
#include "publicapiworker.h"
// #include <qtkeychain/keychain.h>

// void saveKeyToSecureStorage(const QString &apiKey) {
//     auto job = new QKeychain::WritePasswordJob("MyAppIdentifier", this);
//     job->setKey("api_key");
//     job->setTextData(apiKey);
//     QObject::connect(job, &QKeychain::Job::finished, this, [job]() {
//         if (job->error()) {
//             qWarning() << "Failed to save API key:" << job->errorString();
//         } else {
//             qDebug() << "API key saved securely.";
//         }
//         job->deleteLater();
//     });
//     job->start();
// }

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    /*
     * Set the initial properties which can be used in both c++ and qml
     * */
    // Perform authentication with publics api using your secret key
    // TO DO: use qtkeychain to ensure secure storage of keys
    QString mySecretKey = "gtJwUKPxkMR8cnpR3k54474rvqHymfo9"; // From your Publics account settings
    PublicAuthClient authClient(mySecretKey);
    AccountFetcher fetcher("");
    PublicApiWorker newApiWorker;


    // Connect the token received signal to processing the token
    QObject::connect(&authClient, &PublicAuthClient::tokenReceived,
                     &newApiWorker, &PublicApiWorker::processToken);


    engine.rootContext()->setContextProperty("AuthClient", &authClient);
    engine.rootContext()->setContextProperty("ApiWorker", &newApiWorker);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("PublicsTradingInterface", "Main");

    return QGuiApplication::exec();
}
