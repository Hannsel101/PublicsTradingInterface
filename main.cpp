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
    if (CredReadW(L"PublicsApiKey0", CRED_TYPE_GENERIC, 0, &pCred))
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

// Retrieve a list of target names that start with "ApiKey"
QStringList listStoredApiKeys()
{
    QStringList matchingKeys;
    PCREDENTIALW *pCreds = nullptr;
    DWORD count = 0;

    // "ApiKey*" acts as a wildcard filter for Windows Credential Manager
    LPCWSTR filter = L"PublicsApiKey*";

    // Enumerate only generic credentials matching the filter
    if (CredEnumerateW(filter, 0, &count, &pCreds) && pCreds) {
        for (DWORD i = 0; i < count; ++i) {
            if (pCreds[i]->Type == CRED_TYPE_GENERIC && pCreds[i]->TargetName) {
                matchingKeys.append(QString::fromWCharArray(
                    reinterpret_cast<wchar_t*>(pCreds[i]->CredentialBlob),
                    pCreds[i]->CredentialBlobSize / sizeof(wchar_t)
                    ));
            }
        }
        CredFree(pCreds);
    }
    return matchingKeys;
}

/*
 * Main entry point into the application
 * */
int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    /*
     * Test Pulling in stored api keys
     * */
    qDebug() << listStoredApiKeys();

    /*
     * Grab an initial set of Public Brokerage Api Keys from Windows Credential Manager
     *
     * these will persist between runs unless removed by the user
     * */
    QString mySecretKey = getPublicsApiKey();
    PublicAuthClient authClient(mySecretKey);
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
    //engine.loadFromModule("PublicsTradingInterface", "Main");
    engine.load(QUrl("qrc:/Main.qml"));

    return QGuiApplication::exec();
}
