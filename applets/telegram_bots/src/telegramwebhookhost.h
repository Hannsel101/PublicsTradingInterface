#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <memory>

// All calls, notifications and destruction belong on the application thread.
// This transport never owns bot tokens or performs domain actions.
class TelegramWebhookHost final : public QObject
{
    Q_OBJECT
public:
    struct Options {
        // Explicit environment override also works for GUI launches without Homebrew on PATH.
        QString executable = qEnvironmentVariable("NGROK_PATH", QStringLiteral("ngrok"));
        QStringList argumentPrefix;
        quint16 port = 8080;
        QUrl inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:4040/api/tunnels"));
        int discoveryDelayMs = 2000;
        int startupTimeoutMs = 20000;
    };
    static TelegramWebhookHost *instance();
    explicit TelegramWebhookHost(QObject *parent = nullptr);
    ~TelegramWebhookHost() override;
    void acquire(const QString &clientId, const Options &options);
    void release(const QString &clientId);
    bool ownsHealthUrl(const QUrl &url) const;
    QByteArray healthSecret() const;
signals:
    void ready(const QString &clientId, const QUrl &webhookUrl, const QString &secretToken);
    void failed(const QString &clientId, const QString &sanitizedMessage);
    void released(const QString &clientId);
    void updateReceived(const QString &clientId, const QJsonObject &update);
private:
    struct State;
    std::unique_ptr<State> d;
};
