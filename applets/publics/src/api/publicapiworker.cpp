#include "publicapiworker.h"
#include <QUuid>
#include <array>
#include <charconv>
#include <cmath>
#include <climits>
#include <limits>
#include <functional>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSet>
#include <QSettings>
#include <QLocale>
#include <QTimeZone>

namespace {
// QTimer's integer overload cannot represent waits longer than INT_MAX ms.
// Chunk long Retry-After values instead of silently retrying earlier.
void scheduleAfter(qint64 delayMs, QObject *context, std::function<void()> callback)
{
    if (delayMs <= INT_MAX) {
        QTimer::singleShot(int(qMax<qint64>(0, delayMs)), Qt::PreciseTimer, context, std::move(callback));
    } else {
        QTimer::singleShot(INT_MAX, Qt::PreciseTimer, context,
            [delayMs, context, callback = std::move(callback)]() mutable {
                scheduleAfter(delayMs - INT_MAX, context, std::move(callback));
            });
    }
}

QByteArray responseBody(QNetworkReply *reply)
{
    return reply->isReadable() ? reply->readAll() : QByteArray();
}

bool knownOrderStatus(const QString &status)
{
    static const QSet<QString> statuses = {"NEW", "ACCEPTED", "PARTIALLY_FILLED", "CANCELLED",
        "QUEUED_CANCELLED", "FILLED", "REJECTED", "PENDING_REPLACE", "PENDING_CANCEL", "EXPIRED", "REPLACED"};
    return statuses.contains(status);
}

bool retryable(int status)
{
    return status == 429 || status == 502 || status == 503 || status == 504;
}

qint64 retryDelay(const PublicRequestPolicy &policy, int attempt, const QByteArray &header)
{
    const qint64 ceiling = qMin<qint64>(policy.retryCapMs, qint64(policy.retryBaseMs) << qBound(0, attempt, 10));
    return policy.retryDelayMs(attempt, header, QDateTime::currentDateTimeUtc(),
                               QRandomGenerator::global()->bounded(quint32(ceiling / 4 + 1)));
}

QString quantityString(float quantity)
{
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), quantity);
    if (result.ec != std::errc()) {
        return QString();
    }
    return QString::fromLatin1(buffer.data(), static_cast<qsizetype>(result.ptr - buffer.data()));
}
}

// Retry-After is a minimum, not capped by the exponential-backoff ceiling.
qint64 PublicRequestPolicy::retryDelayMs(int attempt, const QByteArray &header,
                                     const QDateTime &now, quint32 jitterMs) const
{
    const qint64 ceiling = qMin<qint64>(retryCapMs, qint64(retryBaseMs) << qBound(0, attempt, 10));
    qint64 delay = qMin<qint64>(retryCapMs, ceiling + qMin<qint64>(jitterMs, ceiling / 4));
    bool ok = false;
    const qint64 seconds = header.trimmed().toLongLong(&ok);
    if (ok && seconds >= 0)
        delay = qMax(delay, qMin<qint64>(seconds, std::numeric_limits<qint64>::max() / 1000) * 1000);
    else {
        // HTTP IMF-fixdate uses the literal GMT; Qt's RFC2822 parser only
        // accepts a numeric zone on some kits. Parse the HTTP form explicitly.
        auto date = QLocale::c().toDateTime(QString::fromLatin1(header.trimmed()),
                                          QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
        if (date.isValid()) date.setTimeZone(QTimeZone::utc());
        else date = QDateTime::fromString(QString::fromLatin1(header), Qt::RFC2822Date);
        if (date.isValid()) delay = qMax(delay, now.msecsTo(date));
    }
    return qMax<qint64>(0, delay);
}

PublicApiWorker::PublicApiWorker(const QUrl &apiBaseUrl, QObject *parent)
    : QObject(parent), manager(new QNetworkAccessManager(this)), m_apiBaseUrl(apiBaseUrl)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("PublicRequestPolicy"));
    const auto setting = [&settings](const char *key, int fallback) {
        bool ok = false;
        const int value = settings.value(QString::fromLatin1(key), fallback).toInt(&ok);
        return ok ? value : fallback;
    };
    PublicRequestPolicy policy;
    policy.portfolioPauseMs = setting("portfolioPauseMs", policy.portfolioPauseMs);
    policy.symbolPauseMs = setting("symbolPauseMs", policy.symbolPauseMs);
    policy.accountPauseMs = setting("accountPauseMs", policy.accountPauseMs);
    policy.gatewayCooldownMs = setting("gatewayCooldownMs", policy.gatewayCooldownMs);
    policy.retryBaseMs = setting("retryBaseMs", policy.retryBaseMs);
    policy.retryCapMs = setting("retryCapMs", policy.retryCapMs);
    policy.maxRetries = setting("maxRetries", policy.maxRetries);
    policy.timeoutMs = setting("timeoutMs", policy.timeoutMs);
    setRequestPolicy(policy);
}

