#include "publicapiworker.h"
#include <QUuid>
#include <array>
#include <charconv>

namespace {
constexpr int tradeRequestTimeoutMs = 30000;
constexpr int betweenSymbolsMs = 100;
constexpr int betweenAccountsMs = 200;

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
    ++m_accountLoadGeneration;
    ++m_tradeGeneration;
    m_accountList.clear();
    m_token = newToken;
    dismissTradeResults();
    if (m_token.isEmpty()) {
        return;
    }
    fetchAllSubaccountsConcurrently();
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

QString PublicApiWorker::apiErrorMessage(const QNetworkReply *reply, const QByteArray &responseData) const
{
    if (!responseData.trimmed().isEmpty()) {
        const QJsonDocument errorDoc = QJsonDocument::fromJson(responseData);
        if (errorDoc.isObject()) {
            const QJsonObject errorObj = errorDoc.object();
            const QString message = errorObj.value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) {
                return message;
            }
            const QString error = errorObj.value(QStringLiteral("error")).toString();
            if (!error.isEmpty()) {
                return error;
            }
            const QString detail = errorObj.value(QStringLiteral("detail")).toString();
            if (!detail.isEmpty()) {
                return detail;
            }
        }
        return QString::fromUtf8(responseData).left(240);
    }

    return reply ? reply->errorString() : QStringLiteral("Unknown transaction error");
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

void PublicApiWorker::updateTradeResult(int resultIndex, bool success, const QString &message)
{
    QVariantMap result = m_tradeResults.at(resultIndex).toMap();
    if (result.value(QStringLiteral("status")) != QStringLiteral("pending")) return;
    result.insert(QStringLiteral("status"), success ? QStringLiteral("success") : QStringLiteral("failed"));
    result.insert(QStringLiteral("success"), success);
    result.insert(QStringLiteral("message"), message);
    m_tradeResults[resultIndex] = result;
    --m_pendingTradeResults;
    emit tradeResultsChanged();
    if (m_pendingTradeResults == 0) setTradeResultsComplete(true);
}

void PublicApiWorker::addAccountToList(QString id,
                                       QString accountType,
                                       const QHash<QString, float> &stockShareBalances)
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
    m_accountList.append(AccountData{id, accountType, stockShareBalances});
}

