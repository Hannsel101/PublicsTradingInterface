#include "rhtradecontroller.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <utility>
#include <memory>

namespace {
bool matches(const QString &s, const QString &pattern) {
    return QRegularExpression("\\A(?:" + pattern + ")\\z").match(s).hasMatch();
}
bool decimal(const QString &s, bool positive = false) {
    return matches(s, "(?:0|[1-9][0-9]{0,11})(?:\\.[0-9]{1,12})?")
        && (!positive || s.contains(QRegularExpression("[1-9]")));
}
// Fixed-width base-10 arithmetic: 12 integer and 12 fractional digits.
// Broker quantities never pass through floating point or a 64-bit sum.
QString scaledDecimal(const QString &s) {
    const auto parts = s.split('.');
    return parts.first().rightJustified(12, '0') + parts.value(1).leftJustified(12, '0');
}
bool subtractDecimal(QString &quantity, const QString &held) {
    if (!decimal(held)) return false;
    const auto rhs = scaledDecimal(held);
    if (quantity < rhs) return false;
    int borrow = 0;
    for (int i = quantity.size() - 1; i >= 0; --i) {
        int digit = quantity.at(i).digitValue() - rhs.at(i).digitValue() - borrow;
        borrow = digit < 0 ? 1 : 0;
        if (digit < 0) digit += 10;
        quantity[i] = QChar('0' + digit);
    }
    return true;
}
QString unscaledDecimal(QString quantity) {
    quantity.insert(12, '.');
    while (quantity.startsWith('0') && quantity.at(1) != '.') quantity.remove(0, 1);
    while (quantity.endsWith('0')) quantity.chop(1);
    if (quantity.endsWith('.')) quantity.chop(1);
    return quantity;
}
QString safeMessage(QString s, const QByteArray &token) {
    // Network responses are already capped at 1 MiB. Preserve useful rejection
    // explanations independently from the short account-label presentation cap.
    s.remove(QRegularExpression("[\\p{Cf}\\p{Cs}<>]"));
    s.replace(QRegularExpression("[\\p{Cc}\\p{Zl}\\p{Zp}]"), " ");
    if (!token.isEmpty()) s.replace(QString::fromLatin1(token), "[redacted]");
    static const QRegularExpression bearer(R"re(\bBearer\s+[^\s,;"'<>]+)re", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fields(
        R"re((\b(?:[A-Za-z0-9_-]*(?:token|password|secret|cookie)[A-Za-z0-9_-]*|authorization|refresh)\b["']?\s*[:=]\s*)(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[^\s,;&}\]]+))re",
        QRegularExpression::CaseInsensitiveOption);
    s.replace(bearer, "Bearer [redacted]");
    s.replace(fields, "\\1[redacted]");
    constexpr qsizetype messageLimit = 64 * 1024;
    if (s.size() > messageLimit) s = s.left(messageLimit) + " [message truncated]";
    return s;
}
QString safeLabel(QString s) {
    s = s.left(128);
    s.remove(QRegularExpression("[\\x00-\\x1f\\x7f<>]"));
    return s;
}
}
RHTradeController::RHTradeController(RHNetworkEngine &network, Dependencies dependencies, QObject *parent)
    : QObject(parent), m_network(network), m_deps(std::move(dependencies)) { refreshAvailability(); }
RHTradeController::~RHTradeController() {
    m_finishing = true;
    if (m_posting && m_row < m_results.size()) {
        auto row = m_results[m_row].toMap();
        row.insert("status", "unknown"); row.insert("success", false); row.insert("orderId", "");
        row.insert("message", "Submission outcome unknown. Check the broker before retrying. lifecycle[destroyed] " + m_attemptDiagnostic);
        m_results[m_row] = row;
    }
    ++m_epoch;
    if (m_reply) { disconnect(m_reply, nullptr, this, nullptr); m_reply->abort(); m_reply->deleteLater(); }
    m_session.token.fill('\0'); m_session.token.clear();
    if (m_locked && m_releaseLease) m_releaseLease();
}