void PublicApiWorker::setRequestPolicy(const PublicRequestPolicy &policy)
{
    // Change only between operations so one batch always has a stable policy.
    if (m_loadingAccounts || tradeResultsBusy()) return;
    m_policy = policy;
    m_policy.portfolioPauseMs = qBound(0, policy.portfolioPauseMs, 60000);
    m_policy.symbolPauseMs = qBound(0, policy.symbolPauseMs, 60000);
    m_policy.accountPauseMs = qBound(0, policy.accountPauseMs, 60000);
    m_policy.gatewayCooldownMs = qBound(0, policy.gatewayCooldownMs, 300000);
    m_policy.retryBaseMs = qBound(1, policy.retryBaseMs, 60000);
    m_policy.retryCapMs = qBound(m_policy.retryBaseMs, policy.retryCapMs, 300000);
    m_policy.maxRetries = qBound(0, policy.maxRetries, 5);
    m_policy.timeoutMs = qBound(1, policy.timeoutMs, 120000);
}

QStringList PublicApiWorker::parseSymbols(const QString &input)
{
    QStringList symbols;
    for (const QString &part : input.split(QLatin1Char(','))) {
        QString symbol;
        for (const QChar character : part) {
            const ushort letter = character.unicode();
            if ((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))
                symbol.append(character.toUpper());
        }
        if (!symbol.isEmpty()) symbols.append(symbol);
    }
    return symbols;
}

void PublicApiWorker::processToken(QString newToken)
{
    clearSubAccountsList();
    m_token = newToken;
    if (m_token.isEmpty()) {
        return;
    }
    fetchAccounts();
}

QVariantList PublicApiWorker::tradeResults() const
{
    return m_tradeResults;
}

QString PublicApiWorker::currentTransactionTitle() const
{
    return m_currentTransactionTitle;
}

bool PublicApiWorker::tradeResultsVisible() const
{
    return m_tradeResultsVisible;
}

void PublicApiWorker::setTradeResultsVisible(bool visible)
{
    if (m_tradeResultsVisible == visible)
        return;
    m_tradeResultsVisible = visible;
    emit tradeResultsVisibleChanged();
}

bool PublicApiWorker::tradeResultsComplete() const
{
    return m_tradeResultsComplete;
}

bool PublicApiWorker::tradeResultsBusy() const
{
    return !m_tradeResultsComplete;
}

void PublicApiWorker::dismissTradeResults()
{
    setTradeResultsVisible(false);
}

void PublicApiWorker::setCurrentTransactionTitle(const QString &title)
{
    if (m_currentTransactionTitle == title)
        return;
    m_currentTransactionTitle = title;
    emit currentTransactionTitleChanged();
}

void PublicApiWorker::setTradeResultsComplete(bool complete)
{
    if (m_tradeResultsComplete == complete)
        return;
    m_tradeResultsComplete = complete;
    emit tradeResultsCompleteChanged();
    emit tradeResultsBusyChanged();
}

