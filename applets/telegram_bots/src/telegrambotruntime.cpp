#include "telegrambotruntime.h"

#include <QHostAddress>
#include <QPointer>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QQueue>
#include <QTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <functional>

struct TelegramBotRuntime::State
{
    TelegramBotRuntime *q;
    QUrl root;
    QNetworkAccessManager network;
    QString token;
    bool active = false;
    bool starting = false;
    quint64 generation = 0;
    QSet<QNetworkReply *> pending;
    QTimer pollTimer;
    QQueue<QPair<qint64, qint64>> acknowledgements; // update id, chat id
    // Token-free, bounded in-memory attempt ledger survives explicit restarts.
    // This is not durable exactly-once delivery across process lifetimes.
    QSet<QPair<qint64, qint64>> attempted; // bot id, update id
    QQueue<QPair<qint64, qint64>> attemptOrder;
    qint64 botId = 0;
    qint64 offset = 0;

    State(TelegramBotRuntime *owner, QUrl url) : q(owner), root(std::move(url))
    {
        pollTimer.setSingleShot(true);
        QObject::connect(&pollTimer, &QTimer::timeout, q, [this] { poll(); });
    }
    bool safeRoot() const
    {
        if (!root.isValid() || !root.userInfo().isEmpty() || root.hasQuery() || root.hasFragment()
            || (!root.path().isEmpty() && root.path() != QStringLiteral("/"))) return false;
        if (root.scheme() == QStringLiteral("https") && root.host() == QStringLiteral("api.telegram.org")
            && (root.port() == -1 || root.port() == 443)) return true;
        // Literal IPs only: no DNS/proxy-dependent "localhost" resolution.
        const QHostAddress address(root.host());
        return root.scheme() == QStringLiteral("http") && address.isLoopback();
    }
    void poll()
    {
        if (!active) return;
        request(QStringLiteral("getUpdates"), {{"offset", offset}, {"timeout", 25},
            {"limit", 100}, {"allowed_updates", QJsonArray{"message"}}}, [this](QJsonValue value) {
            if (!value.isArray() || value.toArray().size() > 100) {
                fail(QStringLiteral("Telegram returned invalid updates.")); return;
            }
            // Validate the complete batch before making any mutations, then order
            // and deduplicate by the provider's update identity, not message text.
            QMap<qint64, QJsonObject> updates;
            for (const auto &entry : value.toArray()) {
                const auto update = entry.toObject();
                const auto id = update.value("update_id").toInteger(-1);
                if (!entry.isObject() || id <= 0 || id >= (qint64(1) << 52)) {
                    fail(QStringLiteral("Telegram returned invalid update identities.")); return;
                }
                if (!updates.contains(id)) updates.insert(id, update);
            }
            for (auto it = updates.cbegin(); it != updates.cend(); ++it) {
                const auto id = it.key();
                if (id < offset) continue;
                offset = id + 1;
                const auto message = it.value().value("message").toObject();
                const auto from = message.value("from").toObject();
                const auto chat = message.value("chat").toObject();
                if (chat.value("type").toString() == QStringLiteral("private")
                    && !message.contains("sender_chat") && !message.contains("sender_business_bot")
                    && !message.contains("business_connection_id") && !message.contains("guest_query_id")
                    && message.value("message_id").toInteger() > 0 && message.value("date").toInteger() > 0
                    && from.value("is_bot").isBool() && !from.value("is_bot").toBool()
                    && from.value("id").toInteger() > 0
                    && from.value("id").toInteger() == chat.value("id").toInteger(-1)
                    && !attempted.contains({botId, id}))
                    acknowledgements.enqueue({id, chat.value("id").toInteger()});
            }
            sendNext();
        });
    }
    void sendNext()
    {
        if (!active) return;
        if (acknowledgements.isEmpty()) { pollTimer.start(250); return; }
        const auto acknowledgement = acknowledgements.dequeue();
        const auto chat = acknowledgement.second;
        const QPair<qint64, qint64> key{botId, acknowledgement.first};
        attempted.insert(key); // Mark before dispatch: a lost receipt is not safe to retry.
        attemptOrder.enqueue(key);
        if (attemptOrder.size() > 1024) attempted.remove(attemptOrder.dequeue());
        request(QStringLiteral("sendMessage"), {{"chat_id", chat}, {"text", "Message received."}},
            [this, chat](QJsonValue value) {
                const auto message = value.toObject();
                const auto receiptChat = message.value("chat").toObject();
                if (!message.value("message_id").isDouble() || message.value("message_id").toInteger(-1) <= 0
                    || message.value("date").toInteger() <= 0
                    || receiptChat.value("id").toInteger(-1) != chat
                    || receiptChat.value("type").toString() != QStringLiteral("private")
                    || (message.contains("text") && message.value("text").toString() != QStringLiteral("Message received."))) {
                    fail(QStringLiteral("Telegram acknowledgement delivery is uncertain; invalid receipt. Not retried."));
                    return;
                }
                sendNext();
            });
    }
    void stop(bool notify = true)
    {
        ++generation;
        const bool changed = active || starting;
        active = starting = false;
        token.clear();
        pollTimer.stop();
        acknowledgements.clear();
        offset = 0;
        botId = 0;
        const auto requests = pending;
        pending.clear();
        for (auto *reply : requests) {
            QObject::disconnect(reply, nullptr, q, nullptr);
            for (auto *timer : reply->findChildren<QTimer *>()) timer->stop();
            reply->abort();
            reply->deleteLater();
        }
        if (notify && changed) emit q->stateChanged();
    }
    void fail(const QString &message)
    {
        const bool changed = active || starting;
        stop(false);
        const auto run = generation;
        const QPointer<TelegramBotRuntime> guard(q);
        if (changed) emit q->stateChanged();
        if (guard && generation == run) emit q->errorOccurred(message);
    }
    void request(const QString &method, QJsonObject body, std::function<void(QJsonValue)> done)
    {
        QUrl url = root;
        url.setPath(QStringLiteral("/bot") + token + QLatin1Char('/') + method);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setDecompressedSafetyCheckThreshold(1024 * 1024);
        auto *reply = network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        reply->setReadBufferSize(64 * 1024);
        pending.insert(reply);
        const auto run = generation;
        struct Response { QByteArray bytes; bool oversized = false; bool timedOut = false; };
        const auto response = std::make_shared<Response>();
        const auto consume = [reply, response] {
            constexpr qsizetype limit = 1024 * 1024;
            if (!reply->isOpen() || response->oversized) return;
            response->bytes += reply->read(limit + 1 - response->bytes.size());
            if (response->bytes.size() > limit) {
                response->oversized = true;
                reply->abort();
            }
        };
        auto *deadline = new QTimer(reply);
        deadline->setSingleShot(true);
        deadline->setTimerType(Qt::PreciseTimer);
        QObject::connect(deadline, &QTimer::timeout, q, [this, reply, run, response] {
            if (generation != run) return;
            response->timedOut = true;
            reply->abort();
        });
        // Absolute deadlines also bound peers that drip bytes indefinitely.
        deadline->start(method == QStringLiteral("getUpdates") ? 35000 : 10000);
        QObject::connect(reply, &QIODevice::readyRead, q, consume);
        QObject::connect(reply, &QNetworkReply::finished, q,
            [this, reply, deadline, run, response, consume, method, done = std::move(done)] {
            deadline->stop();
            pending.remove(reply);
            reply->deleteLater();
            if (generation != run) return;
            consume();
            const auto reject = [this, &method](QString message) {
                // Never expose URLs, provider descriptions, raw bodies or errorString().
                if (method == QStringLiteral("sendMessage"))
                    message += QStringLiteral(" Acknowledgement delivery is uncertain; not retried.");
                fail(message);
            };
            if (response->oversized) { reject(QStringLiteral("Telegram response exceeded size limit.")); return; }
            if (response->timedOut) { reject(QStringLiteral("Telegram request timed out.")); return; }
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) { reject(QStringLiteral("Telegram redirect refused.")); return; }
            const auto doc = QJsonDocument::fromJson(response->bytes);
            const auto object = doc.object();
            const int code = status >= 400 ? status : object.value("error_code").toInt();
            switch (code) {
            case 401: reject(QStringLiteral("Telegram 401: bot token rejected.")); return;
            case 403: reject(QStringLiteral("Telegram 403: access forbidden.")); return;
            case 409: reject(QStringLiteral("Telegram 409: polling conflict; check other pollers or an existing webhook.")); return;
            case 429: reject(QStringLiteral("Telegram 429: rate limited; wait before starting again.")); return;
            default: break;
            }
            if (reply->error() != QNetworkReply::NoError || status == 0) {
                reject(QStringLiteral("Telegram network or service request failed.")); return;
            }
            if (status != 200 || !doc.isObject() || object.value("ok") != QJsonValue(true)
                || !object.contains("result")) {
                reject(QStringLiteral("Telegram returned an invalid or rejected response.")); return;
            }
            done(object.value("result"));
        });
    }
};

