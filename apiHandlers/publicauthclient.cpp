#include "publicauthclient.h"



bool PublicAuthClient::sessionActive() const
{
    return m_sessionActive;
}

void PublicAuthClient::setSessionActive(bool newSessionActive)
{
    if (m_sessionActive == newSessionActive)
        return;
    m_sessionActive = newSessionActive;
    emit sessionActiveChanged();
}

void PublicAuthClient::requestToken()
{
    QUrl url("https://api.public.com/userapiauthservice/personal/access-tokens");
    QNetworkRequest request(url);

    // Set required headers
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // Construct the JSON payload
    QJsonObject jsonObj;
    jsonObj["secret"] = secretKey();
    jsonObj["validityInMinutes"] = 5; // Adjust as needed

    QJsonDocument doc(jsonObj);
    QByteArray data = doc.toJson();

    // Send asynchronous POST request
    QNetworkReply *reply = m_manager->post(request, data);

    // Connect reply finished signal to our slot/lambda
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
            {
                handleReply(reply);
            });
}

QStringList PublicAuthClient::listStoredApiKeys()
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

    // Store the new list of api keys
    setSecretKeys(matchingKeys);
    return secretKeys();
}

bool PublicAuthClient::storeNextApiKey(const QString &userName, const QString &apiKey)
{
    int nextIndex = secretKeys().size();
    QString targetName = QString("PublicsApiKey%1").arg(nextIndex);

    // 1. Convert everything to stable wide strings (UTF-16)
    std::wstring wTargetName = targetName.toStdWString();
    std::wstring wUserName = userName.toStdWString();
    std::wstring wApiKey = apiKey.toStdWString(); // Convert key to wide string

    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(wTargetName.c_str());
    cred.UserName = const_cast<LPWSTR>(wUserName.c_str());

    // 2. Pass the data pointer and calculate the size in total BYTES
    cred.CredentialBlobSize = (DWORD)(wApiKey.size() * sizeof(wchar_t));
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(wApiKey.c_str()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if(CredWriteW(&cred, 0) == TRUE)
    {
        m_secretKeys.append(apiKey);
        emit secretKeysChanged();
        return true;
    }
    return false;
}

void PublicAuthClient::clearUserSession()
{
    if (!m_manager) return;

    // 1. Wipe Qt's underlying connection pool & authentication cache
    // This destroys any open sockets or cached session states.
    m_manager->clearAccessCache();       // Flushes auth tokens & SSL tickets
    m_manager->clearConnectionCache();   // Drops active TCP connections to the API server

    // 2. Purge the HTTP Cookie Jar (if the API sets any cookie data)
    if (m_manager->cookieJar())
    {
        QNetworkCookieJar *emptyJar = new QNetworkCookieJar(m_manager);
        m_manager->setCookieJar(emptyJar); // Overwriting deletes the old cookie cache
    }

    // 3. Wipe physical disk cache
    if (m_manager->cache())
    {
        m_manager->cache()->clear();
    }
}

void PublicAuthClient::handleReply(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError)
    {
        QByteArray responseData = reply->readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(responseData);
        QJsonObject jsonObj = jsonDoc.object();

        // Extract the access token (adjust key name if API differs slightly)
        QString accessToken = jsonObj["accessToken"].toString();
        if (accessToken.isEmpty())
        {
            // Sometimes it might just be under "token" or similar depending on the exact schema
            accessToken = jsonObj["token"].toString();
        }

        qDebug() << "Successfully retrieved Access Token:" << accessToken;
        setSessionActive(true);
        emit tokenReceived(accessToken);
    }
    else
    {
        qWarning() << "Authorization failed:" << reply->errorString();
        qWarning() << "Server response:" << reply->readAll();
        setSessionActive(false);
    }

    reply->deleteLater();
}

QStringList PublicAuthClient::secretKeys() const
{
    return m_secretKeys;
}

void PublicAuthClient::setSecretKeys(const QStringList &newSecretKeys)
{
    if (m_secretKeys == newSecretKeys)
        return;
    m_secretKeys = newSecretKeys;
    emit secretKeysChanged();
}

QString PublicAuthClient::secretKey() const
{
    return m_secretKey;
}

void PublicAuthClient::setSecretKey(const QString &newSecretKey)
{
    if (m_secretKey == newSecretKey)
        return;
    m_secretKey = newSecretKey;
    emit secretKeyChanged();
}