QString PublicApiWorker::transactionTitle(const QString &symbol, const QString &side, bool isPreflight) const
{
    const QString transactionType = isPreflight ? QStringLiteral("Preflight") : QStringLiteral("Market");
    const QString direction = side.compare(QStringLiteral("SELL"), Qt::CaseInsensitive) == 0
                                  ? QStringLiteral("Sell")
                                  : QStringLiteral("Buy");
    return QStringLiteral("%1 %2 of %3")
        .arg(transactionType, direction, symbol.trimmed().toUpper());
}

QString PublicApiWorker::displayLabelForAccount(const QString &accountId, const QString &accountType)
{
    const QString normalizedType = accountType.trimmed().toUpper();
    QString typeLabel;
    if (normalizedType == QStringLiteral("BROKERAGE")) {
        typeLabel = QStringLiteral("Standard Brokerage");
    } else if (normalizedType == QStringLiteral("ROTH_IRA")) {
        typeLabel = QStringLiteral("ROTH IRA");
    } else if (normalizedType == QStringLiteral("TRADITIONAL_IRA")) {
        typeLabel = QStringLiteral("IRA");
    } else if (normalizedType == QStringLiteral("ENTITY")) {
        typeLabel = QStringLiteral("Business");
    } else if (!accountType.trimmed().isEmpty()) {
        typeLabel = accountType.trimmed();
    } else {
        typeLabel = QStringLiteral("Account");
    }

    return QStringLiteral("%1: %2").arg(typeLabel, accountId);
}

void PublicApiWorker::beginTradeResults(const QStringList &symbols, const QString &side, bool isPreflight)
{
    setCurrentTransactionTitle(transactionTitle(symbols.join(QStringLiteral(", ")), side, isPreflight));
    m_tradeResults.clear();
    m_pendingTradeResults = static_cast<int>(m_accountList.size() * symbols.size());

    for (const auto &accountData : m_accountList) {
        for (const QString &symbol : symbols) {
            QVariantMap result;
            result.insert(QStringLiteral("accountId"), accountData.accountId);
            result.insert(QStringLiteral("accountType"), accountData.accountType);
            result.insert(QStringLiteral("accountLabel"), displayLabelForAccount(accountData.accountId, accountData.accountType));
            result.insert(QStringLiteral("symbol"), symbol);
            result.insert(QStringLiteral("status"), QStringLiteral("pending"));
            result.insert(QStringLiteral("success"), false);
            result.insert(QStringLiteral("message"), QStringLiteral("Waiting for Public.com response…"));
            m_tradeResults.append(result);
        }
    }

    if (m_accountList.isEmpty()) {
        QVariantMap result;
        result.insert(QStringLiteral("accountId"), QStringLiteral("No loaded account"));
        result.insert(QStringLiteral("accountType"), QString());
        result.insert(QStringLiteral("accountLabel"), QStringLiteral("No loaded account"));
        result.insert(QStringLiteral("status"), QStringLiteral("failed"));
        result.insert(QStringLiteral("success"), false);
        result.insert(QStringLiteral("message"), QStringLiteral("No eligible brokerage accounts are loaded for the selected API key. Start a new session and try again."));
        m_tradeResults.append(result);
        m_pendingTradeResults = 0;
        setTradeResultsComplete(true);
    } else {
        setTradeResultsComplete(false);
    }

    emit tradeResultsChanged();
    setTradeResultsVisible(true);
}

void PublicApiWorker::updateTradeResult(int resultIndex, bool success, const QString &message, const QString &status)
{
    QVariantMap result = m_tradeResults.at(resultIndex).toMap();
    if (result.value(QStringLiteral("status")) != QStringLiteral("pending")) return;
    result.insert(QStringLiteral("status"), status.isEmpty() ? (success ? QStringLiteral("success") : QStringLiteral("failed")) : status);
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("message"), message);
    m_tradeResults[resultIndex] = result;
    --m_pendingTradeResults;
    emit tradeResultsChanged();
    if (m_pendingTradeResults == 0) setTradeResultsComplete(true);
}

