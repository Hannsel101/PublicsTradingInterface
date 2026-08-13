#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "publicauthclient.h"
#include "publicapiworker.h"
#include "stocksearchcontroller.h"
#include <windows.h>
#include <wincred.h>

/*
 * Grabs a publics api key from windows credential manager
 *
 * Name must be setup as PublicsApiKey in order for the key
 * be grabbed
 * */
QString getPublicsApiKey()
{
    PCREDENTIALW pCred = nullptr;

    // Read the secret securely from Windows Credential Manager
    if (CredReadW(L"PublicsApiKey", CRED_TYPE_GENERIC, 0, &pCred))
    {
        // Convert the raw bytes from the credential blob into a QString
        QString key = QString::fromWCharArray(
            reinterpret_cast<wchar_t*>(pCred->CredentialBlob),
            pCred->CredentialBlobSize / sizeof(wchar_t)
            );
        CredFree(pCred);
        return key;
    }

    qWarning() << "Failed to retrieve the API key from Credential Manager.";
    return QString();
}

/*
 * Main entry point into the application
 * */
int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    /*
     * Set the initial properties which can be used in both c++ and qml
     * */
    QString mySecretKey = getPublicsApiKey();
    PublicAuthClient authClient(mySecretKey);
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

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("PublicsTradingInterface", "Main");

    return QGuiApplication::exec();
}