TelegramBotRuntime::TelegramBotRuntime(QObject *parent)
    : TelegramBotRuntime(QUrl(QStringLiteral("https://api.telegram.org")), parent) {}
TelegramBotRuntime::TelegramBotRuntime(QUrl apiRoot, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, std::move(apiRoot))) {}
TelegramBotRuntime::~TelegramBotRuntime() { d->stop(false); }
bool TelegramBotRuntime::active() const { return d->active; }
bool TelegramBotRuntime::starting() const { return d->starting; }
void TelegramBotRuntime::stop() { d->stop(); }
void TelegramBotRuntime::start(QString token)
{
    const bool hadState = d->active || d->starting;
    d->stop(false);
    static const QRegularExpression format(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (!d->safeRoot() || !format.match(token).hasMatch()) {
        const auto run = d->generation;
        const QPointer<TelegramBotRuntime> guard(this);
        if (hadState) emit stateChanged();
        if (guard && d->generation == run)
            emit errorOccurred(QStringLiteral("Invalid Telegram endpoint or bot token format."));
        return;
    }
    d->token = std::move(token);
    d->starting = true;
    const auto run = d->generation;
    const QPointer<TelegramBotRuntime> guard(this);
    emit stateChanged();
    if (!guard || d->generation != run || !d->starting) return;
    d->request(QStringLiteral("getMe"), {}, [this](QJsonValue value) {
        const auto bot = value.toObject();
        if (!bot.value("is_bot").toBool() || !bot.value("id").isDouble()
            || bot.value("id").toInteger(-1) != d->token.section(QLatin1Char(':'), 0, 0).toLongLong()
            || bot.value("first_name").toString().isEmpty()) {
            d->fail(QStringLiteral("Telegram did not return a valid bot identity."));
            return;
        }
        d->botId = bot.value("id").toInteger();
        d->starting = false;
        d->active = true;
        const auto run = d->generation;
        const QPointer<TelegramBotRuntime> guard(this);
        emit stateChanged();
        if (guard && d->generation == run && d->active) d->poll();
    });
}
