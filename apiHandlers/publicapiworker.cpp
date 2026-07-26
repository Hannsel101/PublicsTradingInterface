#include "publicapiworker.h"

// PublicApiWorker::PublicApiWorker(QObject *parent)
//     : QObject{parent}
// {}

void PublicApiWorker::processToken(QString newToken)
{
    m_token = newToken;
    fetchAllSubaccountsConcurrently();
}

void PublicApiWorker::fetchAllSubaccountsConcurrently() {
    QString baseUrl = "https://public.com";
    QString token = m_token;
    qDebug() << "token: " << token;
    qDebug() << "baseURL: " << baseUrl;

    // 1. Prepare initial request for all accounts
    QNetworkRequest request(QUrl("https://api.public.com/userapigateway/trading/account"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    QNetworkReply *reply = manager->get(request);

    // Connect to handling the initial account list
    connect(reply, &QNetworkReply::finished, this, [this, reply, baseUrl, token]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Failed to fetch accounts:" << reply->errorString();
            return;
        }

        QByteArray responseData = reply->readAll();
        //qDebug() << "Raw Response:  " << responseData;

        // Parse initial list of account IDs
        QJsonDocument doc = QJsonDocument::fromJson(responseData);
        QJsonArray accounts = doc.object().value("accounts").toArray();

        QStringList accountIds;
        for (const QJsonValue &val : accounts) {
            accountIds.append(val.toObject().value("accountId").toString());
        }

        if (accountIds.isEmpty()) return;

        QString baseUrl2 = "https://api.public.com";
        // 2. Dispatch concurrent queries
        executeConcurrentQueries(accountIds, baseUrl2, token);
    });
}

void PublicApiWorker::executeConcurrentQueries(const QStringList &accountIds, const QString &baseUrl, const QString &token) {
    // Create an array of futures—one for each account network request
    QList<QFuture<AccountData>> futures;

    for (const QString &id : accountIds) {
        // QtConcurrent::run handles the parallel wrapper safely
        QFuture<AccountData> future = QtConcurrent::run([this, id, baseUrl, token]() {
            // Set up a local event loop for this specific background thread query
            QEventLoop loop;
            QString requestString = baseUrl + "/userapigateway/trading/" + id + "/portfolio/v2";
            QNetworkRequest req(requestString);
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            req.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

            QNetworkReply *subReply = manager->get(req);
            connect(subReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec(); // Blocks only this background worker thread, not the GUI/Main thread

            AccountData result{id, QJsonObject()};
            if (subReply->error() == QNetworkReply::NoError) {
                result.data = QJsonDocument::fromJson(subReply->readAll()).object();
            } else {
                qDebug() << "Error fetching subaccount" << id << ":" << subReply->errorString();
            }
            subReply->deleteLater();
            return result;
        });
        futures.append(future);
    }

    // 3. Monitor all concurrent tasks and wait for them to finish
    // QtFuture::whenAll maps nicely to JavaScript's Promise.all()
    QtFuture::whenAll(futures.begin(), futures.end())
        .then(this, [this](QList<QFuture<AccountData>> completedFutures) {
            qDebug() << "--- All concurrent subaccount queries finished! ---";
            for (auto &future : completedFutures) {
                m_accountList.append(future.result());
            }

            for(auto &accountData: m_accountList)
            {
                qDebug() << "Account ID: " << accountData.accountId;

                for (const QString &key : accountData.data.keys()) {
                    QJsonValue val = accountData.data[key];
                    qDebug() << key << ":" << val.toVariant(); // .toVariant() converts to a generic QVariant for easy printing
                }

            }
        });
}