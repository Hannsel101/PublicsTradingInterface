#ifndef PUBLICAPIWORKER_H
#define PUBLICAPIWORKER_H

#include <QByteArray>
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
#include <QDeadlineTimer>
#include <QDateTime>
#include <QHash>
#include <QVariantList>
#include <QVariantMap>


// For Debugging
#include <QDebug>

// For adding delays to trades
#include <QTimer>

// Helper structure to bundle results
struct AccountData {
    QString accountId;
    QString accountType;
    QHash<QString, float> stockShareBalances;
    bool holdingsLoaded = false;
};

struct PublicRequestPolicy {
    int portfolioPauseMs = 500;
    int symbolPauseMs = 100;
    int accountPauseMs = 200;
    int gatewayCooldownMs = 5000;
    int retryBaseMs = 1000;
    int retryCapMs = 30000;
    int maxRetries = 3;
    int timeoutMs = 30000;
    qint64 retryDelayMs(int attempt, const QByteArray &retryAfter, const QDateTime &now, quint32 jitterMs) const;
};

class PublicApiWorkerTest;

class PublicApiWorker : public QObject {
    Q_OBJECT
public:
    explicit PublicApiWorker(QObject *parent = nullptr)
        : PublicApiWorker(QUrl(QStringLiteral("https://api.public.com/")), parent) {}
    explicit PublicApiWorker(const QUrl &apiBaseUrl, QObject *parent = nullptr);

    Q_PROPERTY(QVariantList tradeResults READ tradeResults NOTIFY tradeResultsChanged FINAL)
    Q_PROPERTY(QString currentTransactionTitle READ currentTransactionTitle NOTIFY currentTransactionTitleChanged FINAL)
    Q_PROPERTY(bool tradeResultsVisible READ tradeResultsVisible WRITE setTradeResultsVisible NOTIFY tradeResultsVisibleChanged FINAL)
    Q_PROPERTY(bool tradeResultsComplete READ tradeResultsComplete NOTIFY tradeResultsCompleteChanged FINAL)
    Q_PROPERTY(bool tradeResultsBusy READ tradeResultsBusy NOTIFY tradeResultsBusyChanged FINAL)

    // Policy is read from QSettings at construction and can be changed while idle.
    void setRequestPolicy(const PublicRequestPolicy &policy);
    PublicRequestPolicy requestPolicy() const { return m_policy; }


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
    static QStringList parseSymbols(const QString &input);
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
    void accountLoadFinished(bool success, const QString &reason);

private:
    friend class PublicApiWorkerTest;

    QNetworkAccessManager *manager;
    QUrl m_apiBaseUrl;
    QString m_token;
    QVariantList m_tradeResults;
    QString m_currentTransactionTitle;
    bool m_tradeResultsVisible = false;
    bool m_tradeResultsComplete = true;
    int m_pendingTradeResults = 0;
    quint64 m_accountLoadGeneration = 0;

    /*
     * Discovered eligible accounts, including accounts whose holdings failed to load.
     *
     * Each AccountData entry stores its ID, type, and a ticker-to-share-balance
     * map populated from the account portfolio response.
     * */
    QList<AccountData> m_accountList;

    /*
     * Helper function to add a new account ID into the account list
     *
     * if a new id is passed in then it will be appended
     * otherwise, it will be skipped
     * */
    void addAccountToList(QString id,
                          QString accountType = QString(),
                          const QHash<QString, float> &stockShareBalances = {},
                          bool holdingsLoaded = false);

    static QHash<QString, float> stockShareBalancesFromPortfolio(const QByteArray &responseData, bool *valid = nullptr);
    static QJsonObject orderPayload(const QString &symbol,
                                    const QString &side,
                                    float quantity);


    /*
     * Loads portfolios sequentially with completion-based pacing and bounded GET retries.
     * */
    void fetchAccounts(int attempt = 0);
    void fetchPortfolio(int index, int attempt, quint64 generation);
    void reconcileOrder(const QString &accountId, const QString &orderId, int resultIndex, quint64 generation, int attempt = 0);
    PublicRequestPolicy m_policy;
    QDeadlineTimer m_cooldown{0};
    bool m_loadingAccounts = false;

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
                                 float quantity,                // Number of shares to trade
                                 bool isPreflight,
                                 int resultIndex,
                                 quint64 generation);

    void beginTradeResults(const QStringList &symbols, const QString &side, bool isPreflight);
    void updateTradeResult(int resultIndex, bool success, const QString &message, const QString &status = {});
    void startTrades(const QString &input, const QString &side, bool isPreflight);
    void dispatchNextTrade(quint64 generation);
    void finishTradeStep(int resultIndex, bool success, const QString &message, quint64 generation, const QString &status = {});
    void setCurrentTransactionTitle(const QString &title);
    void setTradeResultsComplete(bool complete);
    QString transactionTitle(const QString &symbol, const QString &side, bool isPreflight) const;
    QStringList m_tradeSymbols;
    QList<AccountData> m_tradeAccounts;
    QString m_tradeSide;
    bool m_isPreflight = false;
    int m_nextTradeIndex = 0;
    quint64 m_tradeGeneration = 0;
};


#endif // PUBLICAPIWORKER_H
