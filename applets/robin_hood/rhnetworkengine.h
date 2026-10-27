#pragma once

#include <QJsonObject>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QNetworkReply;
struct RHOAuthGrant;

class RHNetworkEngine final : public QObject
{
public:
    enum class AuthKind { OAuthBearer, WebSecondaryBearer };
    // Short absolute deadlines are C++-only and honored solely on opted-in loopback.
    explicit RHNetworkEngine(QObject *parent = nullptr, bool allowLoopbackForTests = false,
                             int loopbackStockDeadlineMsForTests = 16000);
    QNetworkReply *sendAuthenticated(const QString &endpoint, const QByteArray &token,
                                    AuthKind kind, QString *error = nullptr,
                                    const QUrl &location = {}, const QString &accountNumber = {});
    static bool safeBearer(const QByteArray &token);
    // Narrow refresh-grant POST, independent of order transport and browser cookies.
    QNetworkReply *refreshOAuth(const RHOAuthGrant &grant, QString *error = nullptr);

    // Separate primary-only trading boundary; never widens sendAuthenticated.
    enum class StockRead { Accounts, Instruments, Quotes, Positions };
    enum class BuyKind { Market, FixedLimit };
    struct StockOrder {
        BuyKind buyKind = BuyKind::Market;
        QDateTime quoteTimestamp; // Local quote receipt time, not provider updated_at or POST time.
        QString accountNumber;
        QUrl account;
        QUrl instrument;
        QString symbol;
        QString quantity;
        QString side;
        QString price;
        QString askPrice;
        QString bidPrice;
        QString refId;
    };
    QNetworkReply *readStock(StockRead resource, const QByteArray &token, AuthKind kind,
                            const QString &scope = {}, const QUrl &page = {}, QString *error = nullptr);
    QNetworkReply *sendStockOrder(const StockOrder &order, const QByteArray &token, AuthKind kind,
                                 QString *error = nullptr, bool loopbackLiveForTests = false);
    // Single read-only order lookup. ID is an opaque broker ID, never a client ref.
    QNetworkReply *readStockOrder(const QString &orderId, const QByteArray &token, AuthKind kind,
                                 QString *error = nullptr);
    bool stockUrl(const QUrl &url, const QString &collection) const;
    bool stockLiveOrdersEnabled(bool loopbackLiveForTests = false) const;


    bool loadConfiguration(const QString &path, QString *error = nullptr);
    bool isConfigured() const;
    // A null reply means nothing was dispatched; callers own successful replies' cleanup.
    QNetworkReply *send(const QString &endpoint, const QVariantMap &values = {},
                        const QVariantMap &query = {}, const QVariantMap &body = {},
                        QString *error = nullptr);

private:
    QNetworkAccessManager m_network;
    QJsonObject m_endpoints;
    QJsonObject m_headerSets;
    bool m_configured = false;
    bool m_allowLoopbackForTests = false;
    int m_loopbackStockDeadlineMsForTests = 16000;
};
