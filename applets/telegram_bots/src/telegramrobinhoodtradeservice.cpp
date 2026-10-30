#include "telegramrobinhoodtradeservice.h"
#include "rhsessionmanager.h"
#include "rhtradingcontext.h"
#include <QRegularExpression>
#include <QTimer>
#include <utility>

// Keep Q_INIT_RESOURCE outside a namespace: the generated symbol is global.
static void initializeRobinhoodResources() { Q_INIT_RESOURCE(rh_config); }
namespace {
const QString reauth = QStringLiteral("Robinhood Re-authentication required before trades can be completed.");
using Session = RHTradeController::Session;
using Dependencies = RHTradeController::Dependencies;
struct Lease {
    std::function<void()> release;
    bool started = false, released = false;
    void close() { if (started && !std::exchange(released, true) && release) release(); }
    ~Lease() { close(); }
};
QString safeQuantity(const QVariant &value) {
    const auto text = value.toString();
    static const QRegularExpression decimal(QStringLiteral("\\A[0-9]{1,30}(?:\\.[0-9]{1,10})?\\z"));
    return decimal.match(text).hasMatch() ? text : QString();
}
}
struct TelegramRobinhoodTradeService::State : QObject {
    TelegramRobinhoodTradeService *q;
    QPointer<RHNetworkEngine> network;
    QPointer<RHSessionManager> sessions;
    Dependencies deps;
    bool ownGraph = true, busy = false, waiting = false, revoked = false;
    quint64 generation = 0;
    int slot = -1, offset = 0, chunkSize = 0;
    QString profile, nickname, id, side;
    QStringList symbols, chunk;
    Session snapshot;
    QVariantList results;
    std::shared_ptr<Lease> lease;
    QPointer<RHTradeController> controller;
    QMetaObject::Connection activationConnection;
    std::unique_ptr<RHNetworkEngine> ownedNetwork;
    std::unique_ptr<RHSessionManager> ownedSessions;
    QTimer dependencyWatch;
    explicit State(TelegramRobinhoodTradeService *owner) : q(owner) {
        dependencyWatch.setInterval(100);
        connect(&dependencyWatch, &QTimer::timeout, this, [this] {
            if (!busy || (network && sessions)) return;
            revoked = true;
            if (snapshot.accounts.isEmpty()) { settle({reauth}); return; }
            const QPointer<State> guard(this);
            if (controller) controller->cancel();
            if (guard && busy) stopRemaining();
        });
    }
    ~State() override {
        if (controller) delete controller.data();
        if (lease) lease->close();
        snapshot.token.fill('\0');
    }
    bool pinned() const {
        if (!network || !sessions || sessions->rowCount() == 0 || sessions->reauthenticating()
            || sessions->browserLoginPending() || sessions->nativeLoginPending()) return false;
        const auto first = sessions->index(0);
        return sessions->data(first, RHSessionManager::SessionIndexRole).toInt() == slot
            && sessions->data(first, RHSessionManager::AccountIdRole).toString() == profile
            && !sessions->data(first, RHSessionManager::IsExpiredRole).toBool();
    }
    bool current(const Session &s, bool local = false) {
        const QPointer<State> guard(this);
        const auto check = local ? deps.currentLocal : deps.current;
        const bool ok = pinned() && s.sessionIndex == slot && s.profileId == profile && check && check(s);
        if (guard && !ok) revoked = true;
        return guard && ok;
    }
    void begin() {
        const QPointer<State> guard(this);
        if (ownGraph && !network) {
            initializeRobinhoodResources();
            ownedNetwork = std::make_unique<RHNetworkEngine>(); network = ownedNetwork.get();
            if (!network->loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"))) { settle({reauth}); return; }
            ownedSessions = std::make_unique<RHSessionManager>(*network); sessions = ownedSessions.get();
            deps = makeRHTradingDependencies(*sessions);
        }
        if (!network || !sessions || sessions->busy() || sessions->reauthenticating()
            || sessions->browserLoginPending() || sessions->nativeLoginPending()) { settle({reauth}); return; }
        dependencyWatch.start(); // Pointer liveness only; never poll disk/ownership.
        if (ownGraph) {
            if (!sessions->load()) { if (guard) settle({reauth}); return; }
            if (!guard) return;
        }
        if (sessions->rowCount() == 0) { settle({reauth}); return; }
        const auto first = sessions->index(0);
        slot = sessions->data(first, RHSessionManager::SessionIndexRole).toInt();
        profile = sessions->data(first, RHSessionManager::AccountIdRole).toString();
        nickname = sessions->data(first, RHSessionManager::AccountNameRole).toString();
        if (slot < 0 || profile.isEmpty() || !pinned()) { settle({reauth}); return; }
        waiting = true;
        const auto epoch = generation;
        // startSession is the native saved-session verifier, not interactive login.
        activationConnection = connect(sessions, &RHSessionManager::busyChanged, this, [guard, epoch] {
            if (guard && guard->busy && guard->generation == epoch && guard->waiting
                && guard->sessions && !guard->sessions->busy())
                QTimer::singleShot(0, guard, [guard, epoch] { if (guard && guard->generation == epoch) guard->activated(); });
        });
        const bool started = sessions->startSession(slot);
        if (!guard) return;
        if (!started) { waiting = false; settle({reauth}); return; }
        if (!sessions->busy()) QTimer::singleShot(0, this, [guard, epoch] { if (guard && guard->generation == epoch) guard->activated(); });
    }
    void activated() {
        if (!busy || !waiting || !sessions || sessions->busy()) return;
        waiting = false;
        disconnect(activationConnection); activationConnection = {};
        if (!pinned() || !sessions->sessionActive() || !sessions->tradingUnavailableReason().isEmpty()
            || !deps.acquire || !deps.current || !deps.currentLocal || !deps.currentAsync || !deps.release) { settle({reauth}); return; }
        lease = std::make_shared<Lease>(); lease->release = deps.release; lease->started = true;
        const auto held = lease;
        const auto delivered = std::make_shared<bool>(false);
        const QPointer<State> guard(this); const auto epoch = generation;
        auto acquire = deps.acquire;
        acquire([guard, held, delivered, epoch](bool ok, Session s, QString) mutable {
            if (std::exchange(*delivered, true)) { s.token.fill('\0'); return; }
            if (!guard || !guard->busy || guard->generation != epoch) { s.token.fill('\0'); held->close(); return; }
            QTimer::singleShot(0, guard, [guard, held, epoch, ok, s = std::move(s)]() mutable {
                if (!guard || !guard->busy || guard->generation != epoch) { s.token.fill('\0'); held->close(); return; }
                if (!ok || !guard->current(s)) { s.token.fill('\0'); if (guard) guard->settle({reauth}); return; }
                guard->snapshot = std::move(s);
                const int count = guard->snapshot.accounts.size();
                if (count == 0) { guard->settle({reauth}); return; }
                if (count > 20) { guard->settle({"Robinhood account scope exceeds the supported capacity; no orders were sent."}); return; }
                guard->chunkSize = qMin(20, 100 / count);
                guard->next();
            });
        });
    }
    void next() {
        if (!busy) return;
        const QPointer<State> guard(this);
        if (offset >= symbols.size()) { settle(report()); return; }
        if (!network || !current(snapshot)) { if (guard) stopRemaining(); return; }
        chunk = symbols.mid(offset, chunkSize);
        Dependencies wrapped = deps;
        wrapped.availabilityReason = [] { return QString(); }; // The overarching lease owns admission.
        wrapped.acquire = [guard](auto done) {
            if (!guard || !guard->current(guard->snapshot)) { done(false, {}, {}); return; }
            done(true, guard->snapshot, {});
        };
        wrapped.release = [] {}; // Release the actual manager lease only after the final chunk/cooldown.
        wrapped.current = [guard](const auto &s) { return guard && guard->current(s); };
        wrapped.currentLocal = [guard](const auto &s) { return guard && guard->current(s, true); };
        wrapped.currentAsync = [guard](const Session &s, auto done) {
            if (!guard || !guard->pinned() || !guard->deps.currentAsync) { done(false); return; }
            const auto check = guard->deps.currentAsync;
            check(s, [guard, done = std::move(done)](bool ok) mutable {
                const bool valid = guard && ok && guard->pinned();
                if (guard && !valid) guard->revoked = true;
                done(valid);
            });
        };
        controller = new RHTradeController(*network, std::move(wrapped), this);
        const auto epoch = generation;
        connect(controller, &RHTradeController::tradeResultsCompleteChanged, this, [guard, epoch] {
            if (guard && guard->generation == epoch && guard->controller && guard->controller->tradeResultsComplete())
                QTimer::singleShot(0, guard, [guard, epoch] { if (guard && guard->generation == epoch) guard->chunkDone(); });
        });
        if (!controller->submit(chunk.join(','), side)) stopRemaining();
    }
    void appendRows(const QVariantList &native, const QStringList &requested) {
        // Include every frozen account/ticker even when native preflight stops early.
        for (const auto &symbol : requested) for (const auto &account : snapshot.accounts) {
            const auto number = account.toMap().value("accountNumber").toString();
            QVariantMap row;
            for (const auto &item : native) {
                const auto value = item.toMap();
                if (value.value("symbol") == symbol && value.value("accountNumber") == number) { row = value; break; }
            }
            if (row.isEmpty()) row = {{"symbol", symbol}, {"accountNumber", number}, {"status", "not_sent"}};
            results.append(row);
        }
    }
    void chunkDone() {
        if (!busy || !controller || !controller->tradeResultsComplete()) return;
        const auto rows = controller->tradeResults();
        if (controller->authenticationRequired()) revoked = true;
        bool halt = revoked || !controller->error().isEmpty();
        for (const auto &raw : rows) {
            const auto row = raw.toMap();
            if (row.value("status") == "unknown") halt = true;
        }
        appendRows(rows, chunk); offset += chunk.size(); chunk.clear();
        controller->deleteLater(); controller.clear();
        const QPointer<State> guard(this);
        if (!current(snapshot)) { if (guard) stopRemaining(); return; }
        if (halt) stopRemaining(); else next();
    }
    void stopRemaining() {
        if (controller) {
            appendRows(controller->tradeResults(), chunk);
            offset += chunk.size(); chunk.clear();
            controller->deleteLater(); controller.clear();
        }
        appendRows({}, symbols.mid(offset)); offset = symbols.size();
        settle(report());
    }
    QStringList report() const {
        if (revoked) {
            bool attempted = false;
            for (const auto &raw : results) attempted |= raw.toMap().value("orderPostAttempted").toBool();
            if (!attempted) return {reauth};
        }
        const auto title = QStringLiteral("<b>Robinhood Auto %1 Results</b>").arg(side == "buy" ? "Buy" : "Sell");
        QStringList messages;
        QString message = title;
        const auto add = [&](const QString &line) {
            // Bound encoded text too: every resulting message has intact HTML/entities.
            const auto escaped = line.toHtmlEscaped();
            if (message.size() + escaped.size() + 1 > 3900) { messages.append(message); message = title; }
            message += '\n' + escaped;
        };
        if (revoked) add(reauth);
        for (const auto &account : snapshot.accounts) {
            const auto number = account.toMap().value("accountNumber").toString();
            for (int category = 0; category < 3; ++category) {
                QStringList entries;
                for (const auto &raw : results) {
                    const auto row = raw.toMap(); if (row.value("accountNumber") != number) continue;
                    const auto status = row.value("status").toString();
                    const bool attempted = row.value("orderPostAttempted").toBool();
                    const bool sent = attempted && row.value("orderPostTransportOk").toBool() && status != "failed";
                    const int kind = status == "preview" ? 2 : sent ? 0 : 1;
                    if (kind != category) continue;
                    QString detail;
                    if (kind == 2) detail = "Debug preview; not sent";
                    else if (status == "unknown") detail = sent ? "Sent; confirmation unavailable; check broker; no automatic retry" : "Submission outcome unknown; check broker; no automatic retry";
                    else if (sent) detail = status == "filled" ? "Sent; broker confirmed filled" : "Sent; not fill-confirmed";
                    else if (status == "skipped") detail = "Skipped; no sellable position/order not sent";
                    else if (attempted) detail = "Order send/rejection error; check broker before retrying";
                    else detail = "Not sent";
                    if (sent && row.value("orderState").isValid()) {
                        const auto state = row.value("orderState").toString();
                        if (QStringList{"queued", "unconfirmed", "confirmed", "partially_filled", "filled", "rejected", "cancelled", "failed"}.contains(state)) detail += "; status " + state;
                    }
                    const auto quantity = safeQuantity(row.value("quantity"));
                    entries.append(row.value("symbol").toString() + (quantity.isEmpty() ? QString() : " (" + quantity + ")") + ": " + detail);
                }
                if (entries.isEmpty()) continue;
                const auto heading = category == 0 ? QStringLiteral("🟢 Sent Trades") : category == 1 ? QStringLiteral("🔴 Failed/Not Sent Trades") : QStringLiteral("🟡 Preview Trades (not sent)");
                // Bound display fields independently; never include native diagnostic/provider messages.
                const auto prefix = heading + " — " + nickname.left(160) + " / Account " + number.left(128) + ": ";
                QString line = prefix;
                for (const auto &entry : entries) {
                    if ((line + "; " + entry).toHtmlEscaped().size() > 3000 && line != prefix) { add(line); line = prefix; }
                    if (line != prefix) line += "; ";
                    line += entry;
                }
                add(line);
            }
        }
        if (message != title || messages.isEmpty()) messages.append(message);
        return messages;
    }
    void settle(QStringList messages) {
        if (!busy) return;
        // Retire all old state before release/busy/completed direct observers run.
        const auto request = id;
        dependencyWatch.stop();
        disconnect(activationConnection); activationConnection = {};
        auto held = std::exchange(lease, {});
        if (controller) { delete controller.data(); controller.clear(); }
        snapshot.token.fill('\0'); snapshot = {}; results.clear(); symbols.clear(); chunk.clear();
        waiting = false; offset = 0; slot = -1; profile.clear(); nickname.clear(); id.clear(); side.clear();
        ++generation;
        const QPointer<State> guard(this);
        if (held) held->close();
        if (!guard) return;
        busy = false;
        const QPointer<TelegramRobinhoodTradeService> owner(q);
        emit q->busyChanged();
        if (owner) emit owner->completed(request, messages);
    }
};
TelegramRobinhoodTradeService::TelegramRobinhoodTradeService(QObject *parent)
    : QObject(parent), d(std::make_unique<State>(this)) {}
TelegramRobinhoodTradeService::TelegramRobinhoodTradeService(RHNetworkEngine *network, RHSessionManager *sessions,
    Dependencies dependencies, QObject *parent) : TelegramRobinhoodTradeService(parent) {
    d->ownGraph = false; d->network = network; d->sessions = sessions; d->deps = std::move(dependencies);
}
TelegramRobinhoodTradeService::~TelegramRobinhoodTradeService() = default;
bool TelegramRobinhoodTradeService::busy() const { return d->busy; }
bool TelegramRobinhoodTradeService::submit(QString requestId, QStringList symbols, QString side) {
    side = side.trimmed().toLower();
    if (d->busy || requestId.isEmpty() || (side != "buy" && side != "sell") || symbols.isEmpty()) return false;
    static const QRegularExpression ticker(QStringLiteral("\\A[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?\\z"));
    QStringList normalized;
    for (const auto &symbol : symbols) {
        if (!ticker.match(symbol).hasMatch()) return false;
        const auto upper = symbol.toUpper(); if (!normalized.contains(upper)) normalized.append(upper);
    }
    if (normalized.join(',').size() > 4096) return false;
    d->busy = true; d->revoked = false; d->id = std::move(requestId); d->symbols = std::move(normalized); d->side = std::move(side);
    const auto epoch = ++d->generation; const QPointer<State> guard(d.get());
    emit busyChanged();
    if (guard) QTimer::singleShot(0, guard, [guard, epoch] { if (guard && guard->generation == epoch && guard->busy) guard->begin(); });
    return true;
}