QHash<QString, float> PublicApiWorker::stockShareBalancesFromPortfolio(const QByteArray &responseData)
{
    QHash<QString, float> stockShareBalances;
    const QJsonDocument portfolioDocument = QJsonDocument::fromJson(responseData);
    if (!portfolioDocument.isObject()) {
        return stockShareBalances;
    }

    const QJsonArray positions = portfolioDocument.object().value(QStringLiteral("positions")).toArray();
    for (const QJsonValue &positionValue : positions) {
        const QJsonObject position = positionValue.toObject();
        const QJsonObject instrument = position.value(QStringLiteral("instrument")).toObject();
        if (instrument.value(QStringLiteral("type")).toString().compare(
                QStringLiteral("EQUITY"), Qt::CaseInsensitive) != 0) {
            continue;
        }

        const QString symbol = instrument.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
        bool quantityIsValid = false;
        const float quantity = position.value(QStringLiteral("quantity")).toVariant().toFloat(&quantityIsValid);
        if (!symbol.isEmpty() && quantityIsValid) {
            stockShareBalances.insert(symbol, quantity);
        }
    }

    return stockShareBalances;
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

void PublicApiWorker::fetchAllSubaccountsConcurrently() {
    QString token = m_token;
    const quint64 accountLoadGeneration = m_accountLoadGeneration;

    // 1. Prepare initial request for all accounts
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/account");
    request.setTransferTimeout(30000);
    QNetworkReply *reply = manager->get(request);

    // Connect to handling the initial account list
    connect(reply, &QNetworkReply::finished, this, [this, reply, token, accountLoadGeneration]() {
        reply->deleteLater();
        if (accountLoadGeneration != m_accountLoadGeneration || token != m_token) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Failed to fetch accounts:" << reply->errorString();
            emit accountLoadFinished(false, QStringLiteral("Could not load accounts: %1").arg(reply->errorString()));
            return;
        }

        QByteArray responseData = reply->readAll();

        // Parse initial list of account IDs
        QJsonDocument doc = QJsonDocument::fromJson(responseData);
        QJsonArray accounts = doc.object().value("accounts").toArray();

        QList<AccountData> accountsToQuery;
        for (const QJsonValue &val : accounts)
        {
            const QJsonObject accountObject = val.toObject();
            const QString accountType = accountObject.value("accountType").toString();
            if(accountType == "BROKERAGE" ||
               accountType == "ROTH_IRA"  ||
               accountType == "TRADITIONAL_IRA" ||
               accountType == "ENTITY")
            {
                accountsToQuery.append(AccountData{accountObject.value("accountId").toString(), accountType});
            }
            else
            {
                qDebug() << accountObject.value("accountId").toString() << "of account type" <<
                    accountType << "which is not a supported account type";
            }
        }

        if (accountsToQuery.isEmpty()) {
            emit accountLoadFinished(false, QStringLiteral("No eligible brokerage accounts were found for this key."));
            return;
        }

        // 2. Dispatch concurrent queries
        executeConcurrentQueries(accountsToQuery, m_apiBaseUrl.toString(), token, accountLoadGeneration);
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
    if (tradeResultsBusy()) return;
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
    const int resultIndex = m_nextTradeIndex++;
    const AccountData &account = m_tradeAccounts.at(resultIndex / m_tradeSymbols.size());
    const QString &symbol = m_tradeSymbols.at(resultIndex % m_tradeSymbols.size());
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

void PublicApiWorker::finishTradeStep(int resultIndex, bool success, const QString &message, quint64 generation)
{
    if (generation != m_tradeGeneration) return;
    updateTradeResult(resultIndex, success, message);
    if (generation != m_tradeGeneration || m_nextTradeIndex >= m_tradeResults.size()) return;
    const int pauseMs = m_nextTradeIndex % m_tradeSymbols.size() == 0
                            ? betweenAccountsMs : betweenSymbolsMs;
    QTimer::singleShot(pauseMs, Qt::PreciseTimer, this, [this, generation]() {
        dispatchNextTrade(generation);
    });
}

void PublicApiWorker::clearSubAccountsList()
{
    ++m_accountLoadGeneration;
    ++m_tradeGeneration;
    m_accountList.clear();
    m_token = "";
    dismissTradeResults();
}

void PublicApiWorker::executeTradeOrPreflight(const QString &accountId,
                                              const QString &symbol,
                                              const QString &side,
                                              float quantity,
                                              bool isPreflight,
                                              int resultIndex,
                                              quint64 generation)
{
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);

    // Construct target URL based on whether we are testing or executing
    QString endpoint = isPreflight ? "preflight/single-leg": "order";
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/" +
                                                        accountId +
                                                        "/" +
                                                        endpoint);

    const QJsonObject orderObject = orderPayload(symbol, side, quantity);
    QJsonDocument doc(orderObject);
    QByteArray payload = doc.toJson();

    QNetworkReply *reply = manager->post(request, payload);
    QTimer *timeoutTimer = new QTimer(reply);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(tradeRequestTimeoutMs);

    connect(timeoutTimer, &QTimer::timeout, this, [reply]() {
        if (!reply->isFinished()) {
            reply->setProperty("tradeTimedOut", true);
            reply->abort();
        }
    });
    timeoutTimer->start();

    // Connect asynchronous response
    connect(reply, &QNetworkReply::finished, this, [=, this]() {
        timeoutTimer->stop();
        reply->deleteLater();
        manager->deleteLater();

        if (generation != m_tradeGeneration) return;
        const QByteArray responseData = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = reply->property("tradeTimedOut").toBool()
                                        ? QStringLiteral("Timed out waiting for Public.com to return a transaction result.")
                                        : apiErrorMessage(reply, responseData);
            finishTradeStep(resultIndex, false, message, generation);
            emit transactionFailed(QStringLiteral("%1: %2").arg(accountId, message));
            qDebug() << "Transaction failed for" << accountId << message;
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus >= 400) {
            const QString message = apiErrorMessage(reply, responseData);
            finishTradeStep(resultIndex, false, message, generation);
            emit transactionFailed(QStringLiteral("%1: %2").arg(accountId, message));
            qDebug() << "Transaction failed for" << accountId << message;
            return;
        }

        QJsonDocument responseDoc = QJsonDocument::fromJson(responseData);
        QJsonObject responseObj = responseDoc.object();

        if (isPreflight) {
            // Preflight passes back key impact telemetry (fees, margin requirements)
            QString summary = QString("Preflight Success! Est Value: $%1 | Est Fees: $%2")
                                  .arg(responseObj["orderValue"].toString())
                                  .arg(responseObj["estimatedExecutionFee"].toString());
            qDebug() << summary;
            finishTradeStep(resultIndex, true, summary, generation);
            emit preflightPassed(summary);
        } else {
            const QString details = QString("Live Order Placed! Status: %1")
                                        .arg(responseObj["status"].toString());
            finishTradeStep(resultIndex, true, details, generation);
            emit orderExecuted(details);
        }
    });
}

