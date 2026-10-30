#include "telegramagent.h"
#include "telegramdebuglogging.h"
#include "telegramwebhookmanager.h"

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
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr qint64 maxIdentity = (qint64(1) << 52) - 1;
bool positiveId(const QJsonValue &value)
{
    return value.isDouble() && value.toInteger(-1) > 0 && value.toInteger(-1) <= maxIdentity;
}
}

struct TelegramAgent::State
{
    TelegramAgent *q;
    QUrl root;
    QUrl healthEndpoint;
    QNetworkAccessManager localNetwork;
    bool healthy = false;
    QNetworkAccessManager network;
    QString token;
    QString sessionSecret;
    std::unique_ptr<TelegramWebhookManager> manager;
    bool active = false;
    bool starting = false;
    bool stopping = false;
    bool dispatchBusy = false;
    WebhookOptions options{false, {}};
    QPointer<TelegramWebhookHost> host;
    QString clientId;
    bool inspectorChoicePending = false;
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
    struct PendingReply {
        qint64 updateId;
        qint64 chatId;
        bool statusRequested;
        QString side;
        QStringList symbols;
    };
    QQueue<PendingReply> acknowledgements;
    QSet<qint64> allowedChatIds;
    QString ledgerPath;
    bool ledgerPathInjected = false;
    QString tradeTicket;
    qint64 tradeChat = 0;
    QQueue<QString> tradeReports;
    void sendTradeReport()
    {
        if (!active || tradeReports.isEmpty()) return;
        const auto text = tradeReports.dequeue();
        sendText(tradeChat, text, true);
    }
    // Small, auditable HTML subset. Unknown markup is data, not executable HTML.
    static QStringList chunks(const QString &input)
    {
        QStringList result;
        QString chunk;
        bool bold = false;
        for (qsizetype i = 0; i < input.size();) {
            QString token;
            bool nextBold = bold;
            const auto tail = QStringView(input).mid(i);
            if (!bold && tail.startsWith(u"<b>")) { token = QStringLiteral("<b>"); i += 3; nextBold = true; }
            else if (bold && tail.startsWith(u"</b>")) { token = QStringLiteral("</b>"); i += 4; nextBold = false; }
            else {
                for (const auto &entity : {QStringLiteral("&amp;"), QStringLiteral("&lt;"), QStringLiteral("&gt;"), QStringLiteral("&quot;")}) {
                    if (tail.startsWith(entity)) { token = entity; i += entity.size(); break; }
                }
                if (token.isEmpty()) {
                    qsizetype length = 1;
                    if (input[i].isHighSurrogate() && i + 1 < input.size() && input[i + 1].isLowSurrogate()) length = 2;
                    token = input.mid(i, length).toHtmlEscaped();
                    i += length;
                }
            }
            if (chunk.size() + token.size() + (nextBold ? 4 : 0) > 4096) {
                result.append(chunk + (bold ? QStringLiteral("</b>") : QString{}));
                chunk = bold ? QStringLiteral("<b>") : QString{};
            }
            chunk += token;
            bold = nextBold;
        }
        if (!chunk.isEmpty()) result.append(chunk + (bold ? QStringLiteral("</b>") : QString{}));
        return result;
    }
    // Bounded RAM fast path retained for legacy notifications. Trade replay safety
    // comes from the durable claim, never from this evictable cache.
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
        QObject::connect(host, &TelegramWebhookHost::inspectorConflict, q, [this](const QString &id) {
            if (id != clientId || !starting || stopping || inspectorChoicePending) return;
            inspectorChoicePending = true;
            const auto run = generation;
            const QPointer<TelegramAgent> owner(q);
            QTimer::singleShot(0, q, [owner, run, id] {
                if (!owner || owner->d->generation != run || owner->d->clientId != id
                    || !owner->d->starting || owner->d->stopping || !owner->d->inspectorChoicePending) return;
                emit owner->inspectorPortConflict();
            });
        });
        QObject::connect(host, &TelegramWebhookHost::ready, q,
            [this](const QString &id, const QUrl &url, const QString &secret) {
            if (id != clientId || !starting || stopping) return;
            if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()
                || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery() || secret.isEmpty()) {
                fail(QStringLiteral("Telegram webhook tunnel returned invalid configuration.")); return;
            }
            ownedWebhook = url;
            if (!manager || secret != sessionSecret) {
                fail(QStringLiteral("Telegram webhook secret did not match this session.")); return;
            }
            manager->setWebhookUrl(url);
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

    }
    void activate()
    {
        inspectorChoicePending = false;
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
        const QPointer<TelegramAgent> owner(q);
        const auto run = generation;
        TelegramWebhookManager::Options managerOptions;
        managerOptions.externallyHosted = true;
        managerOptions.apiRoot = root;
        managerOptions.allowLoopbackProviderForTests = root.scheme() == QStringLiteral("http")
            && QHostAddress(root.host()).isLoopback();
        manager = std::make_unique<TelegramWebhookManager>(token, sessionSecret, QString{}, 0, managerOptions);
        QStringList policy;
        for (const auto id : allowedChatIds) policy.append(QString::number(id));
        manager->setAllowedChatIds(policy);
        manager->setAdmissionHandler([owner, run](const QJsonObject &update) {
            if (!owner || owner->d->generation != run || owner->d->stopping
                || (!owner->d->active && !owner->d->starting)) return false;
            if (!owner->d->admit(update)) return false;
            // Dispatch outside the HTTP callback; admission is already durable.
            QTimer::singleShot(0, owner, [owner, run] {
                if (owner && owner->d->generation == run) owner->d->sendNext();
            });
            return true;
        });
        const QPointer<TelegramWebhookManager> intake(manager.get());
        host->acquire(clientId, options.tunnel, {}, sessionSecret, [intake](const QHttpServerRequest &request) {
            return intake ? intake->handleRequest(request) : 503;
        });
    }
    // Monotonic per-bot watermark: older out-of-order trades fail closed, even
    // after Telegram randomizes IDs following long inactivity. Never expire/reset
    // automatically. Availability must not enable replay of an admitted order.
    // Only bot/update IDs are persisted, never credentials, chats or policy.
    enum class Claim { Admitted, Duplicate, Error };
    Claim claimTrade(qint64 updateId)
    {
        QString path = ledgerPath;
        if (!ledgerPathInjected) {
            if (QHostAddress(root.host()).isLoopback()) return Claim::Error;
            const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (base.isEmpty()) return Claim::Error;
            path = QDir(base).filePath(QStringLiteral("telegram-trade-admission-v1"));
        }
        if (path.isEmpty() || !QDir().mkpath(path)) return Claim::Error;
        const auto fileName = QDir(path).filePath(QString::number(botId) + QStringLiteral(".watermark"));
        QLockFile lock(fileName + QStringLiteral(".lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) return Claim::Error;
        QFile existing(fileName);
        if (existing.exists()) {
            if (!existing.open(QIODevice::ReadOnly) || existing.size() > 128) return Claim::Error;
            const auto bytes = existing.readAll();
            if (existing.error() != QFileDevice::NoError) return Claim::Error;
            const auto parts = bytes.split('\n');
            bool ok = false;
            const auto previous = parts.value(2).toLongLong(&ok);
            if (parts.size() != 4 || parts[0] != "PTI-TRADE-1"
                || parts[1] != QByteArray::number(botId) || !parts[3].isEmpty()
                || !ok || previous <= 0 || previous > maxIdentity
                || parts[2] != QByteArray::number(previous)) return Claim::Error;
            if (updateId <= previous) return Claim::Duplicate;
        }
        QSaveFile file(fileName);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) return Claim::Error;
        const auto bytes = QByteArray("PTI-TRADE-1\n") + QByteArray::number(botId)
            + '\n' + QByteArray::number(updateId) + '\n';
        if (file.write(bytes) != bytes.size() || !file.commit()) return Claim::Error;
        return Claim::Admitted;
    }
    bool logIncoming(const QJsonObject &update)
    {
#if PUBLICS_TELEGRAM_DEBUG_PAYLOADS
        if (!TelegramWebhookManager::eligiblePrivateMessage(update, allowedChatIds)) return true;
        const QPointer<TelegramAgent> logGuard(q);
        const auto logRun = generation;
        logTelegramIncomingUpdate(update, options.enabled);
        return logGuard && generation == logRun && !stopping && (active || starting);
#else
        (void)update;
        return true;
#endif
    }
    bool admit(const QJsonObject &update)
    {
        if (!logIncoming(update)) return false;
        const auto identity = update.value("update_id");
        const auto id = identity.toInteger(-1);
        if (!positiveId(identity) || admitted.contains(id)
            || attempted.contains({botId, id})) return true;
        const auto message = update.value("message").toObject();
        const auto from = message.value("from").toObject();
        if (!TelegramWebhookManager::eligiblePrivateMessage(update, allowedChatIds)) return true;
        const auto chat = message.value("chat").toObject();
        if (chat.value("type").toString() != QStringLiteral("private")
            || message.contains("sender_chat") || message.contains("sender_business_bot")
            || message.contains("business_connection_id") || message.contains("guest_query_id")
            || !positiveId(message.value("message_id")) || !positiveId(message.value("date"))
            || !from.value("is_bot").isBool() || from.value("is_bot").toBool()
            || !positiveId(from.value("id")) || !positiveId(chat.value("id"))
            || from.value("id").toInteger() != chat.value("id").toInteger()) return true;
        const auto text = message.value("text").toString().trimmed();
        PendingReply item{id, chat.value("id").toInteger(),
            text == QStringLiteral("status") || text == QStringLiteral("/status"), {}, {}};
        // ASCII only: never case-fold arbitrary Unicode into a valid ticker.
        static const QRegularExpression command(QStringLiteral(
            "\\A/?publics_auto_(buy|sell)[ \t]+([A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?(?:[ \t]*,[ \t]*[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?)*)\\z"));
        if (message.value("text").toString().size() <= 1024 && allowedChatIds.contains(item.chatId)
            && !message.contains("forward_origin") && !message.contains("via_bot")
            && !message.contains("is_automatic_forward") && !message.contains("is_from_offline")
            && !message.contains("guest_bot_caller_user") && !message.contains("guest_bot_caller_chat")
            && !message.contains("message_thread_id") && !message.contains("is_topic_message")
            && !message.contains("direct_messages_topic") && !message.contains("edit_date")) {
            const auto match = command.match(text);
            if (match.hasMatch()) {
                auto symbols = match.captured(2).split(QLatin1Char(','));
                for (auto &symbol : symbols) symbol = symbol.trimmed().toUpper();
                if (symbols.size() <= 50) {
                    symbols.removeDuplicates();
                    item.side = match.captured(1);
                    item.symbols = std::move(symbols);
                }
            }
        }
        if (acknowledgements.size() >= 100) return false;
        if (!item.side.isEmpty()) {
            const auto claim = claimTrade(id);
            if (claim == Claim::Error) return false;
            if (claim == Claim::Duplicate) return true;
        }
        admitted.insert(id);
        admissionOrder.enqueue(id);
        if (admissionOrder.size() > 1024) admitted.remove(admissionOrder.dequeue());
        acknowledgements.enqueue(std::move(item));
        return true;
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
                if (id < offset) {
                    if (!logIncoming(it.value())) return;
                    continue;
                }
                offset = id + 1;
                const QPointer<TelegramAgent> owner(q);
                const auto run = generation;
                const bool accepted = admit(it.value());
                if (!owner || generation != run || stopping || !active) return;
                if (!accepted) {
                    fail(QStringLiteral("Telegram trade admission unavailable; command not dispatched."));
                    return;
                }
            }
            sendNext();
        });
    }
    void sendNext()
    {
        if (!active || dispatchBusy) return;
        while (!acknowledgements.isEmpty() && !allowedChatIds.contains(acknowledgements.head().chatId))
            acknowledgements.dequeue();
        if (acknowledgements.isEmpty()) { if (!options.enabled) pollTimer.start(250); return; }
        dispatchBusy = true;
        const auto acknowledgement = acknowledgements.dequeue();
        const auto chat = acknowledgement.chatId;
        const QPair<qint64, qint64> key{botId, acknowledgement.updateId};
        attempted.insert(key); // Mark before dispatch: a lost receipt is not safe to retry.
        attemptOrder.enqueue(key);
        if (attemptOrder.size() > 1024) attempted.remove(attemptOrder.dequeue());
        if (!acknowledgement.side.isEmpty() && allowedChatIds.contains(chat)) {
            tradeChat = chat;
            tradeTicket = QStringLiteral("%1:%2:%3:%4:%5").arg(botId).arg(acknowledgement.updateId)
                .arg(chat).arg(generation).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            // Busy + immutable notification context and durable claim precede the
            // direct signal. A synchronous receiver may complete, stop or delete us.
            emit q->tradeCommandReceived(tradeTicket, acknowledgement.side, acknowledgement.symbols);
            return; // Never touch State after emitting a domain command.
        }
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
            sendText(chat, result ? QStringLiteral("🟢 Active") : QStringLiteral("Inactive"));
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
    void sendText(qint64 chat, const QString &text, bool html = false)
    {
        QJsonObject body{{"chat_id", chat}, {"text", text}};
        if (html) body.insert("parse_mode", QStringLiteral("HTML"));
        request(QStringLiteral("sendMessage"), body,
            [this, chat, text, html](QJsonValue value) {
                const auto message = value.toObject();
                const auto receiptChat = message.value("chat").toObject();
                if (!message.value("message_id").isDouble() || message.value("message_id").toInteger(-1) <= 0
                    || message.value("date").toInteger() <= 0
                    || receiptChat.value("id").toInteger(-1) != chat
                    || receiptChat.value("type").toString() != QStringLiteral("private")
                    || (message.contains("text") && (!message.value("text").isString()
                        || (!html && message.value("text").toString() != text)))) {
                    fail(QStringLiteral("Telegram acknowledgement delivery is uncertain; invalid receipt. Not retried."));
                    return;
                }
                if (html && !tradeReports.isEmpty()) { sendTradeReport(); return; }
                if (html) tradeChat = 0;
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
        sessionSecret.clear();
        if (manager) manager.release()->deleteLater();
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
        inspectorChoicePending = false;
        ++generation;
        const bool changed = active || starting || healthy;
        active = starting = healthy = false;
        dispatchBusy = false;
        pollTimer.stop();
        acknowledgements.clear();
        tradeTicket.clear(); tradeChat = 0; tradeReports.clear();
        admitted.clear(); admissionOrder.clear();
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
        const bool get = options.enabled && (method == QStringLiteral("getWebhookInfo")
            || method == QStringLiteral("deleteWebhook"));
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
        auto *reply = method == QStringLiteral("setWebhook") && manager ? manager->registerWebhook()
            : get ? network.get(request) : network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        if (!reply) { fail(QStringLiteral("Invalid Telegram webhook configuration.")); return; }
        reply->setReadBufferSize(64 * 1024);
        pending.insert(reply);
        if (method == QStringLiteral("setWebhook")) registrationReply = reply;
        const auto run = generation;
        struct Response { QByteArray bytes; bool oversized = false; bool timedOut = false; };
        const auto response = std::make_shared<Response>();
        const auto consume = [reply, response] {
            constexpr qsizetype limit = 1024 * 1024;
            if (!reply->isOpen() || response->oversized) return;
            const auto managerReceipt = reply->property("webhookResponseBody");
            if (managerReceipt.isValid()) response->bytes = managerReceipt.toByteArray();
            else response->bytes += reply->read(limit + 1 - response->bytes.size());
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
void TelegramAgent::resolveInspectorConflict(const QString &action)
{
    if ((action != QStringLiteral("restart") && action != QStringLiteral("cancel"))
        || !d->inspectorChoicePending || !d->starting || d->stopping || !d->host || d->clientId.isEmpty()) return;
    d->inspectorChoicePending = false;
    const auto run = d->generation;
    const auto id = d->clientId;
    const QPointer<TelegramAgent> owner(this);
    const auto choice = action == QStringLiteral("restart") ? TelegramWebhookHost::InspectorConflictAction::Restart
        : TelegramWebhookHost::InspectorConflictAction::Cancel;
    if (choice == TelegramWebhookHost::InspectorConflictAction::Cancel) d->allowedChatIds.clear();
    QTimer::singleShot(0, this, [owner, run, id, choice] {
        if (!owner || owner->d->generation != run || owner->d->clientId != id
            || !owner->d->starting || owner->d->stopping || !owner->d->host) return;
        owner->d->host->resolveInspectorConflict(id, choice);
        if (choice == TelegramWebhookHost::InspectorConflictAction::Cancel && owner && owner->d->generation == run)
            owner->d->stop();
    });
}
void TelegramAgent::setAllowedChatIds(QStringList ids)
{
    QSet<qint64> policy;
    static const QRegularExpression format(QStringLiteral("\\A[1-9][0-9]{0,15}\\z"));
    for (const auto &id : ids) {
        bool ok = false;
        const auto value = id.toLongLong(&ok);
        if (!format.match(id).hasMatch() || !ok || value > maxIdentity) { policy.clear(); break; }
        policy.insert(value);
    }
    if (d->active || d->starting || d->stopping) policy.intersect(d->allowedChatIds);
    d->allowedChatIds = std::move(policy);
    if (d->manager) {
        QStringList effective;
        for (const auto id : d->allowedChatIds) effective.append(QString::number(id));
        d->manager->setAllowedChatIds(effective);
    }
    if (d->tradeChat && !d->allowedChatIds.contains(d->tradeChat)) d->tradeReports.clear();
}
void TelegramAgent::setCommandLedgerPathForTests(QString path)
{
    if (d->active || d->starting || d->stopping || !QHostAddress(d->root.host()).isLoopback()
        || d->root.scheme() != QStringLiteral("http")) return;
    d->ledgerPath = std::move(path);
    d->ledgerPathInjected = true;
}
void TelegramAgent::completeTradeCommand(QString requestId, QStringList htmlMessages)
{
    if (!d->active || requestId.isEmpty() || requestId != d->tradeTicket) return;
    d->tradeTicket.clear(); // Consume before any synchronous callbacks.
    if (d->allowedChatIds.contains(d->tradeChat)) {
        for (const auto &message : htmlMessages)
            for (const auto &chunk : State::chunks(message)) d->tradeReports.enqueue(chunk);
    }
    if (!d->tradeReports.isEmpty()) { d->sendTradeReport(); return; }
    d->tradeChat = 0;
    d->dispatchBusy = false;
    QTimer::singleShot(0, this, [this] { d->sendNext(); });
}
void TelegramAgent::start(QString token)
{
    // Legacy polling fixture seam only. Production and all webhook starts require
    // the explicit password-decrypted per-session secret, never a generated one.
    if (d->options.enabled || d->root.scheme() != QStringLiteral("http")
        || !QHostAddress(d->root.host()).isLoopback()) {
        emit errorOccurred(QStringLiteral("Session authorization is required.")); return;
    }
    start(std::move(token), QStringLiteral("injected_polling_fixture"));
}
void TelegramAgent::start(QString token, QString sessionSecret)
{
    if (d->stopping || (d->options.enabled && (d->active || d->starting))) {
        emit errorOccurred(QStringLiteral("Telegram transport is busy; wait for stop completion."));
        return;
    }
    const bool hadState = d->active || d->starting;
    d->stop(false);
    static const QRegularExpression format(QStringLiteral("\\A[1-9][0-9]{0,15}:[A-Za-z0-9_-]{1,256}\\z"));
    if (!d->safeRoot() || !d->safeHealthEndpoint() || !format.match(token).hasMatch()
        || !TelegramWebhookManager::validSecret(sessionSecret)) {
        const auto run = d->generation;
        const QPointer<TelegramAgent> guard(this);
        if (hadState) emit stateChanged();
        if (guard && d->generation == run)
            emit errorOccurred(QStringLiteral("Invalid Telegram endpoint or bot token format."));
        return;
    }
    d->token = std::move(token);
    d->sessionSecret = std::move(sessionSecret);
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
