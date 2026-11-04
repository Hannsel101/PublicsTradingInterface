#include "telegramrobinhoodtradeservice.h"
#include "rhautotradecontroller.h"
#include "rhsessionmanager.h"
#include "rhtradingcontext.h"
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <utility>

// Keep Q_INIT_RESOURCE outside a namespace: the generated symbol is global.
static void initializeRobinhoodResources() { Q_INIT_RESOURCE(rh_config); }
namespace {
using Dependencies = RHTradeController::Dependencies;
using Account = RHSessionManager::StoredSessionAccount;
const QString inventoryFailure = QStringLiteral("Saved Robinhood account inventory could not be loaded completely; no orders were sent. No automatic sign-in was attempted.");
const QString unavailable = QStringLiteral("Robinhood trading is unavailable; no orders were sent. No automatic sign-in was attempted.");
constexpr qsizetype messageLimit = 3900;
QString safeQuantity(const QVariant &value) {
    const auto text = value.toString();
    static const QRegularExpression decimal(QStringLiteral("\\A[0-9]{1,30}(?:\\.[0-9]{1,10})?\\z"));
    return decimal.match(text).hasMatch() ? text : QString();
}
// Split plain Unicode before escaping; never split an HTML entity or surrogate.
QStringList textMessages(const QString &title, const QStringList &lines) {
    QStringList messages;
    QString message = title;
    for (const auto &line : lines) {
        if (message.size() + 1 >= messageLimit) { messages.append(message); message = title; }
        message += '\n';
        for (const auto scalar : line.toUcs4()) {
            const char32_t point = scalar;
            const QString escaped = QString::fromUcs4(&point, 1).toHtmlEscaped();
            if (message.size() + escaped.size() > messageLimit) { messages.append(message); message = title + '\n'; }
            message += escaped;
        }
    }
    if (message != title || messages.isEmpty()) messages.append(message);
    return messages;
}
QStringList warnings(const QStringList &emails) {
    if (emails.isEmpty()) return {};
    const QString head = QStringLiteral("🔴🔴 <b>Robinhood Reauthentication Required for the following Accounts</b> 🔴🔴\n"
        "For each account send command /authenticate_robinhood_account accountEmail to remotely authenticate. (Note: Multi-Factor Authentication may be required)\n"
        "Session Emails Affected: ");
    const QString tail = QStringLiteral("\n🟡🟡 Warning: trades for these emails will not go through during this buy and sell order but can be part of the next set of orders if authenticated 🟡🟡");
    QStringList messages, entries;
    for (const auto &email : emails) {
        const auto escaped = email.toHtmlEscaped();
        if (!entries.isEmpty() && head.size() + entries.join(", ").size() + 2 + escaped.size() + tail.size() > messageLimit) {
            messages.append(head + entries.join(", ") + tail); entries.clear();
        }
        entries.append(escaped);
    }
    if (!entries.isEmpty()) messages.append(head + entries.join(", ") + tail);
    return messages;
}
}
struct TelegramRobinhoodTradeService::State : QObject {
    TelegramRobinhoodTradeService *q;
    QPointer<RHNetworkEngine> network;
    QPointer<RHSessionManager> sessions;
    Dependencies deps;
    bool ownGraph = true, busy = false, finishing = false;
    quint64 generation = 0;
    QString id, side;
    QStringList symbols, notified;
    QList<Account> inventory;
    QPointer<RHAutoTradeController> controller;
    std::unique_ptr<RHNetworkEngine> ownedNetwork;
    std::unique_ptr<RHSessionManager> ownedSessions;
    QTimer dependencyWatch;
    explicit State(TelegramRobinhoodTradeService *owner) : q(owner) {
        dependencyWatch.setInterval(100);
        connect(&dependencyWatch, &QTimer::timeout, this, [this] {
            if (!busy || (network && sessions)) return;
            if (controller) { controller->cancel(); return; }
            settle(textMessages(title(), {"🔴 Saved Robinhood dependencies became unavailable; no orders were sent."}));
        });
    }
    ~State() override { if (controller) delete controller.data(); }
    QString title() const { return QStringLiteral("<b>Robinhood Auto %1 Results</b>").arg(side == "buy" ? "Buy" : "Sell"); }
    template<class F> void later(F callback) {
        const QPointer<State> guard(this); const auto epoch = generation;
        QTimer::singleShot(0, this, [guard, epoch, callback = std::move(callback)]() mutable {
            if (guard && guard->busy && guard->generation == epoch) callback(*guard);
        });
    }
    bool matchesInventory(bool checkExpiry) const {
        if (!sessions || inventory.size() != sessions->rowCount()) return false;
        QSet<int> seenSlots; QSet<QString> seenIds;
        for (int i = 0; i < inventory.size(); ++i) {
            const auto &entry = inventory[i]; const auto index = sessions->index(i);
            if (entry.sessionIndex < 0 || entry.profileId.isEmpty() || entry.email.isEmpty()
                || seenSlots.contains(entry.sessionIndex) || seenIds.contains(entry.profileId)
                || sessions->data(index, RHSessionManager::SessionIndexRole).toInt() != entry.sessionIndex
                || sessions->data(index, RHSessionManager::AccountIdRole).toString() != entry.profileId
                || (checkExpiry && sessions->data(index, RHSessionManager::IsExpiredRole).toBool() != entry.expired)) return false;
            seenSlots.insert(entry.sessionIndex); seenIds.insert(entry.profileId);
        }
        return true;
    }
    QStringList affected(const QVariantList &rows = {}) const {
        QStringList emails;
        for (const auto &entry : inventory) {
            bool expired = entry.expired;
            if (sessions) for (int i = 0; i < sessions->rowCount(); ++i) {
                const auto index = sessions->index(i);
                if (sessions->data(index, RHSessionManager::SessionIndexRole).toInt() == entry.sessionIndex
                    && sessions->data(index, RHSessionManager::AccountIdRole).toString() == entry.profileId)
                    expired |= sessions->data(index, RHSessionManager::IsExpiredRole).toBool();
            }
            for (const auto &raw : rows) {
                const auto row = raw.toMap();
                if (row.value("sessionIndex").toInt() == entry.sessionIndex && row.value("profileId").toString() == entry.profileId)
                    expired |= row.value("nativeRow").toMap().value("authenticationRequired").toBool();
            }
            if (expired && !emails.contains(entry.email)) emails.append(entry.email);
        }
        return emails;
    }
    bool notice(const QStringList &emails) {
        if (emails.isEmpty()) return true;
        bool changed = false;
        for (const auto &email : emails) changed |= !notified.contains(email);
        if (!changed) return true;
        notified = emails;
        const auto request = id; const auto messages = warnings(emails);
        const QPointer<State> guard(this); const auto epoch = generation;
        emit q->reportReady(request, messages);
        return guard && guard->busy && guard->generation == epoch;
    }
    void begin() {
        const QPointer<State> guard(this); const auto epoch = generation;
        if (ownGraph && !network) {
            initializeRobinhoodResources();
            ownedNetwork = std::make_unique<RHNetworkEngine>(); network = ownedNetwork.get();
            if (!network->loadConfiguration(QStringLiteral(":/robinhood/config/robinhood-api.json"))) { settle(textMessages(title(), {"🔴 " + unavailable})); return; }
            ownedSessions = std::make_unique<RHSessionManager>(*network); sessions = ownedSessions.get();
            deps = makeRHTradingDependencies(*sessions);
        }
        if (!network || !sessions || sessions->busy() || sessions->reauthenticating()
            || sessions->browserLoginPending() || sessions->nativeLoginPending()) { settle(textMessages(title(), {"🔴 " + unavailable})); return; }
        dependencyWatch.start(); // Pointer liveness only; no background credential reads.
        // Reload every admitted command so another process's explicit authentication
        // is visible on the next batch. Construction remains credential-free.
        if (ownGraph) {
            const bool loaded = sessions->load();
            if (!guard || generation != epoch || !busy) return;
            if (!loaded) { settle(textMessages(title(), {"🔴 " + inventoryFailure})); return; }
        }
        sessions->requestStoredSessionAccounts([guard, epoch](bool ok, QList<Account> entries, QString) mutable {
            if (!guard || !guard->busy || guard->generation != epoch) return;
            // Never advance native trading in a secure-store/model notification stack.
            guard->later([ok, entries = std::move(entries)](State &s) mutable {
                s.inventory = std::move(entries);
                if (!ok || !s.matchesInventory(true)) { s.settle(textMessages(s.title(), {"🔴 " + inventoryFailure})); return; }
                s.start();
            });
        });
    }
    void start() {
        if (!notice(affected())) return;
        // A notice observer can change membership/authentication. Never admit a
        // newly authenticated formerly-expired profile into this frozen batch.
        if (!matchesInventory(true)) { settle(textMessages(title(), {"🔴 " + inventoryFailure})); return; }
        bool eligible = false; for (const auto &entry : inventory) eligible |= !entry.expired;
        if (!eligible) { settle(textMessages(title(), {"🔴 No initially unexpired saved Robinhood sessions; no orders were sent."})); return; }
        controller = new RHAutoTradeController(*network, *sessions, deps, this);
        connect(controller, &RHAutoTradeController::stateChanged, this, [this] {
            if (controller && controller->tradeResultsComplete()) later([](State &s) { s.done(); });
        });
        const QPointer<State> guard(this); const auto epoch = generation;
        const bool admitted = controller->submit(symbols.join(','), side);
        if (guard && guard->busy && guard->generation == epoch && !admitted)
            settle(textMessages(title(), {"🔴 " + unavailable}));
    }
    QStringList report(const QVariantList &rows) const {
        QStringList lines;
        for (const auto &entry : inventory) {
            QStringList numbers;
            for (const auto &raw : rows) {
                const auto row = raw.toMap();
                if (row.value("sessionIndex").toInt() != entry.sessionIndex || row.value("profileId").toString() != entry.profileId) continue;
                const auto number = row.value("accountNumber").toString(); if (!numbers.contains(number)) numbers.append(number);
            }
            for (const auto &number : numbers) for (int category = 0; category < 3; ++category) {
                QStringList trades;
                for (const auto &raw : rows) {
                    const auto row = raw.toMap();
                    if (row.value("sessionIndex").toInt() != entry.sessionIndex || row.value("profileId").toString() != entry.profileId
                        || row.value("accountNumber").toString() != number) continue;
                    const auto status = row.value("status").toString();
                    const int kind = row.value("preview").toBool() ? 2 : row.value("success").toBool() ? 0 : 1;
                    if (kind != category) continue;
                    // Auto's presentation is safe; native diagnostic strings never
                    // enter Telegram. Preserve native uncertainty independently.
                    const auto native = row.value("nativeRow").toMap();
                    QString detail;
                    if (kind == 2) detail = "Debug preview; not sent";
                    else if (kind == 0) detail = native.value("status") == "unknown" ? "Sent; confirmation unavailable; check broker; no automatic retry"
                        : native.value("orderState") == "filled" && native.value("success").toBool() ? "Sent; broker confirmed filled" : "Sent; not fill-confirmed";
                    else if (status == "warning") detail = "Submission outcome unknown; check broker; no automatic retry";
                    else if (native.value("status") == "skipped") detail = "Skipped; no eligible account or sellable position. Not sent";
                    else if (native.value("orderPostAttempted").toBool()) detail = "Order send/rejection error; check broker before retrying";
                    else detail = "Not sent; batch stopped or session unavailable. Check broker before retrying any uncertain order";
                    const auto quantity = safeQuantity(native.value("quantity"));
                    trades.append(row.value("symbol").toString() + (quantity.isEmpty() ? QString() : " (" + quantity + ")") + ": " + detail);
                }
                if (trades.isEmpty()) continue;
                const QString heading = category == 0 ? "🟢 Sent Trades" : category == 1 ? "🔴 Failed/Not Sent Trades" : "🟡 Preview Trades (not sent)";
                const auto prefix = heading + " — " + entry.email + " / " + (number.isEmpty() ? "Account scope unavailable" : "Account " + number) + ": ";
                // A line per ticker keeps grouping/correlation in every fragment.
                for (const auto &trade : trades) lines.append(prefix + trade);
            }
        }
        if (lines.isEmpty()) lines.append("🔴 No orders were sent; no eligible account results were available.");
        return textMessages(title(), lines);
    }
    void done() {
        if (!busy || finishing || !controller || !controller->tradeResultsComplete()) return;
        finishing = true;
        const auto rows = controller->tradeResults();
        if (!notice(affected(rows))) return;
        settle(report(rows));
    }
    void settle(QStringList messages) {
        if (!busy) return;
        const auto request = id;
        dependencyWatch.stop();
        if (controller) { delete controller.data(); controller.clear(); }
        inventory.clear(); notified.clear(); symbols.clear(); id.clear(); side.clear(); finishing = false;
        ++generation; busy = false;
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
    d->busy = true; d->id = std::move(requestId); d->symbols = std::move(normalized); d->side = std::move(side);
    const QPointer<State> guard(d.get()); const auto epoch = ++d->generation;
    emit busyChanged();
    if (guard && guard->generation == epoch && guard->busy) guard->later([](State &s) { s.begin(); });
    return true;
}
