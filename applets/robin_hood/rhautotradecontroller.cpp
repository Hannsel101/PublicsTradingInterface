#include "rhautotradecontroller.h"
#include "rhsessionmanager.h"
#include "rhtradingcontext.h"
#include <QRegularExpression>
#include <QTimer>
#include <utility>

namespace {
using Session = RHTradeController::Session;
using Dependencies = RHTradeController::Dependencies;
const QString sessionFailure = QStringLiteral("Saved Robinhood session is unavailable, expired or rejected; no automatic sign-in was attempted.");
const QString stopped = QStringLiteral("Not sent; batch stopped. Check the broker before retrying any uncertain order.");
struct Lease {
    std::function<void()> release;
    bool closed = false;
    void close() { if (!std::exchange(closed, true) && release) release(); }
    ~Lease() { close(); }
};
}

struct RHAutoTradeController::State : QObject {
    struct Profile { int slot = -1; QString id, name, expires; };
    RHAutoTradeController *q;
    QPointer<RHNetworkEngine> network;
    QPointer<RHSessionManager> sessions;
    Dependencies deps;
    QList<Profile> profiles;
    Session snapshot;
    QStringList symbols, chunk;
    QVariantList results;
    QString side, error;
    QPointer<RHTradeController> child;
    std::shared_ptr<Lease> lease;
    QTimer watch;
    QMetaObject::Connection activation;
    quint64 generation = 0;
    int profile = -1, account = 0, offset = 0;
    bool busy = false, complete = false, externalBusy = false, waiting = false, retiring = false;