// Signals can synchronously cancel, restart or destroy this object. Never continue
// through an external call unless both its lifetime and batch epoch still match.
bool RHTradeController::notify() {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
#define RH_NOTIFY(name) emit name(); if (!guard || guard->m_epoch != epoch) return false
    RH_NOTIFY(tradeResultsChanged);
    RH_NOTIFY(currentTransactionTitleChanged);
    RH_NOTIFY(tradeResultsVisibleChanged);
    RH_NOTIFY(tradeResultsCompleteChanged);
    RH_NOTIFY(errorChanged);
    RH_NOTIFY(unavailableReasonChanged);
    RH_NOTIFY(availableChanged);
    RH_NOTIFY(tradeResultsBusyChanged);
#undef RH_NOTIFY
    return true;
}
void RHTradeController::refreshAvailability() {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    auto callback = m_deps.availabilityReason;
    const QString reason = callback ? callback() : QString("Trading session provider is unavailable.");
    if (!guard || guard->m_epoch != epoch) return;
    m_unavailable = reason;
    emit unavailableReasonChanged();
    if (!guard || guard->m_epoch != epoch) return;
    emit availableChanged();
}
bool RHTradeController::submit(QString input, QString side) {
    if (m_busy || m_finishing) return false;
    QStringList symbols;
    if (input.size() <= 1024 && (side == "buy" || side == "sell")) {
        for (const auto &entry : input.split(',')) {
            const auto candidate = entry.trimmed();
            if (!matches(candidate, "[A-Za-z][A-Za-z0-9]{0,9}(?:[.-][A-Za-z0-9]{1,4})?")) { symbols.clear(); break; }
            const auto symbol = candidate.toUpper();
            if (!symbols.contains(symbol)) symbols.append(symbol);
        }
    }
    if (symbols.isEmpty() || symbols.size() > 20 || !m_deps.acquire || !m_deps.current || !m_deps.release) {
        m_error = "Enter 1–20 valid comma-separated stock symbols and select Buy or Sell.";
        emit errorChanged(); return false;
    }
    QPointer<RHTradeController> guard(this); const auto before = m_epoch;
    refreshAvailability();
    if (!guard || guard->m_epoch != before || !available()) return false;
    const auto epoch = ++m_epoch;
    m_buyPlans.clear(); m_preflighting = false;
    m_symbols = symbols; m_side = side; m_row = 0; m_results.clear(); m_error.clear(); m_title.clear();
    m_busy = true; m_complete = false; m_visible = true; m_cancelled = false; m_acquiring = true;
    if (!notify()) return true;
    auto acquire = m_deps.acquire;
    auto released = m_deps.release;
    const auto relinquished = std::make_shared<bool>(false);
    const auto delivered = std::make_shared<bool>(false);
    auto releaseOnce = [released, relinquished] {
        if (!std::exchange(*relinquished, true) && released) released();
    };
    // Acquisition can lock the session before its callback, even when it fails.
    // Share exactly-once cleanup with late callbacks so an old request cannot
    // unlock a subsequent batch after this controller has been destroyed.
    m_releaseLease = releaseOnce; m_locked = true;
    acquire([guard, epoch, releaseOnce, delivered](bool ok, Session session, QString reason) mutable {
        if (std::exchange(*delivered, true)) { session.token.fill('\0'); return; }
        if (!guard || guard->m_epoch != epoch) { session.token.fill('\0'); releaseOnce(); return; }
        reason = safeMessage(reason, session.token);
        QTimer::singleShot(0, guard, [guard, epoch, ok, session = std::move(session), reason, releaseOnce]() mutable {
            if (!guard || guard->m_epoch != epoch || !guard->m_acquiring) { session.token.fill('\0'); releaseOnce(); return; }
            guard->m_acquiring = false;
            if (!ok) { session.token.fill('\0'); guard->finish(reason.isEmpty() ? "Unable to acquire the active primary trading session." : reason); return; }
            if (guard->m_cancelled) { guard->finish("Cancelled. Already submitted server orders were not cancelled."); return; }
            guard->begin(std::move(session));
        });
    });
    return true;
}
bool RHTradeController::valid() {
    if (!m_busy || m_cancelled || m_session.authKind != RHNetworkEngine::AuthKind::OAuthBearer
        || !RHNetworkEngine::safeBearer(m_session.token) || m_session.sessionIndex < 0 || m_session.profileId.isEmpty()
        || !m_session.expiresAt.isValid() || m_session.expiresAt <= QDateTime::currentDateTimeUtc()) return false;
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    auto current = m_deps.current; const Session snapshot = m_session;
    const bool ok = current && current(snapshot);
    return guard && guard->m_epoch == epoch && ok && guard->m_busy && !guard->m_cancelled
        && guard->m_session.expiresAt > QDateTime::currentDateTimeUtc();
}
bool RHTradeController::locallyValid() {
    if (!m_busy || m_cancelled || m_session.authKind != RHNetworkEngine::AuthKind::OAuthBearer
        || !RHNetworkEngine::safeBearer(m_session.token) || m_session.sessionIndex < 0 || m_session.profileId.isEmpty()
        || !m_session.expiresAt.isValid() || m_session.expiresAt <= QDateTime::currentDateTimeUtc()) return false;
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    auto current = m_deps.currentLocal; const Session snapshot = m_session;
    const bool ok = current && current(snapshot);
    return guard && guard->m_epoch == epoch && ok && guard->m_busy && !guard->m_cancelled
        && guard->m_session.expiresAt > QDateTime::currentDateTimeUtc();
}
void RHTradeController::finish(QString error) {
    if (!m_busy || m_finishing) return;
    m_finishing = true; ++m_epoch;
    if (m_reply) { disconnect(m_reply, nullptr, this, nullptr); m_reply->abort(); m_reply->deleteLater(); m_reply.clear(); }
    m_posting = false; m_preflighting = false; m_buyPlans.clear();
    for (auto &v : m_results) {
        auto row = v.toMap();
        if (row.value("status") == "pending") {
            row.insert("status", "cancelled"); row.insert("message", "Not sent; batch stopped."); v = row;
        }
    }
    m_session.token.fill('\0'); m_session = {};
    m_error = error; m_complete = true; m_busy = false; m_title.clear();
    const bool release = std::exchange(m_locked, false);
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    auto callback = std::exchange(m_releaseLease, {});
    if (release && callback) callback();
    if (!guard || guard->m_epoch != epoch) return;
    m_finishing = false;
    notify();
}
void RHTradeController::cancel() {
    if (!m_busy || m_cancelled || m_finishing) return;
    m_cancelled = true;
    if (m_acquiring) return; // Retain the busy gate until the outstanding acquisition relinquishes its lock.
    if (m_posting && m_row < m_results.size()) {
        auto row = m_results[m_row].toMap(); row.insert("status", "unknown"); row.insert("success", false);
        row.insert("message", "Submission outcome unknown. Check the broker before retrying; cancellation does not cancel server orders. lifecycle[cancelled] " + m_attemptDiagnostic);
        m_results[m_row] = row;
    }
    finish("Cancelled. Already submitted server orders were not cancelled. Check the broker before retrying any unknown order.");
}
void RHTradeController::dismissTradeResults() {
    if (m_busy) return;
    m_visible = false; emit tradeResultsVisibleChanged();
}
void RHTradeController::begin(Session session) {
    m_session = std::move(session);
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Primary trading authorization expired or changed."); return; }
    if (m_session.accounts.isEmpty() || m_session.accounts.size() > 20 || m_session.accounts.size() * m_symbols.size() > 100) {
        finish("Invalid or excessive active account scope."); return;
    }
    QSet<QString> numbers, urls;
    for (const auto &raw : m_session.accounts) {
        const auto a = raw.toMap(); const QString n = a.value("accountNumber").toString(), u = a.value("_url").toString();
        if (!matches(n, "[A-Za-z0-9_-]{1,128}") || numbers.contains(n) || urls.contains(u)
            || !m_network.stockUrl(QUrl(u, QUrl::StrictMode), "accounts")) { finish("Invalid or duplicate active brokerage account."); return; }
        numbers.insert(n); urls.insert(u);
    }
    const auto populateResults = [this] {
        m_results.clear();
        for (const auto &symbol : m_symbols) for (const auto &raw : m_session.accounts) {
            const auto a = raw.toMap();
            m_results.append(QVariantMap{{"accountNumber", a.value("accountNumber")}, {"accountLabel", safeLabel(a.value("displayName").toString())},
                {"symbol", symbol}, {"quantity", m_side == "buy" ? "1" : ""}, {"side", m_side}, {"status", "pending"},
                {"success", false}, {"message", "Waiting"}, {"orderId", ""}, {"refId", ""}});
        }
    };
    populateResults();
    if (!notify()) return;
    // Fresh authoritative state selects eligibility; cached display rows deliberately
    // carry identity, not trading state. Keep the displayed order after filtering.
    pages(Read::Accounts, {}, [this, populateResults](QJsonArray accounts) {
        QSet<QString> found, freshUrls;
        QSet<QString> active;
        for (const auto &raw : accounts) {
            const auto a = raw.toObject();
            const auto n = a.value("account_number").toString();
            const auto u = a.value("url").toString();
            const auto state = a.value("state").toString();
            if (!matches(n, "[A-Za-z0-9_-]{1,128}") || found.contains(n) || freshUrls.contains(u)
                || !m_network.stockUrl(QUrl(u, QUrl::StrictMode), "accounts")
                || !matches(state, "[a-z_]{1,32}")) {
                finish("Fresh brokerage account scope is invalid."); return;
            }
            bool match = false;
            for (const auto &frozen : m_session.accounts) {
                const auto f = frozen.toMap();
                if (f.value("accountNumber").toString() == n) {
                    if (f.value("_url").toString() != u) { finish("Fresh account identity changed; no orders were sent."); return; }
                    match = true;
                }
            }
            if (state == "active") {
                if (!match) { finish("Fresh active account scope changed; no orders were sent."); return; }
                active.insert(n);
            }
            found.insert(n); freshUrls.insert(u);
        }
        QVariantList eligible;
        for (const auto &frozen : m_session.accounts) {
            const auto n = frozen.toMap().value("accountNumber").toString();
            if (!found.contains(n)) { finish("A displayed account is missing from the fresh scope; no orders were sent."); return; }
            if (active.contains(n)) eligible.append(frozen);
        }
        if (eligible.isEmpty()) {
            m_results.clear();
            finish("No active brokerage accounts remain; no orders were sent."); return;
        }
        m_session.accounts = std::move(eligible);
        populateResults();
        if (!notify()) return;
        if (m_side == "buy") preflightBuy(); else nextRow();
    });
}
void RHTradeController::preflightBuy() {
    m_preflighting = true;
    if (m_buyPlans.size() == m_symbols.size()) { m_preflighting = false; nextRow(); return; }
    pages(Read::Instruments, m_symbols.at(m_buyPlans.size()), [this](QJsonArray a) { resolveInstrument(a); });
}
void RHTradeController::pages(Read read, QString scope, PageDone done, QUrl page, QJsonArray accumulated, QSet<QString> visited) {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Trading session expired or changed."); return; }
    if (visited.size() >= 20 || visited.contains(page.toString())) { result("failed", "Unsafe or excessive pagination.", false, {}, true); return; }
    visited.insert(page.toString());
    auto *reply = m_network.readStock(read, m_session.token, m_session.authKind, scope, page);
    if (!reply) { result("failed", "Stock lookup was rejected before dispatch.", false, {}, true); return; }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, guard, epoch, reply, read, scope, done, accumulated, visited]() mutable {
        reply->deleteLater();
        if (!guard || guard->m_epoch != epoch || !m_busy || m_reply != reply) return;
        m_reply.clear();
        if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Trading session expired or changed."); return; }
        const QByteArray bytes = reply->isReadable() ? reply->readAll() : QByteArray{}; QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
            || bytes.size() > 1024 * 1024 || error.error != QJsonParseError::NoError || !document.isObject()
            || !document.object().value("results").isArray()) { result("failed", "Fresh stock lookup failed; no order sent for this row.", false, {}, true); return; }
        const auto object = document.object(); const auto rows = object.value("results").toArray();
        if (accumulated.size() + rows.size() > 10000) { result("failed", "Excessive stock lookup response.", false, {}, true); return; }
        for (const auto &v : rows) { if (!v.isObject()) { result("failed", "Invalid stock lookup row.", false, {}, true); return; } accumulated.append(v); }
        const auto next = object.value("next");
        if (m_preflighting && !next.isNull() && !next.isUndefined()) {
            result("failed", "Unexpected preflight pagination; fetch a new batch after checking the ticker.", false, {}, true); return;
        }
        if (next.isString() && !next.toString().isEmpty()) { pages(read, scope, done, QUrl(next.toString(), QUrl::StrictMode), accumulated, visited); return; }
        if (!next.isNull() && !((read == Read::Quotes || read == Read::Accounts) && next.isUndefined())) { result("failed", "Incomplete stock lookup response.", false, {}, true); return; }
        done(accumulated);
    });
}
void RHTradeController::nextRow() {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Trading session expired or changed."); return; }
    if (m_row >= m_results.size()) { finish(); return; }
    const auto row = m_results.at(m_row).toMap();
    m_title = row.value("side").toString() + " " + row.value("symbol").toString() + " — " + row.value("accountLabel").toString();
    if (!notify()) return;
    if (m_side == "buy") {
        const auto &plan = m_buyPlans.at(m_row / m_session.accounts.size());
        dispatch(plan.instrument, "1", plan.quote.value("ask_price").toString(), plan.quote.value("bid_price").toString(),
                 plan.kind, plan.price, plan.capturedAt);
    } else pages(Read::Instruments, row.value("symbol").toString(), [this](QJsonArray a) { resolveInstrument(a); });
}
void RHTradeController::resolveInstrument(QJsonArray instruments) {
    const auto symbol = m_preflighting ? m_symbols.at(m_buyPlans.size()) : m_results.at(m_row).toMap().value("symbol").toString();
    const auto i = instruments.size() == 1 ? instruments.first().toObject() : QJsonObject{};
    const QUrl url(i.value("url").toString(), QUrl::StrictMode);
    if (i.value("symbol").toString() != symbol || !m_network.stockUrl(url, "instruments")
        || url.path().split('/').value(2) != i.value("id").toString()
        || !i.value("tradeable").isBool() || !i.value("tradeable").toBool()
        || i.value("tradability").toString() != "tradable" || i.value("state").toString() != "active") {
        result("failed", "Ticker did not resolve to exactly one active tradable instrument."); return;
    }
    if (m_side == "buy") pages(Read::Quotes, symbol, [this, i](QJsonArray q) { resolveQuote(q, i); });
    else pages(Read::Positions, m_results.at(m_row).toMap().value("accountNumber").toString(), [this, i](QJsonArray p) { resolvePosition(p, i); });
}
void RHTradeController::resolveQuote(QJsonArray quotes, QJsonObject instrument, QString quantity) {
    const auto q = quotes.size() == 1 ? quotes.first().toObject() : QJsonObject{};
    if (q.value("symbol") != instrument.value("symbol") || q.value("instrument") != instrument.value("url")
        || !decimal(q.value("ask_price").toString(), true) || !decimal(q.value("bid_price").toString(), true)) {
        result("failed", "Missing or mismatched fresh quote."); return;
    }
    if (m_preflighting) {
        if (!decimal(q.value("last_trade_price").toString(), true)) { result("failed", "Buy preflight: invalid last trade price; no orders sent.", false, {}, true); return; }
        BuyPlan plan; plan.instrument = instrument; plan.quote = q; plan.capturedAt = QDateTime::currentDateTimeUtc();
        plan.price = q.value("ask_price").toString();
        if (scaledDecimal(q.value("last_trade_price").toString()) < scaledDecimal("1")) {
            plan.kind = RHNetworkEngine::BuyKind::FixedLimit;
            QString sum = scaledDecimal(plan.price);
            const QString cent = scaledDecimal("0.01");
            int carry = 0;
            for (int j = sum.size() - 1; j >= 0; --j) {
                const int digit = sum.at(j).digitValue() + cent.at(j).digitValue() + carry;
                sum[j] = QChar('0' + digit % 10); carry = digit / 10;
            }
            if (carry) { result("failed", "Buy preflight: ask plus $0.01 overflows supported decimal precision; no orders sent.", false, {}, true); return; }
            plan.price = unscaledDecimal(sum);
        }
        m_buyPlans.append(std::move(plan)); preflightBuy(); return;
    }
    dispatch(instrument, quantity, q.value("ask_price").toString(), q.value("bid_price").toString());
}
void RHTradeController::resolvePosition(QJsonArray positions, QJsonObject instrument) {
    const auto account = m_session.accounts.at(m_row % m_session.accounts.size()).toMap();
    QJsonObject position;
    for (const auto &raw : positions) {
        const auto p = raw.toObject();
        if (p.value("account").toString() != account.value("_url").toString()
            || (p.contains("account_number") && p.value("account_number").toString() != account.value("accountNumber").toString())) {
            result("failed", "Position response belongs to a different account.", false, {}, true); return;
        }
        if (p.value("instrument") != instrument.value("url")) continue;
        if (!position.isEmpty()) { result("failed", "Duplicate matching positions; no order sent."); return; }
        position = p;
    }
    if (position.isEmpty()) { result("skipped", "No position held in this account."); return; }
    if (!decimal(position.value("quantity").toString())) { result("failed", "Invalid position quantity."); return; }
    QString quantity = scaledDecimal(position.value("quantity").toString());
    for (const auto &field : {"shares_held_for_sells", "shares_held_for_stock_grants", "shares_held_for_options_collateral", "shares_held_for_options_events"}) {
        if (!subtractDecimal(quantity, position.value(field).toString())) {
            result("failed", "Missing, invalid, or excessive reserved shares; no order sent."); return;
        }
    }
    quantity = unscaledDecimal(quantity);
    if (!decimal(quantity, true)) { result("skipped", "No unreserved sellable shares in this account."); return; }
    pages(Read::Quotes, instrument.value("symbol").toString(), [this, instrument, quantity](QJsonArray q) { resolveQuote(q, instrument, quantity); });
}
void RHTradeController::dispatch(QJsonObject instrument, QString quantity, QString ask, QString bid,
                                 RHNetworkEngine::BuyKind kind, QString price, QDateTime capturedAt) {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Trading session expired or changed."); return; }
    auto row = m_results[m_row].toMap(); row.insert("quantity", quantity);
    if (m_side == "buy") {
        row.insert("orderType", kind == RHNetworkEngine::BuyKind::FixedLimit ? "limit" : "market");
        row.insert("price", price); row.insert("askPrice", ask); row.insert("bidPrice", bid);
        row.insert("quoteTimestamp", capturedAt.toUTC().toString(Qt::ISODateWithMs));
    }
    row.insert("refId", QUuid::createUuid().toString(QUuid::WithoutBraces)); m_results[m_row] = row;
    if (!notify()) return;
    if (!valid()) { if (guard && guard->m_epoch == epoch) finish("Trading session expired or changed."); return; }
    if (!liveOrdersEnabled()) { result("preview", "Debug preview only; no stock order was sent.", true); return; }
    const auto account = m_session.accounts.at(m_row % m_session.accounts.size()).toMap();
    RHNetworkEngine::StockOrder order;
    order.accountNumber = row.value("accountNumber").toString(); order.account = QUrl(account.value("_url").toString());
    order.instrument = QUrl(instrument.value("url").toString()); order.symbol = row.value("symbol").toString();
    order.quantity = quantity; order.side = m_side; order.buyKind = kind; order.price = price; order.quoteTimestamp = capturedAt; order.askPrice = ask; order.bidPrice = bid; order.refId = row.value("refId").toString();
    auto *reply = m_network.sendStockOrder(order, m_session.token, m_session.authKind, nullptr, m_deps.loopbackLiveOrdersForTests);
    if (!reply) { result("failed", "Order rejected before dispatch.", false, {}, true); return; }
    m_reply = reply; m_posting = true;
    m_attemptDiagnostic = "post[http=0,qt=0,pending] lookup[not_dispatched]";
    connect(reply, &QNetworkReply::finished, this, [this, guard, epoch, reply, order] {
        reply->deleteLater();
        if (!guard || guard->m_epoch != epoch || !m_busy || m_reply != reply) return;
        m_reply.clear(); orderReceipt(reply, order);
    });
}
void RHTradeController::orderReceipt(QNetworkReply *reply, const RHNetworkEngine::StockOrder &order,
                                    bool lookup, QString candidate, QString postDiagnostic, bool ownershipVerified) {
    QPointer<RHTradeController> guard(this); const auto epoch = m_epoch;
    const auto bytes = reply->isReadable() ? reply->readAll() : QByteArray{};
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse); const auto object = doc.object();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int networkError = int(reply->error());
    const bool boundedObject = bytes.size() <= 1024 * 1024 && parse.error == QJsonParseError::NoError && doc.isObject();
    if (!lookup && boundedObject && reply->error() != QNetworkReply::OperationCanceledError
        && reply->error() != QNetworkReply::TimeoutError && status >= 400 && status < 500 && status != 408) {
        m_posting = false;
        const QString reason = safeMessage(object.value("detail").toString(), m_session.token);
        result("failed", reason.isEmpty() ? "Broker rejected the order." : reason, false, {}, status == 401 || status == 403);
        return;
    }
    // Static categories and numeric metadata only: no provider values or secrets.
    QStringList reasons;
    if (networkError) reasons.append("transport");
    if (lookup ? status != 200 : status < 200 || status >= 300) reasons.append("http");
    if (!boundedObject) reasons.append("json_or_size");
    const auto check = [&](const char *key, const QString &expected) {
        if (!object.contains(key)) reasons.append(QString(key) + "_absent");
        else if (!object.value(key).isString() || object.value(key).toString() != expected)
            reasons.append(QString(key) + "_mismatch");
    };
    check("account", order.account.toString()); check("instrument", order.instrument.toString());
    check("ref_id", order.refId); check("side", order.side);
    if (object.contains("symbol")) check("symbol", order.symbol);
    if (!object.contains("quantity")) reasons.append("quantity_absent");
    else if (!object.value("quantity").isString() || !decimal(object.value("quantity").toString())
        || scaledDecimal(object.value("quantity").toString()) != scaledDecimal(order.quantity)) reasons.append("quantity_mismatch");
    const QString id = object.value("id").toString();
    const bool safeId = object.value("id").isString() && matches(id, "[A-Za-z0-9_-]{1,128}")
        && !id.contains(QString::fromLatin1(m_session.token)) && id != order.refId;
    if (!safeId) reasons.append(object.contains("id") ? "id_invalid" : "id_absent");
    if (lookup && id != candidate) reasons.append("id_mismatch");
    bool duplicate = false;
    for (int i = 0; i < m_results.size(); ++i) {
        if (i == m_row) continue;
        const auto row = m_results.at(i).toMap();
        if ((!id.isEmpty() && (row.value("orderId").toString() == id || row.value("refId").toString() == id))
            || row.value("refId").toString() == order.refId) duplicate = true;
    }
    if (duplicate) reasons.append("duplicate_identity");
    const QString state = object.value("state").toString();
    const bool accepted = QStringList{"queued", "unconfirmed", "confirmed", "partially_filled", "filled"}.contains(state);
    if (!accepted && !QStringList{"rejected", "cancelled", "failed"}.contains(state)) reasons.append("state_unknown");
    const bool current = lookup ? ownershipVerified && locallyValid() : valid();
    if (!guard || guard->m_epoch != epoch) return;
    if (!current) reasons.append("session_changed");
    const QString diagnostic = QString("%1[http=%2,qt=%3,%4]").arg(lookup ? "lookup" : "post")
        .arg(status).arg(networkError).arg(reasons.join(','));
    if (reasons.isEmpty()) {
        m_posting = false;
        result(state == "filled" ? "filled" : accepted ? "submitted" : "failed",
            state == "filled" ? "Broker confirmed filled." : accepted ? "Submitted to broker; not a confirmed fill." : "Broker did not accept this order.", accepted, id);
        return;
    }
    if (!lookup && boundedObject && safeId && !duplicate && current) {
        // Candidate remains untrusted and is never displayed as a verified ID.
        auto *read = m_network.readStockOrder(id, m_session.token, m_session.authKind);
        if (read) {
            m_reply = read; // Keep m_posting true: the POST has already escaped.
            m_attemptDiagnostic = diagnostic + " lookup[http=0,qt=0,pending]";
            struct OwnershipWatch {
                bool inFlight = false, responseReady = false;
                QElapsedTimer lifetime, refresh;
            };
            auto state = std::make_shared<OwnershipWatch>(); state->lifetime.start();
            const QPointer<QNetworkReply> pending(read);
            auto *watch = new QTimer(read); watch->setInterval(50);
            const int deadline = m_deps.loopbackLiveOrdersForTests && m_network.stockLiveOrdersEnabled(true)
                ? qBound(1, m_deps.ownershipDeadlineMsForTests, 16000) : 16000;
            auto stopUnknown = [this, guard, epoch, pending, diagnostic, watch](QString reason) {
                if (!guard || guard->m_epoch != epoch || !pending || m_reply != pending) return;
                watch->stop(); disconnect(pending, nullptr, this, nullptr); m_reply.clear();
                pending->abort();
                if (!guard || guard->m_epoch != epoch || !pending) return;
                pending->deleteLater(); m_posting = false;
                result("unknown", "Submission outcome unknown. Check the broker before retrying. No automatic retry was made. "
                    + diagnostic + QString(" lookup[http=%1,qt=%2,%3]")
                        .arg(pending->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                        .arg(int(pending->error())).arg(reason), false, {}, true);
            };
            auto check = [this, guard, epoch, pending, state, order, id, diagnostic, watch, deadline, stopUnknown] {
                if (!guard || guard->m_epoch != epoch || !pending || m_reply != pending) return;
                const bool local = locallyValid();
                if (!guard || guard->m_epoch != epoch || !pending || m_reply != pending) return;
                if (!local) { stopUnknown("session_changed"); return; }
                if (state->lifetime.elapsed() >= deadline) { stopUnknown("ownership_timeout"); return; }
                if (state->inFlight || (!state->responseReady && state->refresh.isValid()
                                       && state->refresh.elapsed() < 250)) return;
                auto current = m_deps.currentAsync;
                if (!current) { stopUnknown("ownership_unavailable"); return; }
                const bool acceptanceProbe = state->responseReady;
                state->inFlight = true; state->refresh.start();
                current(m_session, [this, guard, epoch, pending, state, order, id, diagnostic, watch, deadline, stopUnknown, acceptanceProbe](bool ok) {
                    if (!guard || guard->m_epoch != epoch || !pending || m_reply != pending) return;
                    state->inFlight = false;
                    if (state->lifetime.elapsed() >= deadline) { stopUnknown("ownership_timeout"); return; }
                    if (!ok) { stopUnknown("session_changed"); return; }
                    if (!acceptanceProbe) return; // Only a probe started after GET completion can accept it.
                    const bool local = locallyValid();
                    if (!guard || guard->m_epoch != epoch || !pending || m_reply != pending) return;
                    if (!local) { stopUnknown("session_changed"); return; }
                    watch->stop(); m_reply.clear(); pending->deleteLater();
                    orderReceipt(pending, order, true, id, diagnostic, true);
                });
            };
            connect(read, &QNetworkReply::finished, this, [state, check, stopUnknown, pending] {
                if (!pending) return;
                if (pending->error() != QNetworkReply::NoError) { stopUnknown("transport"); return; }
                state->responseReady = true; check();
            });
            connect(watch, &QTimer::timeout, this, check);
            watch->start(); check(); return;
        }
    }
    m_posting = false;
    result("unknown", "Submission outcome unknown. Check the broker before retrying. No automatic retry was made. "
        + (lookup ? postDiagnostic + " " : QString()) + diagnostic
        + (!lookup ? " lookup[not_dispatched]" : ""), false, {}, true);
}
void RHTradeController::result(QString status, QString message, bool success, QString orderId, bool halt) {
    if (!m_busy) return;
    if (m_preflighting) {
        halt = true;
        message = "Buy preflight failed for " + m_symbols.at(m_buyPlans.size()) + ": " + message
            + " No orders were sent for any ticker. Check the ticker and quote, then submit a new batch.";
    }
    if (m_row >= m_results.size()) { finish(message); return; }
    auto row = m_results[m_row].toMap(); row.insert("status", status); row.insert("message", message);
    row.insert("success", success); row.insert("orderId", orderId); m_results[m_row] = row;
    if (halt) m_error = message;
    const auto epoch = m_epoch; QPointer<RHTradeController> guard(this);
    if (!notify()) return;
    const int delay = QRandomGenerator::global()->bounded(2000, 3001);
    auto callback = [guard, epoch, halt, message] {
        if (!guard || guard->m_epoch != epoch) return;
        QTimer::singleShot(0, guard, [guard, epoch, halt, message] {
            if (!guard || guard->m_epoch != epoch || !guard->m_busy) return;
            if (halt) { guard->finish(message); return; }
            ++guard->m_row; guard->nextRow();
        });
    };
    auto schedule = m_deps.schedule;
    if (schedule) schedule(delay, callback); else QTimer::singleShot(delay, this, callback);
}