void PublicApiWorker::addAccountToList(QString id,
                                       QString accountType,
                                       const QHash<QString, float> &stockShareBalances, bool holdingsLoaded)
{
    if(id == "")
        return;

    // check if account is already stored
    for(auto account: m_accountList)
    {
        if(account.accountId == id)
        {
            return;
        }
    }

    // store new account
    m_accountList.append(AccountData{id, accountType, stockShareBalances, holdingsLoaded});
}

QHash<QString, float> PublicApiWorker::stockShareBalancesFromPortfolio(const QByteArray &responseData, bool *valid)
{
    if (valid) *valid = false;
    QHash<QString, float> balances;
    const auto doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject() || !doc.object().value("positions").isArray()) return {};
    for (const auto &value : doc.object().value("positions").toArray()) {
        if (!value.isObject()) return {};
        const auto position = value.toObject();
        const auto instrument = position.value("instrument").toObject();
        const auto type = instrument.value("type").toString();
        if (type.isEmpty()) return {};
        if (type.compare("EQUITY", Qt::CaseInsensitive) != 0) continue;
        const auto symbol = instrument.value("symbol").toString().trimmed().toUpper();
        const auto raw = position.value("quantity");
        bool ok = false;
        const float quantity = raw.toVariant().toFloat(&ok);
        if (symbol.isEmpty() || (!raw.isString() && !raw.isDouble()) || !ok || !std::isfinite(quantity)) return {};
        // Duplicate equity rows must not overwrite holdings silently.
        if (balances.contains(symbol)) return {};
        balances.insert(symbol, quantity);
    }
    if (valid) *valid = true;
    return balances;
}

QJsonObject PublicApiWorker::orderPayload(const QString &symbol,
                                          const QString &side,
                                          float quantity)
{
    QJsonObject orderObject;
    orderObject[QStringLiteral("orderId")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    orderObject[QStringLiteral("orderSide")] = side.trimmed().toUpper();
    orderObject[QStringLiteral("orderType")] = QStringLiteral("MARKET");
    orderObject[QStringLiteral("quantity")] = quantityString(quantity);
    orderObject[QStringLiteral("validateOrder")] = QStringLiteral("true");

    QJsonObject instrumentObject;
    instrumentObject[QStringLiteral("symbol")] = symbol.trimmed().toUpper();
    instrumentObject[QStringLiteral("type")] = QStringLiteral("EQUITY");
    orderObject[QStringLiteral("instrument")] = instrumentObject;

    QJsonObject expirationObject;
    expirationObject[QStringLiteral("timeInForce")] = QStringLiteral("DAY");
    orderObject[QStringLiteral("expiration")] = expirationObject;
    return orderObject;
}

void PublicApiWorker::fetchAccounts(int attempt)
{
    const quint64 generation = m_accountLoadGeneration;
    m_loadingAccounts = true;
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/account");
    request.setTransferTimeout(m_policy.timeoutMs);
    QElapsedTimer elapsed;
    elapsed.start();
    auto *reply = manager->get(request);
    scheduleAfter(m_policy.timeoutMs, reply, [reply]() { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, attempt, elapsed]() {
        reply->deleteLater();
        if (generation != m_accountLoadGeneration) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qInfo() << "Public request accounts status" << status << "latencyMs" << elapsed.elapsed()
                << "retry" << attempt << "requestId" << reply->request().attribute(QNetworkRequest::User).toString()
                << "networkError" << int(reply->error());
        if (retryable(status) && attempt < m_policy.maxRetries) {
            scheduleAfter(retryDelay(m_policy, attempt, reply->rawHeader("Retry-After")), this,
                [this, generation, attempt]() { if (generation == m_accountLoadGeneration) fetchAccounts(attempt + 1); });
            return;
        }
        const auto doc = QJsonDocument::fromJson(responseBody(reply));
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300
            || !doc.isObject() || !doc.object().value("accounts").isArray()) {
            m_loadingAccounts = false;
            emit accountLoadFinished(false, QStringLiteral("Could not load accounts (HTTP %1).").arg(status));
            return;
        }
        QSet<QString> seen;
        for (const auto &value : doc.object().value("accounts").toArray()) {
            const auto account = value.toObject();
            const QString type = account.value("accountType").toString();
            const QString id = account.value("accountId").toString();
            if (!id.isEmpty() && !seen.contains(id) &&
                (type == "BROKERAGE" || type == "ROTH_IRA" || type == "TRADITIONAL_IRA" || type == "ENTITY")) {
                seen.insert(id);
                m_accountList.append(AccountData{id, type});
            }
        }
        if (m_accountList.isEmpty()) {
            m_loadingAccounts = false;
            emit accountLoadFinished(false, QStringLiteral("No eligible brokerage accounts were found for this key."));
            return;
        }
        scheduleAfter(m_policy.portfolioPauseMs, this,
            [this, generation]() { fetchPortfolio(0, 0, generation); });
    });
}

