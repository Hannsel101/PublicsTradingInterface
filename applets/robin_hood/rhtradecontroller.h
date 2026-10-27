#pragma once
#include "rhnetworkengine.h"
#include <QDateTime>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <functional>

// QObject-thread confined. Network and dependency owners must outlive this controller.
// Dependency callbacks may complete asynchronously; completion is generation guarded.
class RHTradeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY unavailableReasonChanged)
    Q_PROPERTY(QVariantList tradeResults READ tradeResults NOTIFY tradeResultsChanged)
    Q_PROPERTY(QString currentTransactionTitle READ currentTransactionTitle NOTIFY currentTransactionTitleChanged)
    Q_PROPERTY(bool tradeResultsVisible READ tradeResultsVisible NOTIFY tradeResultsVisibleChanged)
    Q_PROPERTY(bool tradeResultsComplete READ tradeResultsComplete NOTIFY tradeResultsCompleteChanged)
    Q_PROPERTY(bool tradeResultsBusy READ tradeResultsBusy NOTIFY tradeResultsBusyChanged)
    Q_PROPERTY(bool liveOrdersEnabled READ liveOrdersEnabled CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
public:
    struct Session {
        int sessionIndex = -1;
        quint64 generation = 0;
        QString profileId;
        QByteArray token;
        QDateTime expiresAt;
        RHNetworkEngine::AuthKind authKind = RHNetworkEngine::AuthKind::OAuthBearer;
        QVariantList accounts;
    };
    struct Dependencies {
        std::function<void(std::function<void(bool, Session, QString)>)> acquire;
        std::function<bool(const Session &)> current;
        // Reconciliation only: local must be disk-free; async completes on this
        // QObject thread. Missing callbacks fail closed, never poll current().
        std::function<bool(const Session &)> currentLocal;
        std::function<void(const Session &, std::function<void(bool)>)> currentAsync;
        std::function<void()> release;
        std::function<QString()> availabilityReason;
        // Optional test scheduler must retain callbacks, not block. Delay is milliseconds.
        std::function<void(int, std::function<void()>)> schedule;
        // C++ only, honored only with explicit network loopback override in Debug.
        bool loopbackLiveOrdersForTests = false;
        int ownershipDeadlineMsForTests = 16000;
    };
    RHTradeController(RHNetworkEngine &network, Dependencies dependencies, QObject *parent = nullptr);
    ~RHTradeController() override;
    bool available() const { return !m_busy && m_unavailable.isEmpty(); }
    QString unavailableReason() const { return m_unavailable; }
    QVariantList tradeResults() const { return m_results; }
    QString currentTransactionTitle() const { return m_title; }
    bool tradeResultsVisible() const { return m_visible; }
    bool tradeResultsComplete() const { return m_complete; }
    bool tradeResultsBusy() const { return m_busy; }
    bool liveOrdersEnabled() const { return m_network.stockLiveOrdersEnabled(m_deps.loopbackLiveOrdersForTests); }
    QString error() const { return m_error; }
    Q_INVOKABLE bool submit(QString input, QString side);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void dismissTradeResults();
    void refreshAvailability();
signals:
    void availableChanged();
    void unavailableReasonChanged();
    void tradeResultsChanged();
    void currentTransactionTitleChanged();
    void tradeResultsVisibleChanged();
    void tradeResultsCompleteChanged();
    void tradeResultsBusyChanged();
    void errorChanged();
private:
    using Read = RHNetworkEngine::StockRead;
    using PageDone = std::function<void(QJsonArray)>;
    RHNetworkEngine &m_network;
    Dependencies m_deps;
    std::function<void()> m_releaseLease;
    Session m_session;
    struct BuyPlan {
        QJsonObject instrument, quote;
        QString price;
        RHNetworkEngine::BuyKind kind = RHNetworkEngine::BuyKind::Market;
        QDateTime capturedAt;
    };
    // Built completely before any row; read-only during account execution.
    QList<BuyPlan> m_buyPlans;
    bool m_preflighting = false;
    QVariantList m_results;
    QStringList m_symbols;
    QString m_attemptDiagnostic;
    QString m_side, m_error, m_unavailable, m_title;
    QPointer<QNetworkReply> m_reply;
    quint64 m_epoch = 0;
    int m_row = 0;
    bool m_busy = false, m_visible = false, m_complete = false;
    bool m_locked = false, m_acquiring = false, m_cancelled = false, m_posting = false, m_finishing = false;
    bool notify();
    bool valid();
    bool locallyValid();
    void finish(QString error = {});
    void begin(Session session);
    void preflightBuy();
    void nextRow();
    void pages(Read read, QString scope, PageDone done, QUrl page = {}, QJsonArray accumulated = {}, QSet<QString> visited = {});
    void resolveInstrument(QJsonArray instruments);
    void resolveQuote(QJsonArray quotes, QJsonObject instrument, QString quantity = "1");
    void resolvePosition(QJsonArray positions, QJsonObject instrument);
    void dispatch(QJsonObject instrument, QString quantity, QString ask = {}, QString bid = {},
                  RHNetworkEngine::BuyKind kind = RHNetworkEngine::BuyKind::Market,
                  QString price = {}, QDateTime capturedAt = {});
    void orderReceipt(QNetworkReply *reply, const RHNetworkEngine::StockOrder &order,
                      bool lookup = false, QString candidate = {}, QString postDiagnostic = {}, bool ownershipVerified = false);
    void result(QString status, QString message, bool success = false, QString orderId = {}, bool halt = false);
};
