#include "telegramagent.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkProxy>
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
#include <QUuid>
#include <QUrlQuery>
#include <QDebug>
#include <functional>

struct TelegramAgent::State
{
    TelegramAgent *q;
    QUrl root;
    QUrl healthEndpoint;
    QNetworkAccessManager localNetwork;
    bool healthy = false;
    QNetworkAccessManager network;
    QString token;
    bool active = false;
    bool starting = false;
    bool stopping = false;
    bool dispatchBusy = false;
    WebhookOptions options{false, {}};
    QPointer<TelegramWebhookHost> host;
    QString clientId;
    QUrl ownedWebhook;
    QPointer<QNetworkReply> registrationReply;
    bool registrationUncertain = false;
    bool hostReleased = true;
    bool cleanupDone = true;
    QTimer cleanupTimer;
    QSet<qint64> admitted;
    QQueue<qint64> admissionOrder;
    quint64 generation = 0;
    QSet<QNetworkReply *> pending;
    QTimer pollTimer;
    struct PendingReply { qint64 updateId; qint64 chatId; bool statusRequested; };
    QQueue<PendingReply> acknowledgements;
    // Token-free, bounded in-memory attempt ledger survives explicit restarts.
    // This is not durable exactly-once delivery across process lifetimes.
    QSet<QPair<qint64, qint64>> attempted; // bot id, update id
    QQueue<QPair<qint64, qint64>> attemptOrder;
    qint64 botId = 0;
    qint64 offset = 0;

