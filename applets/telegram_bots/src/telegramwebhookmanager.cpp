#include "telegramwebhookmanager.h"
#include "telegramdebuglogging.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerResponder>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cmath>

namespace {
bool positive(const QJsonValue &value)
{
    return value.isDouble() && value.toDouble() > 0 && value.toDouble() < 4503599627370496.0
        && std::floor(value.toDouble()) == value.toDouble();
}
bool safeWebhook(const QUrl &url)
{
    return url.isValid() && url.scheme() == QStringLiteral("https") && !url.host().isEmpty()
        && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}
class BoundedListener final : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
        socket->setReadBufferSize(272 * 1024);
        auto *deadline = new QTimer(socket);
        deadline->setSingleShot(true); deadline->setTimerType(Qt::PreciseTimer);
        QObject::connect(deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
        deadline->start(3000);
        addPendingConnection(socket);
    }
};
}
struct TelegramWebhookManager::State
{
    QString token;
    QString secret;
    QUrl webhook;
    Options options;
    QSet<qint64> allowed;
    AdmissionHandler admit;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> registration;
    QPointer<QHttpServer> server;
    QPointer<QTcpServer> listener;
    bool validRoot() const
    {
        const auto &url = options.apiRoot;
        if (!url.isValid() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
            || (!url.path().isEmpty() && url.path() != QStringLiteral("/"))) return false;
        if (url.scheme() == QStringLiteral("https") && url.host() == QStringLiteral("api.telegram.org")
            && (url.port() == -1 || url.port() == 443)) return true;
        return options.allowLoopbackProviderForTests && url.scheme() == QStringLiteral("http")
            && QHostAddress(url.host()).isLoopback();
    }
};
TelegramWebhookManager::TelegramWebhookManager(QString token, QString secret, QString url, qint64 admin, QObject *parent)
    : TelegramWebhookManager(std::move(token), std::move(secret), std::move(url), admin, Options{}, parent) {}
