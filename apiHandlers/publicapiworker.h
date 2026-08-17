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
    //QJsonObject data;
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


    Q_INVOKABLE void executePreflight(const QString &symbol,    // Ticker Symbol
                                      const QString &side);     // BUY or SELL

    Q_INVOKABLE void executeTrade(const QString &symbol,        // Ticker Symbol
                                  const QString &side);         // BUY or SELL

public slots:
    /*
     * Slot used to process new session tokens after authorization has been granted
     * */
    void processToken(QString newToken);

signals:
    void transactionFailed(QString reason);
    void preflightPassed(QString summary);
    void orderExecuted(QString details);

private:
    QNetworkAccessManager *manager;
    QString m_token;

    /*
     * List of accounts that can perform a trade
     *
     * Uses a custom AccountData struct which in its current form
     * has a single QString parameter for the account ID but
     * more data such as buying power stocks available to sell
     * can be added for a more complete set of options that
     * the end user can perform
     * */
    QList<AccountData> m_accountList;

    /*
     * Helper function to add a new account ID into the account list
     *
     * if a new id is passed in then it will be appended
     * otherwise, it will be skipped
     * */
    void addAccountToList(QString id);


    /*
     * Pulls data from multiple accounts in parallel using QFutures and an event loop that runs in
     * the background
     * */
    void executeConcurrentQueries(const QStringList &accountIds, const QString &baseUrl, const QString &token);

    /*
     * Sets up the base headers for the publics api
     *
     * The passed in requestAppendature completes the transaction type being performed
     * while keeping the boilerplate portions the same
     *
     * function assumes the user is authenticated and has a valid token to perform
     * get and post requests with api endpoints
     *
     * Common request types:
     *  token generation: /userapiauthservice/
     *  account & data (GET): /userapigateway/trading/
     *  order placement (POST): /userapigateway/trading/
     * */
    QNetworkRequest setPublicBrokerageHeaders(QString requestType);


    /*
     * perform a trade if the preflight approves the trade
     * */
    void executeTradeOrPreflight(const QString &accountId,     // Account to perform trade
                                 const QString &symbol,        // Ticker Symbol
                                 const QString &side,          // "BUY" or "SELL"
                                 bool isPreflight);            // isPreflight = true means this will be a preflight
};


#endif // PUBLICAPIWORKER_H
