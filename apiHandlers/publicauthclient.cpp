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
    jsonObj["secret"] = m_secretKey;
    jsonObj["validityInMinutes"] = 5; // Adjust as needed

    QJsonDocument doc(jsonObj);
    QByteArray data = doc.toJson();

    // Send asynchronous POST request
    QNetworkReply *reply = m_manager.post(request, data);

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
    // Determine the next index based on the count of existing keys
    int nextIndex = secretKeys().size();
    QString targetName = QString("PublicsApiKey%1").arg(nextIndex);

    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR)targetName.utf16();
    cred.UserName = (LPWSTR)userName.utf16();
    cred.CredentialBlobSize = (DWORD)(apiKey.toUtf8().size());
    cred.CredentialBlob = (LPBYTE)apiKey.toUtf8().data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if(CredWriteW(&cred, 0) == TRUE)
    {
        m_secretKeys.append(apiKey);
        emit secretKeysChanged();
        return true;
    }
    return false;
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
    } else
    {
        qWarning() << "Authorization failed:" << reply->errorString();
        qWarning() << "Server response:" << reply->readAll();
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
