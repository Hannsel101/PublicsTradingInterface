#ifndef PUBLICAPIWORKER_H
#define PUBLICAPIWORKER_H

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QPromise>
#include <QFuture>
#include <QVariantList>
#include <QVariantMap>
#include <QtConcurrent>

// For Debugging
#include <QDebug>

// For adding delays to trades
#include <QEventLoop>
#include <QTimer>

// Helper structure to bundle results
struct AccountData {
    QString accountId;
    QString accountType;
    //QJsonObject data;
};

class PublicApiWorkerTest;

class PublicApiWorker : public QObject {
    Q_OBJECT
public:
    explicit PublicApiWorker(QObject *parent = nullptr) : QObject(parent), manager(new QNetworkAccessManager(this)) {}

    Q_PROPERTY(QVariantList tradeResults READ tradeResults NOTIFY tradeResultsChanged FINAL)
    Q_PROPERTY(QString currentTransactionTitle READ currentTransactionTitle NOTIFY currentTransactionTitleChanged FINAL)
    Q_PROPERTY(bool tradeResultsVisible READ tradeResultsVisible WRITE setTradeResultsVisible NOTIFY tradeResultsVisibleChanged FINAL)
    Q_PROPERTY(bool tradeResultsComplete READ tradeResultsComplete NOTIFY tradeResultsCompleteChanged FINAL)
    Q_PROPERTY(bool tradeResultsBusy READ tradeResultsBusy NOTIFY tradeResultsBusyChanged FINAL)

    /*
     * Performs a pull of all the available accounts and their base level data such as
     * ability to buy, sell, and what balance the accounts have.
     * */
    void fetchAllSubaccountsConcurrently();


    /*
     * Preflight is a test function to test sending a request for any type of trade
     *
     * Trade performs an actual market buy or sell on equities
     * */
    Q_INVOKABLE void executePreflight(const QString &symbol,    // Ticker Symbol
                                      const QString &side);     // BUY or SELL

    Q_INVOKABLE void executeTrade(const QString &symbol,        // Ticker Symbol
                                  const QString &side);         // BUY or SELL

    Q_INVOKABLE void dismissTradeResults();

    /*
     * Clears the sub account list to ensure accounts don't linger when attempting
     * to use different API keys. Also clears the authorization token to ensure
     * incorrectly configured token use when swapping between keys does not occur.
     * */
    Q_INVOKABLE void clearSubAccountsList();

    QVariantList tradeResults() const;
    QString currentTransactionTitle() const;
    bool tradeResultsVisible() const;
    void setTradeResultsVisible(bool visible);
    bool tradeResultsComplete() const;
    bool tradeResultsBusy() const;
    static QString displayLabelForAccount(const QString &accountId, const QString &accountType);

public slots:
    /*
     * Slot used to process new session tokens after authorization has been granted
     * */
    void processToken(QString newToken);

signals:
    void transactionFailed(QString reason);
    void preflightPassed(QString summary);
    void orderExecuted(QString details);
    void tradeResultsChanged();
    void currentTransactionTitleChanged();
    void tradeResultsVisibleChanged();
    void tradeResultsCompleteChanged();
    void tradeResultsBusyChanged();

private:
    friend class PublicApiWorkerTest;

    QNetworkAccessManager *manager;
    QString m_token;
    QVariantList m_tradeResults;
    QString m_currentTransactionTitle;
    bool m_tradeResultsVisible = false;
    bool m_tradeResultsComplete = true;
    int m_pendingTradeResults = 0;
    quint64 m_accountLoadGeneration = 0;

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
    void addAccountToList(QString id, QString accountType = QString());


    /*
     * Pulls data from multiple accounts in parallel using QFutures and an event loop that runs in
     * the background
     * */
    void executeConcurrentQueries(const QList<AccountData> &accounts,
                                  const QString &baseUrl,
                                  const QString &token,
                                  quint64 accountLoadGeneration);

    void applyDiscoveredAccounts(const QList<AccountData> &accounts,
                                 quint64 accountLoadGeneration,
                                 const QString &token);

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

    void beginTradeResults(const QString &symbol, const QString &side, bool isPreflight);
    void updateTradeResult(const QString &accountId, bool success, const QString &message);
    void setCurrentTransactionTitle(const QString &title);
    void setTradeResultsComplete(bool complete);
    QString transactionTitle(const QString &symbol, const QString &side, bool isPreflight) const;
    QString apiErrorMessage(const QNetworkReply *reply, const QByteArray &responseData) const;
};


#endif // PUBLICAPIWORKER_H