    explicit State(RHAutoTradeController *owner) : q(owner) {
        watch.setInterval(100);
        connect(&watch, &QTimer::timeout, this, [this] {
            if (busy && (!network || !sessions)) stop(sessionFailure);
        });
    }
    ~State() override {
        retiring = true;
        disconnect(activation);
        if (child) delete child.data();
        if (waiting && sessions) sessions->cancelSignIn();
        if (lease) lease->close();
        snapshot.token.fill('\0');
    }
    bool dependenciesReady() const {
        return network && sessions && deps.acquire && deps.current && deps.currentLocal
            && deps.currentAsync && deps.release;
    }
    bool admitted() const {
        if (!dependenciesReady() || busy || retiring || externalBusy || sessions->busy()
            || sessions->reauthenticating() || sessions->browserLoginPending() || sessions->nativeLoginPending()
            || !network->isConfigured()) return false;
        for (int i = 0; i < sessions->rowCount(); ++i)
            if (!sessions->data(sessions->index(i), RHSessionManager::IsExpiredRole).toBool()) return true;
        return false;
    }
    Profile readProfile(int row) const {
        const auto index = sessions->index(row);
        Profile p;
        p.slot = sessions->data(index, RHSessionManager::SessionIndexRole).toInt();
        p.name = sessions->data(index, RHSessionManager::AccountNameRole).toString();
        p.id = sessions->data(index, RHSessionManager::AccountIdRole).toString();
        if (p.id.isEmpty()) p.id = p.name;
        p.expires = sessions->data(index, RHSessionManager::ExpiresAtRole).toString();
        return p;
    }
    bool interactiveAuthentication() const {
        return !sessions || sessions->reauthenticating() || sessions->browserLoginPending() || sessions->nativeLoginPending();
    }
    void saved(int slot) {
        // startSession's non-interactive OAuth verifier can rotate credentials
        // before any lease/order exists. Admit only its same-owner publication;
        // the later native lease binds the new durable credential revision.
        if (busy && waiting && !lease && sessions && sessions->busy() && !interactiveAuthentication()
            && profile >= 0 && profile < profiles.size() && profiles[profile].slot == slot) {
            for (int row = 0; row < sessions->rowCount(); ++row) {
                const auto renewed = readProfile(row);
                if (renewed.slot != slot) continue;
                if (renewed.id == profiles[profile].id) { profiles[profile].expires = renewed.expires; return; }
                break;
            }
        }
        if (busy) stop(sessionFailure); else notify();
    }
    bool pinned() const {
        if (!network || !sessions || profile < 0 || profile >= profiles.size()
            || sessions->reauthenticating() || sessions->browserLoginPending() || sessions->nativeLoginPending()) return false;
        const auto &p = profiles[profile];
        for (int row = 0; row < sessions->rowCount(); ++row) {
            const auto actual = readProfile(row);
            if (actual.slot == p.slot) return actual.id == p.id && actual.expires == p.expires
                && !sessions->data(sessions->index(row), RHSessionManager::IsExpiredRole).toBool();
        }
        return false;
    }
    bool current(const Session &s, bool local = false) const {
        if (!busy || !pinned() || s.sessionIndex != profiles[profile].slot || s.profileId != profiles[profile].id) return false;
        const auto callback = local ? deps.currentLocal : deps.current;
        const QPointer<State> guard(const_cast<State *>(this)); const auto epoch = generation;
        const bool ok = callback && callback(s);
        return guard && guard->generation == epoch && guard->busy && ok;
    }
    bool notify() {
        const QPointer<State> guard(this); const auto epoch = generation;
        emit q->stateChanged();
        return guard && guard->generation == epoch;
    }
    template<class F> void later(F callback) {
        const QPointer<State> guard(this); const auto epoch = generation;
        QTimer::singleShot(0, this, [guard, epoch, callback = std::move(callback)]() mutable {
            if (guard && guard->busy && guard->generation == epoch) callback(*guard);
        });
    }
    QVariantMap decorate(QVariantMap row, const Profile &p) const {
        const auto native = row;
        const auto status = native.value("status").toString();
        const auto orderState = native.value("orderState").toString();
        const bool preview = status == "preview";
        const bool rejected = status == "failed" || status == "rejected" || orderState == "rejected"
            || orderState == "failed" || orderState == "cancelled";
        const bool attempted = native.value("orderPostAttempted").toBool();
        const bool sent = attempted && native.value("orderPostTransportOk").toBool() && !rejected;
        QString message, presentation;
        if (preview) { presentation = "preview"; message = "Debug preview; not sent."; }
        else if (sent) {
            presentation = "sent";
            message = status == "unknown" ? "Sent; confirmation unavailable. Check broker; no automatic retry."
                : orderState == "filled" && native.value("success").toBool() ? "Sent; broker confirmed filled."
                : "Sent; not fill-confirmed.";
        } else if (status == "unknown") {
            presentation = "warning"; message = "Submission outcome unknown. Check broker; no automatic retry.";
        } else if (status == "pending") { presentation = "pending"; message = "Waiting."; }
        else if (status == "skipped") { presentation = "not_sent"; message = "Skipped; no eligible account or sellable position. Not sent."; }
        else { presentation = "failed"; message = attempted ? "Order send/rejection error; check broker before retrying." : "Not sent."; }
        row.insert("nativeRow", native);
        row.insert("sessionIndex", p.slot); row.insert("profileId", p.id); row.insert("sessionName", p.name.left(160));
        row.insert("success", sent); row.insert("preview", preview);
        row.insert("status", presentation); row.insert("message", message);
        return row;
    }
    QVariantList visibleResults() const {
        auto all = results;
        if (child && profile >= 0 && profile < profiles.size())
            for (const auto &raw : child->tradeResults()) all.append(decorate(raw.toMap(), profiles[profile]));
        return all;
    }
    void append(const QVariantList &native, const QStringList &requested, const QString &number) {
        for (const auto &symbol : requested) {
            QVariantMap row;
            for (const auto &raw : native) {
                const auto candidate = raw.toMap();
                if (candidate.value("symbol") == symbol && candidate.value("accountNumber") == number) { row = candidate; break; }
            }
            if (row.isEmpty()) row = {{"symbol", symbol}, {"accountNumber", number}, {"side", side},
                {"status", "not_sent"}, {"success", false}, {"message", stopped},
                {"orderPostAttempted", false}, {"orderPostTransportOk", false}};
            results.append(decorate(row, profiles[profile]));
        }
    }
    QString number() const { return snapshot.accounts.value(account).toMap().value("accountNumber").toString(); }
    void release() {
        const auto held = std::exchange(lease, {});
        snapshot.token.fill('\0'); snapshot = {};
        if (held) held->close();
    }
    void nextProfile() {
        const QPointer<State> guard(this); const auto epoch = generation;
        // Retire the previous profile's cursor before release emits direct idle
        // observers; cancellation there must not append its completed rows twice.
        account = 0; offset = 0; ++profile;
        release();
        if (!guard || generation != epoch || !busy) return;
        if (profile >= profiles.size()) { finish(); return; }
        if (!pinned() || sessions->busy()) { failProfile(); return; }
        waiting = true;
        activation = connect(sessions, &RHSessionManager::busyChanged, this, [this] {
            if (busy && waiting && sessions && !sessions->busy()) later([](State &s) { s.activated(); });
        });
        const bool started = sessions->startSession(profiles[profile].slot);
        if (!guard || generation != epoch || !busy) return;
        if (!started) { waiting = false; failProfile(); return; }
        if (!sessions->busy()) later([](State &s) { s.activated(); });
    }
    void failProfile() {
        disconnect(activation); activation = {}; waiting = false;
        error = sessionFailure;
        append({}, symbols, {});
        if (!notify()) return;
        later([](State &s) { s.nextProfile(); });
    }
    void activated() {
        if (!busy || !waiting || !sessions || sessions->busy()) return;
        waiting = false; disconnect(activation); activation = {};
        if (!pinned() || !sessions->sessionActive() || !sessions->tradingUnavailableReason().isEmpty()) { failProfile(); return; }
        lease = std::make_shared<Lease>(); lease->release = deps.release;
        const auto held = lease; const auto delivered = std::make_shared<bool>(false);
        const QPointer<State> guard(this); const auto epoch = generation;
        const auto acquire = deps.acquire;
        acquire([guard, epoch, held, delivered](bool ok, Session s, QString) mutable {
            if (std::exchange(*delivered, true)) { s.token.fill('\0'); return; }
            if (!guard || !guard->busy || guard->generation != epoch) { s.token.fill('\0'); held->close(); return; }
            guard->later([ok, s = std::move(s), held](State &state) mutable {
                const QPointer<State> live(&state);
                if (!ok || !state.current(s)) { s.token.fill('\0'); if (live) state.failProfile(); return; }
                state.snapshot = std::move(s);
                if (state.snapshot.accounts.isEmpty() || state.snapshot.accounts.size() > 20) { state.failProfile(); return; }
                state.nextChunk();
            });
        });
    }
    void retireSession() {
        error = sessionFailure;
        for (int a = account; a < snapshot.accounts.size(); ++a)
            append({}, symbols.mid(a == account ? offset : 0), snapshot.accounts[a].toMap().value("accountNumber").toString());
        account = snapshot.accounts.size(); offset = 0;
        if (notify()) later([](State &s) { s.nextProfile(); });
    }
    void nextChunk() {
        if (!busy) return;
        if (account >= snapshot.accounts.size()) { nextProfile(); return; }
        if (offset >= symbols.size()) { ++account; offset = 0; nextChunk(); return; }
        if (snapshot.expiresAt <= QDateTime::currentDateTimeUtc()) { retireSession(); return; }
        const QPointer<State> guard(this); const auto epoch = generation;
        if (!current(snapshot)) { if (guard) stop(sessionFailure); return; }
        chunk = symbols.mid(offset, 20);
        Dependencies wrapped = deps;
        wrapped.autoModeAccountNumber = number();
        wrapped.availabilityReason = [] { return QString(); };
        wrapped.acquire = [guard, epoch](auto done) {
            if (!guard || guard->generation != epoch || !guard->current(guard->snapshot)) { done(false, {}, {}); return; }
            done(true, guard->snapshot, {});
        };
        wrapped.release = [] {}; // The one true native lease spans all accounts/chunks of this login.
        wrapped.current = [guard, epoch](const auto &s) { return guard && guard->generation == epoch && guard->current(s); };
        wrapped.currentLocal = [guard, epoch](const auto &s) { return guard && guard->generation == epoch && guard->current(s, true); };
        wrapped.currentAsync = [guard, epoch](const Session &s, auto done) {
            if (!guard || guard->generation != epoch || !guard->pinned() || !guard->deps.currentAsync) { done(false); return; }
            const auto callback = guard->deps.currentAsync;
            callback(s, [guard, epoch, done = std::move(done)](bool ok) mutable {
                done(guard && guard->generation == epoch && guard->busy && ok && guard->pinned());
            });
        };
        child = new RHTradeController(*network, std::move(wrapped), this);
        connect(child, &RHTradeController::tradeResultsChanged, this, [this] { notify(); });
        connect(child, &RHTradeController::currentTransactionTitleChanged, this, [this] { notify(); });
        connect(child, &RHTradeController::tradeResultsCompleteChanged, this, [this] {
            if (child && child->tradeResultsComplete()) later([](State &s) { s.chunkDone(); });
        });
        auto *native = child.data();
        const bool started = native->submit(chunk.join(','), side);
        if (guard && generation == epoch && busy && !started) stop(stopped);
    }
    void chunkDone() {
        if (!busy || !child || !child->tradeResultsComplete()) return;
        const auto rows = child->tradeResults();
        // Native has already validated the entire fresh scope and pinned account
        // identity. Its empty, definite-no-order eligibility outcome is not an
        // uncertain failure and must not block later supported account products.
        const bool excluded = rows.isEmpty() && !child->authenticationRequired()
            && child->error() == QStringLiteral("No active brokerage accounts remain; no orders were sent.");
        const bool invalidSession = child->authenticationRequired()
            || snapshot.expiresAt <= QDateTime::currentDateTimeUtc();
        bool halt = !excluded && !child->error().isEmpty();
        bool unknown = false;
        for (const auto &raw : rows) if (raw.toMap().value("status") == "unknown") unknown = true;
        if (!excluded) { append(rows, chunk, number()); offset += chunk.size(); }
        else offset = symbols.size();
        chunk.clear();
        child->deleteLater(); child.clear();
        if (unknown) { stop(stopped); return; }
        if (invalidSession) { retireSession(); return; }
        if (halt) { stop(stopped); return; }
        if (!notify()) return;
        later([](State &s) { s.nextChunk(); });
    }
    void stop(QString reason) {
        if (!busy || retiring) return;
        error = std::move(reason);
        const QPointer<State> guard(this); const auto epoch = generation;
        if (child) {
            child->cancel();
            if (!guard || generation != epoch || !busy) return;
            append(child->tradeResults(), chunk, number()); offset += chunk.size(); chunk.clear();
            delete child.data(); child.clear();
        }
        if (!snapshot.accounts.isEmpty()) {
            for (int a = account; a < snapshot.accounts.size(); ++a)
                append({}, symbols.mid(a == account ? offset : 0), snapshot.accounts[a].toMap().value("accountNumber").toString());
        } else if (profile >= 0 && profile < profiles.size()) append({}, symbols, {});
        // Future logins are never opened after uncertainty/cancellation. Record
        // every unattempted ticker explicitly without inventing account scope.
        for (++profile; profile < profiles.size(); ++profile) append({}, symbols, {});
        finish();
    }
    void finish() {
        if (!busy || retiring) return;
        retiring = true; ++generation;
        watch.stop(); disconnect(activation); activation = {};
        const bool cancelActivation = std::exchange(waiting, false);
        if (child) { delete child.data(); child.clear(); }
        chunk.clear(); symbols.clear();
        const QPointer<State> guard(this);
        if (cancelActivation && sessions) sessions->cancelSignIn();
        if (!guard) return;
        release();
        if (!guard) return;
        complete = true; busy = false; retiring = false;
        notify(); // No old-generation writes after the idle notification.
    }
};