void PublicApiWorker::executePreflight(const QString &symbol, const QString &side)
{
    startTrades(symbol, side, true);
}

void PublicApiWorker::executeTrade(const QString &symbol, const QString &side)
{
    startTrades(symbol, side, false);
}

void PublicApiWorker::startTrades(const QString &input, const QString &side, bool isPreflight)
{
    if (tradeResultsBusy() || m_loadingAccounts) return;
    const QStringList symbols = parseSymbols(input);
    const QString direction = side.trimmed().toUpper();
    if (symbols.isEmpty() || (direction != QStringLiteral("BUY") && direction != QStringLiteral("SELL"))) return;

    ++m_tradeGeneration;
    m_tradeSymbols = symbols;
    m_tradeAccounts = m_accountList;
    m_tradeSide = direction;
    m_isPreflight = isPreflight;
    m_nextTradeIndex = 0;
    beginTradeResults(symbols, direction, isPreflight);
    if (!m_tradeAccounts.isEmpty()) dispatchNextTrade(m_tradeGeneration);
}

void PublicApiWorker::dispatchNextTrade(quint64 generation)
{
    if (generation != m_tradeGeneration || m_nextTradeIndex >= m_tradeResults.size()) return;
    if (!m_cooldown.hasExpired()) {
        scheduleAfter(m_cooldown.remainingTime(), this, [this, generation]() { dispatchNextTrade(generation); });
        return;
    }
    const int resultIndex = m_nextTradeIndex++;
    const AccountData &account = m_tradeAccounts.at(resultIndex / m_tradeSymbols.size());
    const QString &symbol = m_tradeSymbols.at(resultIndex % m_tradeSymbols.size());
    if (!account.holdingsLoaded) {
        finishTradeStep(resultIndex, false, QStringLiteral("Holdings unavailable or malformed; no request submitted."), generation, QStringLiteral("not_submitted"));
        return;
    }
    const bool isSellAll = m_tradeSide == QStringLiteral("SELL");
    const float quantity = account.stockShareBalances.value(symbol, 0.0F);
    if (isSellAll && quantity <= 0.0F) {
        finishTradeStep(resultIndex, false,
                        QStringLiteral("No %1 shares are held in this account; no sell order was submitted.").arg(symbol),
                        generation);
        return;
    }

    executeTradeOrPreflight(account.accountId, symbol, m_tradeSide,
                            isSellAll ? quantity : 1.0F, m_isPreflight, resultIndex, generation);
}

