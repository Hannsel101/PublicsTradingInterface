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
#include <QString>

// Helper structure to bundle results
struct AccountData {
    QString accountId;
    QJsonObject data;
};

class PublicApiWorker : public QObject {
    Q_OBJECT
public:
    PublicApiWorker(QObject *parent = nullptr) : QObject(parent), manager(new QNetworkAccessManager(this)) {}

    /*
     * Performs a pull of all the available accounts and their base level data such as
     * ability to buy, sell, and what balance the accounts have.
     * */
    void fetchAllSubaccountsConcurrently();

public slots:
    /*
     * Slot used to process new session tokens after authorization has been granted
     * */
    void processToken(QString newToken);

private:
    QNetworkAccessManager *manager;
    QString m_token;
    QList<AccountData> m_accountList;


    /*
     * Pulls data from multiple accounts in parallel using QFutures and an event loop that runs in
     * the background
     * */
    void executeConcurrentQueries(const QStringList &accountIds, const QString &baseUrl, const QString &token);
};


#endif // PUBLICAPIWORKER_H