RHAutoTradeController::RHAutoTradeController(RHNetworkEngine &network, RHSessionManager &sessions, QObject *parent)
    : RHAutoTradeController(network, sessions, makeRHTradingDependencies(sessions), parent) {}
RHAutoTradeController::RHAutoTradeController(RHNetworkEngine &network, RHSessionManager &sessions,
    Dependencies deps, QObject *parent) : QObject(parent), d(std::make_unique<State>(this)) {
    d->network = &network; d->sessions = &sessions; d->deps = std::move(deps);
    connect(&sessions, &RHSessionManager::busyChanged, d.get(), [s = d.get()] { if (!s->busy) s->notify(); });
    const auto changed = [s = d.get()] { if (s->busy) s->stop(sessionFailure); else s->notify(); };
    connect(&sessions, &QAbstractItemModel::modelReset, d.get(), changed);
    connect(&sessions, &QAbstractItemModel::rowsInserted, d.get(), changed);
    connect(&sessions, &QAbstractItemModel::rowsRemoved, d.get(), changed);
    connect(&sessions, &RHSessionManager::sessionSaved, d.get(), [s = d.get()](int slot) { s->saved(slot); });
    const auto authenticationChanged = [s = d.get()] {
        if (s->busy && s->interactiveAuthentication()) s->stop(sessionFailure);
        else if (!s->busy) s->notify();
    };
    connect(&sessions, &RHSessionManager::reauthenticatingChanged, d.get(), authenticationChanged);
    connect(&sessions, &RHSessionManager::nativeLoginChanged, d.get(), authenticationChanged);
    connect(&sessions, &RHSessionManager::browserLoginPendingChanged, d.get(), authenticationChanged);
}
RHAutoTradeController::~RHAutoTradeController() = default;
bool RHAutoTradeController::available() const { return d->admitted(); }
bool RHAutoTradeController::busy() const { return d->busy; }
bool RHAutoTradeController::liveOrdersEnabled() const { return d->network && d->network->stockLiveOrdersEnabled(d->deps.loopbackLiveOrdersForTests); }
QVariantList RHAutoTradeController::tradeResults() const { return d->visibleResults(); }
QString RHAutoTradeController::currentTransactionTitle() const { return d->child ? d->child->currentTransactionTitle() : QString(); }
QString RHAutoTradeController::error() const { return d->error; }
bool RHAutoTradeController::tradeResultsComplete() const { return d->complete; }
bool RHAutoTradeController::submit(QString input, QString side) {
    if (d->busy || d->retiring) return false;
    static const QRegularExpression ticker(QStringLiteral("\\A[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?\\z"));
    QStringList symbols;
    if (input.size() <= 4096 && (side == "buy" || side == "sell")) {
        for (const auto &entry : input.split(',')) {
            const auto candidate = entry.trimmed();
            if (!ticker.match(candidate).hasMatch()) { symbols.clear(); break; }
            const auto upper = candidate.toUpper();
            if (!symbols.contains(upper)) symbols.append(upper);
        }
    }
    if (symbols.isEmpty()) {
        d->error = "Enter valid comma-separated ASCII stock symbols (maximum 4096 characters) and select Buy or Sell.";
        d->notify(); return false;
    }
    if (!d->admitted()) { d->error = sessionFailure; d->notify(); return false; }
    d->profiles.clear();
    for (int row = 0; row < d->sessions->rowCount(); ++row) {
        if (d->sessions->data(d->sessions->index(row), RHSessionManager::IsExpiredRole).toBool()) continue;
        const auto p = d->readProfile(row);
        if (p.slot >= 0 && !p.id.isEmpty()) d->profiles.append(p);
    }
    if (d->profiles.isEmpty()) return false;
    ++d->generation; d->busy = true; d->complete = false; d->error.clear(); d->results.clear();
    d->profile = -1; d->account = 0; d->offset = 0;
    d->symbols = std::move(symbols); d->side = std::move(side); d->watch.start();
    if (d->notify() && d->busy) d->later([](State &s) { s.nextProfile(); });
    return true;
}
void RHAutoTradeController::cancel() { d->stop(stopped); }
void RHAutoTradeController::clearResults() {
    if (d->busy || d->retiring) return;
    d->results.clear(); d->complete = false; d->error.clear(); d->notify();
}
void RHAutoTradeController::setExternalBusy(bool busy) {
    if (d->externalBusy == busy) return;
    d->externalBusy = busy; d->notify();
}