void PublicApiWorker::finishTradeStep(int resultIndex, bool success, const QString &message, quint64 generation, const QString &status)
{
    if (generation != m_tradeGeneration) return;
    updateTradeResult(resultIndex, success, message, status);
    if (generation != m_tradeGeneration || m_nextTradeIndex >= m_tradeResults.size()) return;
    const int pauseMs = m_nextTradeIndex % m_tradeSymbols.size() == 0
                            ? m_policy.accountPauseMs : m_policy.symbolPauseMs;
    scheduleAfter(qMax<qint64>(pauseMs, m_cooldown.remainingTime()), this, [this, generation]() {
        dispatchNextTrade(generation);
    });
}

void PublicApiWorker::clearSubAccountsList()
{
    ++m_accountLoadGeneration;
    ++m_tradeGeneration;
    m_accountList.clear();
    m_token = "";
    m_loadingAccounts = false;
    // Invalidated callbacks must not keep a replacement session busy forever.
    // Aborting a live request does not cancel an order at the broker.
    for (auto *reply : manager->findChildren<QNetworkReply *>())
        if (!reply->isFinished()) reply->abort();
    if (tradeResultsBusy()) {
        for (auto &value : m_tradeResults) {
            auto row = value.toMap();
            if (row.value("status").toString() != "pending") continue;
            const bool uncertain = !m_isPreflight && !row.value("orderId").toString().isEmpty();
            row.insert("status", uncertain ? "uncertain" : "not_submitted");
            row.insert("success", false);
            row.insert("message", uncertain
                ? QStringLiteral("Session ended before confirmation. Order ID: %1. Check Public before submitting again.").arg(row.value("orderId").toString())
                : QStringLiteral("Session ended; no live order was submitted for this row."));
            value = row;
        }
        m_pendingTradeResults = 0;
        m_tradeResultsComplete = true;
        emit tradeResultsChanged();
        emit tradeResultsCompleteChanged();
        emit tradeResultsBusyChanged();
    } else {
        dismissTradeResults();
    }
}

void PublicApiWorker::executeTradeOrPreflight(const QString &accountId,
                                              const QString &symbol,
                                              const QString &side,
                                              float quantity,
                                              bool isPreflight,
                                              int resultIndex,
                                              quint64 generation)
{
    const QString endpoint = isPreflight ? "preflight/single-leg" : "order";
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/" + accountId + "/" + endpoint);
    // Redirecting a POST risks a second submission or leaking authorization.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    const auto order = orderPayload(symbol, side, quantity);
    const QString orderId = order.value("orderId").toString();
    auto row = m_tradeResults.at(resultIndex).toMap();
    row.insert("orderId", orderId);
    m_tradeResults[resultIndex] = row;
    QElapsedTimer elapsed;
    elapsed.start();
    auto *reply = manager->post(request, QJsonDocument(order).toJson(QJsonDocument::Compact));
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, [reply]() {
        if (!reply->isFinished()) reply->abort();
    });
    timer->start(m_policy.timeoutMs);
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        timer->stop();
        reply->deleteLater();
        if (generation != m_tradeGeneration) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto doc = QJsonDocument::fromJson(responseBody(reply));
        const auto object = doc.object();
        qInfo() << "Public request" << endpoint << "symbol" << symbol << "side" << side
                << "status" << status << "latencyMs" << elapsed.elapsed() << "retry" << 0
                << "correlation" << orderId << "requestId" << reply->request().attribute(QNetworkRequest::User).toString()
                << "networkError" << int(reply->error());
        if (retryable(status) || status == 0 || status >= 500)
            m_cooldown = QDeadlineTimer(qMax<qint64>(m_policy.gatewayCooldownMs,
                retryDelay(m_policy, 0, reply->rawHeader("Retry-After"))), Qt::PreciseTimer);

        // A gateway error, timeout, lost response, redirect or malformed success
        // cannot prove that a live order was rejected. Never repeat this POST.
        const bool httpSuccess = status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError;
        const bool validResponse = doc.isObject() && (isPreflight
            ? object.contains("orderValue") && object.contains("estimatedExecutionFee")
            : object.value("orderId").toString() == orderId);
        const bool definitiveRejection = status >= 400 && status < 500 && status != 408 && status != 409 && status != 429;
        if (!httpSuccess || !validResponse) {
            const bool uncertain = !isPreflight && !definitiveRejection;
            const QString message = uncertain
                ? QStringLiteral("Outcome uncertain (HTTP %1). Order ID: %2. Check this order in Public before submitting again; it was not retried.").arg(status).arg(orderId)
                : QStringLiteral("%1 did not succeed (HTTP %2); no automatic retry.").arg(isPreflight ? "Preflight" : "Order").arg(status);
            if (uncertain) {
                auto result = m_tradeResults.at(resultIndex).toMap();
                result.insert("message", QStringLiteral("Submission outcome uncertain; checking the original order ID without resubmitting…"));
                m_tradeResults[resultIndex] = result;
                emit tradeResultsChanged();
                scheduleAfter(qMax<qint64>(m_policy.symbolPauseMs, m_cooldown.remainingTime()), this,
                    [this, accountId, orderId, resultIndex, generation]() { reconcileOrder(accountId, orderId, resultIndex, generation); });
                return;
            }
            finishTradeStep(resultIndex, false, message, generation);
            if (!uncertain) emit transactionFailed(message);
            return;
        }
        const QString orderStatus = object.value("status").toString().toUpper();
        if (!isPreflight && orderStatus == "REJECTED") {
            finishTradeStep(resultIndex, false, QStringLiteral("Order was rejected by Public."), generation);
            return;
        }
        // Do not echo arbitrary server strings or response bodies into logs/UI.
        const QString message = isPreflight ? QStringLiteral("Preflight succeeded.")
                                            : QStringLiteral("Order acknowledged by Public; this is not a fill confirmation.");
        finishTradeStep(resultIndex, true, message, generation);
        if (isPreflight) emit preflightPassed(message);
        else emit orderExecuted(message);
    });
}

