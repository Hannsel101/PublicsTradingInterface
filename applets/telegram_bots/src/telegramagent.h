#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <memory>
#include "telegramwebhookhost.h"

// Construct, use and destroy on one thread with a running event loop; do not move threads.
// active()/starting() describe Telegram transport; healthy() is the last health result
// (optimistically true after verified transport activation until the first status probe).
// Failed health leaves transport available for recovery. The default constructor owns
// an automatic ngrok webhook lease; injected URL-only constructors retain polling.
// Webhook stop retires local work immediately; stopping() gates bounded remote cleanup
// and last-lease process teardown. No durable exactly-once delivery is claimed.
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
    void start(QString token);
    void stop();

signals:
    void stateChanged();
    void errorOccurred(const QString &message);

private:
    struct State;
    std::unique_ptr<State> d;
};
