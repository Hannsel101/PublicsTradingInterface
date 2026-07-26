#ifndef PUBLICAPIWORKER_H
#define PUBLICAPIWORKER_H

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QPromise>
#include <QFuture>
#include <QtConcurrent>
#include <QDebug>

// Helper structure to bundle results
struct AccountData {
    QString accountId;
    QJsonObject data;
};

class PublicApiWorker : public QObject {
    Q_OBJECT
public:
    PublicApiWorker(QObject *parent = nullptr) : QObject(parent), manager(new QNetworkAccessManager(this)) {}

    void fetchAllSubaccountsConcurrently() {
        QString baseUrl = "https://public.com";
        QString token = m_token;
        qDebug() << "token: " << token;
        qDebug() << "baseURL: " << baseUrl;

        // 1. Prepare initial request for all accounts
        QNetworkRequest request(QUrl("https://api.public.com/userapigateway/trading/account"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
        request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(token).toUtf8());
        request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

        qDebug() << "Made it past the setHeader";

        QNetworkReply *reply = manager->get(request);

        // Connect to handling the initial account list
        connect(reply, &QNetworkReply::finished, this, [this, reply, baseUrl, token]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qDebug() << "Failed to fetch accounts:" << reply->errorString();
                return;
            }

            QByteArray responseData = reply->readAll();
            qDebug() << "Raw Response:  " << responseData;

            // Parse initial list of account IDs
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            QJsonArray accounts = doc.object().value("accounts").toArray();

            QStringList accountIds;
            for (const QJsonValue &val : accounts) {
                accountIds.append(val.toObject().value("accountId").toString());
            }

            if (accountIds.isEmpty()) return;

            // 2. Dispatch concurrent queries
            executeConcurrentQueries(accountIds, baseUrl, token);
        });
    }

public slots:
    void processToken(QString newToken);

private:
    QNetworkAccessManager *manager;
    QString m_token;

    void executeConcurrentQueries(const QStringList &accountIds, const QString &baseUrl, const QString &token) {
        // Create an array of futures—one for each account network request
        QList<QFuture<AccountData>> futures;

        for (const QString &id : accountIds) {
            // QtConcurrent::run handles the parallel wrapper safely
            QFuture<AccountData> future = QtConcurrent::run([this, id, baseUrl, token]() {
                // Set up a local event loop for this specific background thread query
                QEventLoop loop;
                QNetworkRequest req(QUrl(baseUrl + QString("/userapigateway/trading/account/%1/portfolio").arg(id)));
                req.setRawHeader("Authorization", "Bearer " + token.toUtf8());

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
            .then([](QList<QFuture<AccountData>> completedFutures) {
                qDebug() << "--- All concurrent subaccount queries finished! ---";
                for (auto &future : completedFutures) {
                    AccountData res = future.result();
                    qDebug() << "Account ID:" << res.accountId << "Data size:" << QJsonDocument(res.data).toJson().size();
                }
            });
    }
};


#endif // PUBLICAPIWORKER_H