// Documented read-only endpoint: public.com/api/docs/resources/order-placement/get-order.
// A 404 can be eventual visibility, never evidence that it is safe to resubmit.
void PublicApiWorker::reconcileOrder(const QString &accountId, const QString &orderId,
                                    int resultIndex, quint64 generation, int attempt)
{
    if (generation != m_tradeGeneration) return;
    auto request = setPublicBrokerageHeaders("userapigateway/trading/" + accountId + "/order/" + orderId);
    request.setTransferTimeout(m_policy.timeoutMs);
    auto *reply = manager->get(request);
    scheduleAfter(m_policy.timeoutMs, reply, [reply]() { if (!reply->isFinished()) reply->abort(); });
    QElapsedTimer elapsed;
    elapsed.start();
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        reply->deleteLater();
        if (generation != m_tradeGeneration) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto object = QJsonDocument::fromJson(responseBody(reply)).object();
        const QString orderStatus = object.value("status").toString();
        qInfo() << "Public request order-status symbol" << m_tradeResults.at(resultIndex).toMap().value("symbol").toString()
                << "status" << status << "latencyMs" << elapsed.elapsed() << "retry" << attempt
                << "correlation" << orderId << "requestId" << reply->request().attribute(QNetworkRequest::User).toString()
                << "networkError" << int(reply->error());
        if (status == 200 && reply->error() == QNetworkReply::NoError
            && object.value("orderId").toString() == orderId && knownOrderStatus(orderStatus)) {
            const bool rejected = orderStatus == "REJECTED";
            finishTradeStep(resultIndex, !rejected,
                QStringLiteral("Original order located. Status: %1. Order ID: %2. Review fills in Public before submitting again.").arg(orderStatus, orderId), generation);
            return;
        }
        const qint64 delay = retryDelay(m_policy, attempt, reply->rawHeader("Retry-After"));
        if (retryable(status))
            m_cooldown = QDeadlineTimer(qMax<qint64>(m_policy.gatewayCooldownMs, delay), Qt::PreciseTimer);
        if ((retryable(status) || status == 404 || status == 0) && attempt < m_policy.maxRetries) {
            scheduleAfter(qMax<qint64>(delay, m_cooldown.remainingTime()), this,
                [=, this]() { reconcileOrder(accountId, orderId, resultIndex, generation, attempt + 1); });
            return;
        }
        finishTradeStep(resultIndex, false,
            QStringLiteral("Outcome uncertain. Order ID: %1. Status could not be confirmed (HTTP %2). Check Public before submitting again; this order was never retried.").arg(orderId).arg(status),
            generation, QStringLiteral("uncertain"));
    });
}