    State(TelegramAgent *owner, QUrl url, QUrl health)
        : q(owner), root(std::move(url)), healthEndpoint(std::move(health))
    {
        localNetwork.setProxy(QNetworkProxy::NoProxy);
        pollTimer.setSingleShot(true);
        QObject::connect(&pollTimer, &QTimer::timeout, q, [this] { poll(); });
        cleanupTimer.setSingleShot(true);
        cleanupTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&cleanupTimer, &QTimer::timeout, q, [this] { finishCleanup(false); });
    }
    void configureWebhook(WebhookOptions value)
    {
        options = std::move(value);
        if (!options.enabled) return;
        host = TelegramWebhookHost::instance();
        QObject::connect(host, &TelegramWebhookHost::ready, q,
            [this](const QString &id, const QUrl &url, const QString &secret) {
            if (id != clientId || !starting || stopping) return;
            if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()
                || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() || secret.isEmpty()) {
                fail(QStringLiteral("Telegram webhook tunnel returned invalid configuration.")); return;
            }
            ownedWebhook = url;
            qDebug("Telegram webhook registration phase");
            request(QStringLiteral("setWebhook"), {{"url", url.toString(QUrl::FullyEncoded)},
                {"secret_token", secret}, {"allowed_updates", QJsonArray{"message"}},
                {"max_connections", 1}, {"drop_pending_updates", false}}, [this](QJsonValue result) {
                if (result != QJsonValue(true)) {
                    fail(QStringLiteral("Telegram webhook registration was not confirmed.")); return;
                }
                request(QStringLiteral("getWebhookInfo"), {}, [this](QJsonValue info) {
                    if (!info.isObject() || !info.toObject().value("url").isString()
                        || info.toObject().value("url").toString() != ownedWebhook.toString(QUrl::FullyEncoded)) {
                        fail(QStringLiteral("Telegram webhook readback did not match.")); return;
                    }
                    activate();
                });
            });
        });
        QObject::connect(host, &TelegramWebhookHost::failed, q,
            [this](const QString &id, const QString &message) {
            if (id == clientId && !stopping && (active || starting))
                fail(message); // Host messages are fixed, sanitized diagnostics.
        });
        QObject::connect(host, &TelegramWebhookHost::released, q, [this](const QString &id) {
            if (id != clientId) return;
            hostReleased = true;
            QTimer::singleShot(0, q, [this] { finishStop(); });
        });
        QObject::connect(host, &TelegramWebhookHost::updateReceived, q,
            [this](const QString &id, const QJsonObject &update) {
            // Telegram may POST as soon as setWebhook commits, before readback.
            // Admit authenticated arrivals now; dispatch waits for verified activation.
            if (id != clientId || (!active && !starting) || stopping) return;
            admit(update);
            sendNext();
        });
    }
    void activate()
    {
        starting = false;
        active = healthy = true;
        const auto run = generation;
        const QPointer<TelegramAgent> guard(q);
        qDebug("Telegram transport active");
        emit q->stateChanged();
        if (!guard || generation != run || !active) return;
        if (options.enabled) sendNext();
        else poll();
    }
    void acquireHost()
    {
        clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ownedWebhook = QUrl{};
        registrationUncertain = false;
        admitted.clear(); admissionOrder.clear();
        hostReleased = false;
        host->acquire(clientId, options.tunnel);
    }
    void admit(const QJsonObject &update)
    {
        const auto id = update.value("update_id").toInteger(-1);
        if (id <= 0 || id >= (qint64(1) << 52) || admitted.contains(id)
            || attempted.contains({botId, id}) || acknowledgements.size() >= 100) return;
        const auto message = update.value("message").toObject();
        const auto from = message.value("from").toObject();
        const auto chat = message.value("chat").toObject();
        if (chat.value("type").toString() != QStringLiteral("private")
            || message.contains("sender_chat") || message.contains("sender_business_bot")
            || message.contains("business_connection_id") || message.contains("guest_query_id")
            || message.value("message_id").toInteger() <= 0 || message.value("date").toInteger() <= 0
            || !from.value("is_bot").isBool() || from.value("is_bot").toBool()
            || from.value("id").toInteger() <= 0
            || from.value("id").toInteger() != chat.value("id").toInteger(-1)) return;
        admitted.insert(id);
        admissionOrder.enqueue(id);
        if (admissionOrder.size() > 1024) admitted.remove(admissionOrder.dequeue());
        const auto text = message.value("text").toString().trimmed();
        acknowledgements.enqueue({id, chat.value("id").toInteger(),
            text == QStringLiteral("status") || text == QStringLiteral("/status")});
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
    bool safeHealthEndpoint() const
    {
        if (!healthEndpoint.isValid() || healthEndpoint.scheme() != QStringLiteral("http")
            || !healthEndpoint.userInfo().isEmpty() || healthEndpoint.hasQuery()
            || healthEndpoint.hasFragment()) return false;
        return healthEndpoint.host() == QStringLiteral("localhost")
            || QHostAddress(healthEndpoint.host()).isLoopback();
    }
    void poll()
    {
        if (!active || options.enabled) return;
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
                    && !attempted.contains({botId, id})) {
                    // Public, read-only bot liveness only; no application/domain commands.
                    const auto text = message.value("text").toString().trimmed();
                    const bool statusRequested = text == QStringLiteral("status")
                        || text == QStringLiteral("/status");
                    acknowledgements.enqueue({id, chat.value("id").toInteger(), statusRequested});
                }
            }
            sendNext();
        });
    }
    void sendNext()
    {
        if (!active || dispatchBusy) return;
        if (acknowledgements.isEmpty()) { if (!options.enabled) pollTimer.start(250); return; }
        dispatchBusy = true;
        const auto acknowledgement = acknowledgements.dequeue();
        const auto chat = acknowledgement.chatId;
        const QPair<qint64, qint64> key{botId, acknowledgement.updateId};
        attempted.insert(key); // Mark before dispatch: a lost receipt is not safe to retry.
        attemptOrder.enqueue(key);
        if (attemptOrder.size() > 1024) attempted.remove(attemptOrder.dequeue());
        if (acknowledgement.statusRequested) {
            checkHealth(chat);
            return;
        }
        sendText(chat, QStringLiteral("Message received."));
    }
    void checkHealth(qint64 chat)
    {
        // Dedicated manager: a Telegram long poll cannot consume local capacity.
        QNetworkRequest request(healthEndpoint);
        if (options.enabled && host && host->ownsHealthUrl(healthEndpoint))
            request.setRawHeader("X-PTI-Health-Secret", host->healthSecret());
        request.setRawHeader("Accept-Encoding", "identity");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setDecompressedSafetyCheckThreshold(64 * 1024);
        auto *reply = localNetwork.get(request);
        reply->setReadBufferSize(16 * 1024);
        pending.insert(reply);
        const auto run = generation;
        struct Operation {
            bool settled = false;
            qsizetype received = 0;
            QElapsedTimer elapsed;
        };
        const auto operation = std::make_shared<Operation>();
        operation->elapsed.start();
        auto *deadline = new QTimer(reply);
        deadline->setSingleShot(true);
        deadline->setTimerType(Qt::PreciseTimer);
        const auto settle = [this, reply, deadline, run, chat, operation](bool result) {
            if (operation->settled) return;
            // abort() may synchronously emit finished; claim the winner first.
            operation->settled = true;
            deadline->stop();
            pending.remove(reply);
            QObject::disconnect(reply, nullptr, q, nullptr);
            if (!reply->isFinished()) reply->abort();
            reply->deleteLater();
            if (generation != run || !active) return;
            const bool changed = healthy != result;
            healthy = result;
            const QPointer<TelegramAgent> guard(q);
            if (changed) emit q->stateChanged();
            if (!guard || generation != run || !active) return;
            sendText(chat, result ? QStringLiteral("Active") : QStringLiteral("Inactive"));
        };
        const auto consume = [reply, operation] {
            constexpr qsizetype limit = 64 * 1024;
            if (reply->isOpen()) operation->received += reply->read(limit + 1 - operation->received).size();
            return operation->received <= limit;
        };
        QObject::connect(deadline, &QTimer::timeout, q, [settle] { settle(false); });
        QObject::connect(reply, &QIODevice::readyRead, q, [operation, consume, settle] {
            if (!operation->settled && (!consume() || operation->elapsed.hasExpired(2999))) settle(false);
        });
        QObject::connect(reply, &QNetworkReply::finished, q, [reply, operation, consume, settle] {
            if (operation->settled) return;
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            settle(consume() && !operation->elapsed.hasExpired(2999)
                && reply->error() == QNetworkReply::NoError && status >= 200 && status < 300);
        });
        deadline->start(3000); // Absolute, never refreshed by a dripping peer.
    }
    void sendText(qint64 chat, const QString &text)
    {
        request(QStringLiteral("sendMessage"), {{"chat_id", chat}, {"text", text}},
            [this, chat, text](QJsonValue value) {
                const auto message = value.toObject();
                const auto receiptChat = message.value("chat").toObject();
                if (!message.value("message_id").isDouble() || message.value("message_id").toInteger(-1) <= 0
                    || message.value("date").toInteger() <= 0
                    || receiptChat.value("id").toInteger(-1) != chat
                    || receiptChat.value("type").toString() != QStringLiteral("private")
                    || (message.contains("text") && message.value("text").toString() != text)) {
                    fail(QStringLiteral("Telegram acknowledgement delivery is uncertain; invalid receipt. Not retried."));
                    return;
                }
                dispatchBusy = false;
                sendNext();
            });
    }
    void cancelRequests(bool preserveRegistration = false)
    {
        const auto requests = pending;
        pending.clear();
        for (auto *reply : requests) {
            if (preserveRegistration && reply == registrationReply) {
                pending.insert(reply);
                continue;
            }
            QObject::disconnect(reply, nullptr, q, nullptr);
            for (auto *timer : reply->findChildren<QTimer *>()) timer->stop();
            reply->abort();
            reply->deleteLater();
        }
    }
    void finishStop()
    {
        if (!stopping || !hostReleased || !cleanupDone) return;
        token.clear();
        ownedWebhook = QUrl{};
        registrationReply = nullptr;
        clientId.clear();
        stopping = false;
        qDebug("Telegram webhook stop complete");
        emit q->stateChanged(); // Final old-run mutation; observer may restart or delete.
    }
    void finishCleanup(bool confirmed)
    {
        if (!stopping || cleanupDone) return;
        cleanupDone = true;
        cleanupTimer.stop();
        cancelRequests();
        token.clear();
        const QPointer<TelegramAgent> guard(q);
        if (!confirmed) emit q->errorOccurred(QStringLiteral("Telegram webhook remote cleanup is unknown; not retried."));
        if (guard) finishStop();
    }
    void cleanupRemote()
    {
        request(QStringLiteral("getWebhookInfo"), {}, [this](QJsonValue info) {
            if (!info.isObject() || !info.toObject().value("url").isString()) { finishCleanup(false); return; }
            if (info.toObject().value("url").toString() != ownedWebhook.toString(QUrl::FullyEncoded)) {
                finishCleanup(!registrationUncertain); return; // Do not delete another owner's replacement.
            }
            registrationUncertain = false; // Readback proves this registration committed.
            request(QStringLiteral("deleteWebhook"), {{"drop_pending_updates", false}}, [this](QJsonValue result) {
                if (result != QJsonValue(true)) { finishCleanup(false); return; }
                request(QStringLiteral("getWebhookInfo"), {}, [this](QJsonValue current) {
                    finishCleanup(current.isObject() && current.toObject().value("url").isString()
                        && current.toObject().value("url").toString() != ownedWebhook.toString(QUrl::FullyEncoded));
                });
            });
        });
    }
    void stop(bool notify = true)
    {
        if (stopping) return;
        ++generation;
        const bool changed = active || starting || healthy;
        active = starting = healthy = false;
        dispatchBusy = false;
        pollTimer.stop();
        acknowledgements.clear();
        offset = 0;
        botId = 0;
        // Aborting setWebhook cannot retract an accepted remote mutation.
        cancelRequests(options.enabled && !clientId.isEmpty());
        if (options.enabled && !clientId.isEmpty()) {
            stopping = true;
            cleanupDone = ownedWebhook.isEmpty();
            const auto run = generation;
            const QPointer<TelegramAgent> guard(q);
            // Release removes our local route immediately; last-lease completion is asynchronous.
            if (host && !hostReleased) host->release(clientId);
            else hostReleased = true;
            if (!guard || generation != run) return;
            if (!cleanupDone) {
                cleanupTimer.start(15000);
                if (!registrationReply) cleanupRemote();
            }
            if (notify && changed) emit q->stateChanged();
            if (guard && generation == run) finishStop();
            return;
        }
        token.clear();
        if (notify && changed) emit q->stateChanged();
    }
    void destroy()
    {
        ++generation;
        active = starting = healthy = false;
        cleanupTimer.stop(); pollTimer.stop();
        cancelRequests();
        if (host) {
            QObject::disconnect(host, nullptr, q, nullptr);
            if (!clientId.isEmpty() && !hostReleased) host->release(clientId);
        }
        token.clear();
    }
    void fail(const QString &message)
    {
        qDebug().noquote() << "Telegram agent error:" << message;
        if (stopping) { finishCleanup(false); return; }
        const bool changed = active || starting || healthy;
        const auto run = generation + 1; // stop retires exactly this run, even if an observer restarts.
        const QPointer<TelegramAgent> guard(q);
        stop(false);
        if (!guard || generation != run) return;
        if (changed) emit q->stateChanged();
        if (guard && generation == run) emit q->errorOccurred(message);
    }
    void request(const QString &method, QJsonObject body, std::function<void(QJsonValue)> done)
    {
        QUrl url = root;
        url.setPath(QStringLiteral("/bot") + token + QLatin1Char('/') + method);
        const bool get = options.enabled && (method == QStringLiteral("setWebhook")
            || method == QStringLiteral("getWebhookInfo") || method == QStringLiteral("deleteWebhook"));
        if (get) {
            QUrlQuery query;
            for (auto it = body.cbegin(); it != body.cend(); ++it) {
                const auto value = it.value();
                QString encoded;
                if (value.isArray()) encoded = QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
                else if (value.isString()) encoded = value.toString();
                else if (value.isBool()) encoded = value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
                else encoded = QString::number(value.toInteger());
                query.addQueryItem(it.key(), encoded);
            }
            url.setQuery(query);
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setDecompressedSafetyCheckThreshold(1024 * 1024);
        auto *reply = get ? network.get(request)
                          : network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        reply->setReadBufferSize(64 * 1024);
        pending.insert(reply);
        if (method == QStringLiteral("setWebhook")) registrationReply = reply;
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
            consume();
            if (registrationReply == reply) {
                registrationReply = nullptr;
                const auto receipt = QJsonDocument::fromJson(response->bytes).object();
                registrationUncertain = response->oversized || response->timedOut
                    || reply->error() != QNetworkReply::NoError
                    || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
                    || receipt.value("ok") != QJsonValue(true) || !receipt.value("result").isBool();
                if (stopping && generation != run) {
                    if (!cleanupDone) cleanupRemote();
                    return;
                }
            }
            if (generation != run) return;
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

TelegramAgent::TelegramAgent(QObject *parent)
    : TelegramAgent(QUrl(QStringLiteral("https://api.telegram.org")),
        QUrl(QStringLiteral("http://localhost:8080/health")), WebhookOptions{}, parent) {}
TelegramAgent::TelegramAgent(QUrl apiRoot, QObject *parent)
    : TelegramAgent(std::move(apiRoot), QUrl(QStringLiteral("http://localhost:8080/health")), parent) {}
TelegramAgent::TelegramAgent(QUrl apiRoot, QUrl healthEndpoint, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, std::move(apiRoot), std::move(healthEndpoint))) {}
TelegramAgent::TelegramAgent(QUrl apiRoot, QUrl healthEndpoint, WebhookOptions options, QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this, std::move(apiRoot), std::move(healthEndpoint)))
{ d->configureWebhook(std::move(options)); }
TelegramAgent::~TelegramAgent() { d->destroy(); }
bool TelegramAgent::healthy() const { return d->healthy; }
bool TelegramAgent::active() const { return d->active; }
bool TelegramAgent::starting() const { return d->starting; }
bool TelegramAgent::stopping() const { return d->stopping; }
void TelegramAgent::stop() { d->stop(); }
void TelegramAgent::start(QString token)
{
    if (d->stopping || (d->options.enabled && (d->active || d->starting))) {
        emit errorOccurred(QStringLiteral("Telegram transport is busy; wait for stop completion."));
        return;
    }
    const bool hadState = d->active || d->starting;
    d->stop(false);
    static const QRegularExpression format(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (!d->safeRoot() || !d->safeHealthEndpoint() || !format.match(token).hasMatch()) {
        const auto run = d->generation;
        const QPointer<TelegramAgent> guard(this);
        if (hadState) emit stateChanged();
        if (guard && d->generation == run)
            emit errorOccurred(QStringLiteral("Invalid Telegram endpoint or bot token format."));
        return;
    }
    d->token = std::move(token);
    d->starting = true;
    const auto run = d->generation;
    const QPointer<TelegramAgent> guard(this);
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
        if (d->options.enabled) d->acquireHost();
        else d->activate();
    });
}