TelegramWebhookManager::TelegramWebhookManager(QString token, QString secret, QString url, qint64 admin, Options options, QObject *parent)
    : QObject(parent), d(std::make_unique<State>())
{
    d->token = std::move(token); d->secret = std::move(secret);
    d->webhook = QUrl(url, QUrl::StrictMode); d->options = std::move(options);
    d->network.setProxy(QNetworkProxy::NoProxy);
    if (admin > 0) setAllowedChatIds({QString::number(admin)});
    if (d->options.externallyHosted) return;
    d->server = new QHttpServer(this);
    QHttpServerConfiguration limits;
    limits.setMaximumBodySize(256 * 1024); limits.setMaximumUrlSize(1024);
    limits.setMaximumTotalHeaderSize(16 * 1024); limits.setMaximumHeaderFieldSize(4096);
    limits.setMaximumHeaderFieldCount(32); limits.setMaximumConnections(64);
    limits.setMaximumConnectionsPerHost(32); limits.setRateLimitPerSecond(100);
    limits.setKeepAliveTimeout(std::chrono::seconds(1)); d->server->setConfiguration(limits);
    d->server->setMissingHandler(this, [this](const QHttpServerRequest &request, QHttpServerResponder &responder) {
        const QPointer<TelegramWebhookManager> guard(this);
        const auto status = handleRequest(request);
        if (guard) responder.write(static_cast<QHttpServerResponder::StatusCode>(status));
    });
    d->listener = new BoundedListener(d->server);
    if (!d->listener->listen(QHostAddress::LocalHost, d->options.port) || !d->server->bind(d->listener)) {
        d->listener->close();
        QTimer::singleShot(0, this, [this] { emit errorOccurred(QStringLiteral("Cannot bind the private webhook receiver.")); });
    }
}
TelegramWebhookManager::~TelegramWebhookManager()
{
    if (d->listener) d->listener->close();
    // A synchronous command observer can delete us within the HTTP callback.
    if (d->server) { d->server->setParent(nullptr); d->server->deleteLater(); }
}
bool TelegramWebhookManager::validSecret(const QString &secret)
{
    static const QRegularExpression format(QStringLiteral("\\A[A-Za-z0-9_-]{1,256}\\z"));
    return format.match(secret).hasMatch();
}
void TelegramWebhookManager::setAllowedChatIds(QStringList ids)
{
    QSet<qint64> policy;
    static const QRegularExpression format(QStringLiteral("\\A[1-9][0-9]{0,15}\\z"));
    for (const auto &id : ids) {
        bool ok = false; const auto value = id.toLongLong(&ok);
        if (!ok || !format.match(id).hasMatch() || value >= (qint64(1) << 52)) { policy.clear(); break; }
        policy.insert(value);
    }
    d->allowed = std::move(policy);
}
void TelegramWebhookManager::setWebhookUrl(QUrl url) { d->webhook = std::move(url); }
void TelegramWebhookManager::setAdmissionHandler(AdmissionHandler handler) { d->admit = std::move(handler); }
quint16 TelegramWebhookManager::listeningPort() const { return d->listener ? d->listener->serverPort() : 0; }
bool TelegramWebhookManager::eligiblePrivateMessage(const QJsonObject &update, const QSet<qint64> &allowed)
{
    // A single supported update branch only. Reject hybrid/edited/business envelopes.
    if (update.size() != 2 || !positive(update.value("update_id")) || !update.value("message").isObject()) return false;
    const auto message = update.value("message").toObject();
    const auto from = message.value("from").toObject(); const auto chat = message.value("chat").toObject();
    for (const auto *field : {"sender_chat", "sender_business_bot", "business_connection_id", "guest_query_id",
         "forward_origin", "forward_from", "forward_from_chat", "forward_date", "via_bot", "is_automatic_forward",
         "is_from_offline", "guest_bot_caller_user", "guest_bot_caller_chat", "message_thread_id", "is_topic_message",
         "direct_messages_topic", "edit_date"}) if (message.contains(QLatin1String(field))) return false;
    return positive(message.value("message_id")) && positive(message.value("date"))
        && message.value("text").isString() && !message.value("text").toString().isEmpty()
        && message.value("text").toString().size() <= 4096
        && from.value("is_bot").isBool() && !from.value("is_bot").toBool()
        && positive(from.value("id")) && positive(chat.value("id"))
        && chat.value("type") == QJsonValue(QStringLiteral("private"))
        && from.value("id").toInteger() == chat.value("id").toInteger()
        && allowed.contains(chat.value("id").toInteger());
}
int TelegramWebhookManager::handleRequest(const QHttpServerRequest &request)
{
    const auto path = d->webhook.path().isEmpty() ? QStringLiteral("/") : d->webhook.path();
    if (!request.remoteAddress().isLoopback() || request.url().path() != path
        || request.url().hasQuery() || request.url().hasFragment()) return 404;
    if (request.method() != QHttpServerRequest::Method::Post) return 405;
    const auto headers = request.headers().values("X-Telegram-Bot-Api-Secret-Token");
    const auto expected = d->secret.toLatin1();
    if (!validSecret(d->secret) || headers.size() != 1 || headers.first().size() != expected.size()) return 403;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(headers.first().at(i) ^ expected.at(i));
    if (difference) return 403;
    if (request.body().size() > 256 * 1024) return 413;
    const auto document = QJsonDocument::fromJson(request.body());
    if (!document.isObject() || !positive(document.object().value("update_id"))) return 400;
    if (!eligiblePrivateMessage(document.object(), d->allowed)) return 200;
    const auto command = document.object().value("message").toObject().value("text").toString();
    const auto admit = d->admit;
    const QPointer<TelegramWebhookManager> guard(this);
#if PUBLICS_TELEGRAM_DEBUG_PAYLOADS
    if (!admit) {
        logTelegramIncomingUpdate(document.object(), true);
        if (!guard || !eligiblePrivateMessage(document.object(), d->allowed)) return 200;
    }
#endif
    const bool accepted = !admit || admit(document.object());
    if (!guard) return accepted ? 200 : 503;
    if (!accepted) return 503;
    emit commandReceived(command);
    return 200;
}
QNetworkReply *TelegramWebhookManager::registerWebhook()
{
    static const QRegularExpression tokenFormat(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (d->registration) return nullptr;
    if (!d->validRoot() || !safeWebhook(d->webhook) || !validSecret(d->secret)
        || !tokenFormat.match(d->token).hasMatch() || d->options.requestTimeoutMs <= 0
        || (d->options.requestTimeoutMs != 10000 && !d->options.allowLoopbackProviderForTests)) {
        emit errorOccurred(QStringLiteral("Invalid Telegram webhook configuration.")); return nullptr;
    }
    auto url = d->options.apiRoot;
    url.setPath(QStringLiteral("/bot") + d->token + QStringLiteral("/setWebhook"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    const QJsonObject payload{{"url", d->webhook.toString(QUrl::FullyEncoded)}, {"secret_token", d->secret},
        {"allowed_updates", QJsonArray{"message"}}, {"max_connections", 1}, {"drop_pending_updates", false}};
    auto *reply = d->network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    d->registration = reply;
    reply->setReadBufferSize(64 * 1024);
    struct Receipt { QByteArray bytes; QElapsedTimer elapsed; bool oversized = false; };
    const auto receiptData = std::make_shared<Receipt>();
    receiptData->elapsed.start();
    // Publish a bounded snapshot for the Agent's retained-receipt lifecycle path.
    // One reader owns the network bytes, so standalone and hosted reports agree.
    reply->setProperty("webhookResponseBody", QByteArray{});
    const auto consume = [reply, receiptData] {
        if (!reply->isOpen() || receiptData->oversized) return;
        receiptData->bytes += reply->read(1024 * 1024 + 1 - receiptData->bytes.size());
        reply->setProperty("webhookResponseBody", receiptData->bytes);
        if (receiptData->bytes.size() > 1024 * 1024) {
            receiptData->oversized = true; reply->abort();
        }
    };
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true); deadline->setTimerType(Qt::PreciseTimer);
    connect(deadline, &QTimer::timeout, reply, [reply] { reply->setProperty("webhookTimedOut", true); reply->abort(); });
    connect(reply, &QIODevice::readyRead, reply, consume);
    deadline->start(d->options.requestTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply, deadline, consume, receiptData] {
        deadline->stop(); d->registration = nullptr;
        consume();
        const auto receipt = QJsonDocument::fromJson(receiptData->bytes).object();
        const bool complete = !receiptData->oversized && !reply->property("webhookTimedOut").toBool()
            && receiptData->elapsed.elapsed() < d->options.requestTimeoutMs;
        const bool confirmed = complete && reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && receipt.value("ok") == QJsonValue(true) && receipt.value("result") == QJsonValue(true);
        const bool rejected = complete && reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && receipt.value("ok") == QJsonValue(true) && receipt.value("result") == QJsonValue(false);
        reply->deleteLater();
        // abort() emits finished synchronously. Public observers may destroy this
        // manager (and its network owner), so notify only after that stack returns.
        QTimer::singleShot(0, this, [this, confirmed, uncertain = !confirmed && !rejected] {
            emit registrationFinished(confirmed, uncertain);
        });
    });
    return reply;
}
