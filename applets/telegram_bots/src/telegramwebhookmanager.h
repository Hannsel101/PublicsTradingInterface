#pragma once

#include <QObject>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

class QHttpServerRequest;
class QNetworkReply;

// Thread-affine transport/policy boundary. No passwords, trading parser or ledger.
// Standalone owns a bounded loopback QHttpServer; externallyHosted uses the existing
// shared host's QHttpServer through handleRequest(), and never opens another port.
class TelegramWebhookManager final : public QObject
{
    Q_OBJECT
public:
    struct Options {
        bool externallyHosted = false;
        quint16 port = 8080;
        QUrl apiRoot = QUrl(QStringLiteral("https://api.telegram.org"));
        bool allowLoopbackProviderForTests = false;
        int requestTimeoutMs = 10000;
    };
    TelegramWebhookManager(QString botToken, QString secretToken, QString ngrokUrl,
        qint64 adminChatId, QObject *parent = nullptr);
    TelegramWebhookManager(QString botToken, QString secretToken, QString ngrokUrl,
        qint64 adminChatId, Options options, QObject *parent = nullptr);
    ~TelegramWebhookManager() override;
    void setAllowedChatIds(QStringList ids);
    void setWebhookUrl(QUrl url);
    using AdmissionHandler = std::function<bool(const QJsonObject &)>;
    void setAdmissionHandler(AdmissionHandler handler);
    // Returns the managed reply for lifecycle receipt retention. Never abort merely
    // because Stop was requested: commitment may precede the acknowledgement.
    // nullptr means local validation failed. finished signal is still bounded.
    QNetworkReply *registerWebhook();
    int handleRequest(const QHttpServerRequest &request);
    quint16 listeningPort() const;
    static bool eligiblePrivateMessage(const QJsonObject &update, const QSet<qint64> &allowed);
    static bool validSecret(const QString &secret);
signals:
    void commandReceived(QString command);
    // Queued beyond the reply/abort stack; observers may destroy this manager.
    void registrationFinished(bool confirmed, bool uncertain);
    void errorOccurred(QString sanitizedMessage);
private:
    struct State;
    std::unique_ptr<State> d;
};