void PublicApiWorker::executeConcurrentQueries(const QList<AccountData> &accounts,
                                               const QString &baseUrl,
                                               const QString &token,
                                               quint64 accountLoadGeneration) {
    // Create an array of futures—one for each account network request
    QList<QFuture<AccountData>> futures;

    for (const AccountData &account : accounts) {
        // QtConcurrent::run handles the parallel wrapper safely
        QFuture<AccountData> future = QtConcurrent::run([account, baseUrl, token]() {
            // Set up a local event loop for this specific background thread query
            QEventLoop loop;
            QString requestString = baseUrl + "userapigateway/trading/" + account.accountId + "/portfolio/v2";
            QNetworkRequest req(requestString);
            req.setTransferTimeout(30000);
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            req.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

            QNetworkAccessManager requestManager;
            QNetworkReply *subReply = requestManager.get(req);
            connect(subReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec(); // Blocks only this background worker thread, not the GUI/Main thread
            AccountData result = account;
            if (subReply->error() == QNetworkReply::NoError) {
                result.stockShareBalances = stockShareBalancesFromPortfolio(subReply->readAll());
            } else {
                qDebug() << "Error fetching portfolio for account" << account.accountId
                         << ":" << subReply->errorString();
            }
            return result;
        });
        futures.append(future);
    }

    // 3. Monitor all concurrent tasks and wait for them to finish
    // QtFuture::whenAll maps nicely to JavaScript's Promise.all()
    QtFuture::whenAll(futures.begin(), futures.end())
        .then(this, [this, accountLoadGeneration, token](QList<QFuture<AccountData>> completedFutures) {
            qDebug() << "--- All concurrent subaccount queries finished! ---";
            QList<AccountData> discoveredAccounts;
            discoveredAccounts.reserve(completedFutures.size());
            for (auto &future : completedFutures) {
                discoveredAccounts.append(future.result());
            }

            if (accountLoadGeneration != m_accountLoadGeneration || token != m_token) return;
            applyDiscoveredAccounts(discoveredAccounts, accountLoadGeneration, token);
            emit accountLoadFinished(true, {});

            for(auto &accountData: m_accountList)
            {
                qDebug() << "Account ID: " << accountData.accountId;

            }
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
        addAccountToList(account.accountId, account.accountType, account.stockShareBalances);
    }
}

QNetworkRequest PublicApiWorker::setPublicBrokerageHeaders(QString requestType)
{
    QNetworkRequest request(m_apiBaseUrl.resolved(QUrl(requestType)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(m_token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    return request;
}
