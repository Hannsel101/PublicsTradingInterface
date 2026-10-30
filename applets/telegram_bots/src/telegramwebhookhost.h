#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <memory>
#include <functional>

class QHttpServerRequest;

// All calls, notifications and destruction belong on the application thread.
// This transport never owns bot tokens or performs domain actions.
class TelegramWebhookHost final : public QObject
{
    Q_OBJECT
public:
    enum class InspectorConflictAction { Restart, Cancel };
    Q_ENUM(InspectorConflictAction)
    // Only a currently pending client may resolve a conflict. Cancel abandons that
    // client's lease; Restart requires native orphan/executable/listener proof.
    void resolveInspectorConflict(const QString &clientId, InspectorConflictAction action);
    struct Options {
        // Explicit environment override also works for GUI launches without Homebrew on PATH.
        QString executable = qEnvironmentVariable("NGROK_PATH", QStringLiteral("ngrok"));
        QStringList argumentPrefix;
        // Empty selects app-private persistent storage; tests inject a temporary root.
        QString ownershipDirectory;
        quint16 port = 8080;
        QUrl inspectorUrl = QUrl(QStringLiteral("http://127.0.0.1:4040/api/tunnels"));
        int discoveryDelayMs = 2000;
        int startupTimeoutMs = 20000;
    };
    static TelegramWebhookHost *instance();
    explicit TelegramWebhookHost(QObject *parent = nullptr);
    ~TelegramWebhookHost() override;
    // Synchronous per-client admission, installed before any ready notification.
    // Return false for retryable capacity/durability failure (HTTP503), true for
    // admitted work or deliberate ignores/duplicates (HTTP200). No handler retains
    // the legacy updateReceived-only transport. Use guarded owner captures.
    using AdmissionHandler = std::function<bool(const QJsonObject &)>;
    using RequestHandler = std::function<int(const QHttpServerRequest &)>;
    void acquire(const QString &clientId, const Options &options, AdmissionHandler admit = {},
        QString secretToken = {}, RequestHandler receive = {});
    void release(const QString &clientId);
    bool ownsHealthUrl(const QUrl &url) const;
    QByteArray healthSecret() const;
signals:
    void inspectorConflict(const QString &clientId);
    void ready(const QString &clientId, const QUrl &webhookUrl, const QString &secretToken);
    void failed(const QString &clientId, const QString &sanitizedMessage);
    void released(const QString &clientId);
    void updateReceived(const QString &clientId, const QJsonObject &update);
private:
    struct State;
    std::unique_ptr<State> d;
};
