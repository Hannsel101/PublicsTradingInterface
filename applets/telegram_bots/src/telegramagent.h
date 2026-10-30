#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>
#include "telegramwebhookhost.h"

// Construct, use and destroy on one thread with a running event loop; do not move threads.
// active()/starting() describe Telegram transport; healthy() is the last health result
// (optimistically true after verified transport activation until the first status probe).
// Failed health leaves transport available for recovery. The default constructor owns
// an automatic ngrok webhook lease; injected URL-only constructors retain polling.
// Webhook stop retires local work immediately; stopping() gates bounded remote cleanup
// and last-lease process teardown. Trade admission is durable at-most-once, not
// guaranteed execution: a crash after admission can suppress an unexecuted command.
// Older out-of-order trade IDs fail closed; admission watermarks never expire.
// No brokerage integration, persistent credentials or QML-visible token state.
class TelegramAgent final : public QObject
{
    Q_OBJECT
public:
    struct WebhookOptions {
        bool enabled = true;
        TelegramWebhookHost::Options tunnel;
    };
    // Injected URL constructors explicitly retain polling compatibility.
    explicit TelegramAgent(QObject *parent = nullptr);
    explicit TelegramAgent(QUrl apiRoot, QObject *parent = nullptr);
    TelegramAgent(QUrl apiRoot, QUrl healthEndpoint, QObject *parent = nullptr);
    TelegramAgent(QUrl apiRoot, QUrl healthEndpoint, WebhookOptions options, QObject *parent = nullptr);
    // Remote cleanup requires stop() and waiting until stopping() is false before destruction.
    ~TelegramAgent() override;

    bool active() const;
    bool healthy() const;
    bool starting() const;
    bool stopping() const;
    // One-argument compatibility is restricted to explicitly injected literal-
    // loopback polling fixtures. Production/webhook Start requires the persisted,
    // password-decrypted session secret on every activation; no secret is generated.
    void start(QString token);
    void start(QString token, QString sessionSecret);
    void stop();
    void resolveInspectorConflict(const QString &action);
    // Policy is RAM-only here. While running, changes may only revoke existing IDs.
    // Malformed IDs fail closed; callers persist policy separately in protected storage.
    void setAllowedChatIds(QStringList ids);
    // Pre-start only, literal loopback API roots only; path is a dedicated directory.
    void setCommandLedgerPathForTests(QString path);
    // Reports accept escaped text plus balanced <b> markup only; all other markup
    // is escaped. Chunks preserve entities/scalars/bold and are sent serially.
    // Stop/revocation cancels notification context, not previously admitted orders.
    void completeTradeCommand(QString requestId, QStringList htmlMessages);
    // Intermediate report: preserves the current origin and busy lease. true
    // admits one report attempt; tradeReportFinished settles it asynchronously,
    // including cancellation/failure (not proof of delivery). false emits nothing.
    bool reportTradeCommand(QString requestId, QStringList htmlMessages);

signals:
    void inspectorPortConflict();
    void tradeCommandReceived(QString requestId, QString side, QStringList symbols);
    void robinhoodTradeCommandReceived(QString requestId, QString side, QStringList symbols);
    void combinedTradeCommandReceived(QString requestId, QString side, QStringList symbols);
    void tradeReportFinished(QString requestId);
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    struct State;
    std::unique_ptr<State> d;
};
