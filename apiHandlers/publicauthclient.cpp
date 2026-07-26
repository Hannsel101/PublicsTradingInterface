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