void PublicApiWorker::fetchPortfolio(int index, int attempt, quint64 generation)
{
    if (generation != m_accountLoadGeneration) return;
    if (index >= m_accountList.size()) {
        m_loadingAccounts = false;
        // Discovery succeeded even when every portfolio failed: retain per-account
        // not-submitted rows, including in Auto mode, rather than hiding accounts.
        emit accountLoadFinished(true, {});
        return;
    }
    const QString accountId = m_accountList.at(index).accountId;
    QNetworkRequest request = setPublicBrokerageHeaders(
        "userapigateway/trading/" + accountId + "/portfolio/v2");
    request.setTransferTimeout(m_policy.timeoutMs);
    QElapsedTimer elapsed;
    elapsed.start();
    auto *reply = manager->get(request);
    scheduleAfter(m_policy.timeoutMs, reply, [reply]() { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, index, attempt, generation, elapsed]() {
        reply->deleteLater();
        if (generation != m_accountLoadGeneration) return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 delay = retryDelay(m_policy, attempt, reply->rawHeader("Retry-After"));
        qInfo() << "Public request" << "portfolio" << "accountIndex" << index
                << "status" << status << "latencyMs" << elapsed.elapsed()
                << "retry" << attempt << "requestId" << reply->request().attribute(QNetworkRequest::User).toString()
                << "networkError" << int(reply->error())
                << "retryDelayMs" << (retryable(status) && attempt < m_policy.maxRetries ? delay : 0);
        if (retryable(status) && attempt < m_policy.maxRetries) {
            scheduleAfter(qMax<qint64>(m_policy.portfolioPauseMs, delay), this,
                [this, index, attempt, generation]() { fetchPortfolio(index, attempt + 1, generation); });
            return;
        }
        auto &account = m_accountList[index];
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300)
            account.stockShareBalances = stockShareBalancesFromPortfolio(responseBody(reply), &account.holdingsLoaded);
        qInfo() << "Public portfolio outcome" << (account.holdingsLoaded ? "loaded" : "not_loaded")
                << "accountIndex" << index << "generation" << generation;
        scheduleAfter(qMax<qint64>(m_policy.portfolioPauseMs, retryable(status) ? delay : 0), this,
            [this, index, generation]() { fetchPortfolio(index + 1, 0, generation); });
    });
}

void PublicApiWorker::applyDiscoveredAccounts(const QList<AccountData> &accounts,
                                              quint64 accountLoadGeneration,
                                              const QString &token)
{
    if (accountLoadGeneration != m_accountLoadGeneration || token.isEmpty() || token != m_token) {
        return;
    }

    for (const AccountData &account : accounts) {
        addAccountToList(account.accountId, account.accountType, account.stockShareBalances, account.holdingsLoaded);
    }
}

QNetworkRequest PublicApiWorker::setPublicBrokerageHeaders(QString requestType)
{
    QNetworkRequest request(m_apiBaseUrl.resolved(QUrl(requestType)));
    // Local correlation only: never log untrusted server IDs or account URLs.
    request.setAttribute(QNetworkRequest::User, QUuid::createUuid().toString(QUuid::WithoutBraces));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(m_token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    return request;
}
