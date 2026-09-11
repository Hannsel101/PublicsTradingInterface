#include "publicapiworker.h"
#include <QUuid>

namespace {
constexpr int tradeRequestTimeoutMs = 30000;
}

void PublicApiWorker::processToken(QString newToken)
{
    ++m_accountLoadGeneration;
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
    } else if (!accountType.trimmed().isEmpty()) {
        typeLabel = accountType.trimmed();
    } else {
        typeLabel = QStringLiteral("Account");
    }

    return QStringLiteral("%1: %2").arg(typeLabel, accountId);
}

void PublicApiWorker::beginTradeResults(const QString &symbol, const QString &side, bool isPreflight)
{
    setCurrentTransactionTitle(transactionTitle(symbol, side, isPreflight));
    m_tradeResults.clear();
    m_pendingTradeResults = static_cast<int>(m_accountList.size());

    for (const auto &accountData : m_accountList) {
        QVariantMap result;
        result.insert(QStringLiteral("accountId"), accountData.accountId);
        result.insert(QStringLiteral("accountType"), accountData.accountType);
        result.insert(QStringLiteral("accountLabel"), displayLabelForAccount(accountData.accountId, accountData.accountType));
        result.insert(QStringLiteral("status"), QStringLiteral("pending"));
        result.insert(QStringLiteral("success"), false);
        result.insert(QStringLiteral("message"), QStringLiteral("Waiting for Public.com response…"));
        m_tradeResults.append(result);
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

void PublicApiWorker::updateTradeResult(const QString &accountId, bool success, const QString &message)
{
    for (int i = 0; i < m_tradeResults.size(); ++i) {
        QVariantMap result = m_tradeResults.at(i).toMap();
        if (result.value(QStringLiteral("accountId")).toString() != accountId) {
            continue;
        }

        const QString previousStatus = result.value(QStringLiteral("status")).toString();
        if (previousStatus != QStringLiteral("pending")) {
            return;
        }

        result.insert(QStringLiteral("status"), success ? QStringLiteral("success") : QStringLiteral("failed"));
        result.insert(QStringLiteral("success"), success);
        result.insert(QStringLiteral("message"), message);
        m_tradeResults[i] = result;
        if (m_pendingTradeResults > 0) {
            --m_pendingTradeResults;
        }
        emit tradeResultsChanged();

        if (m_pendingTradeResults == 0) {
            setTradeResultsComplete(true);
        }
        return;
    }
}

void PublicApiWorker::addAccountToList(QString id, QString accountType)
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
    m_accountList.append(AccountData{id, accountType});
}

void PublicApiWorker::fetchAllSubaccountsConcurrently() {
    QString baseUrl = "https://public.com";
    QString token = m_token;
    const quint64 accountLoadGeneration = m_accountLoadGeneration;
    qDebug() << "baseURL: " << baseUrl;

    // 1. Prepare initial request for all accounts
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/account");
    QNetworkReply *reply = manager->get(request);

    // Connect to handling the initial account list
    connect(reply, &QNetworkReply::finished, this, [this, reply, baseUrl, token, accountLoadGeneration]() {
        reply->deleteLater();
        if (accountLoadGeneration != m_accountLoadGeneration || token != m_token) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Failed to fetch accounts:" << reply->errorString();
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
               accountType == "TRADITIONAL_IRA")
            {
                accountsToQuery.append(AccountData{accountObject.value("accountId").toString(), accountType});
            }
            else
            {
                qDebug() << accountObject.value("accountId").toString() << "of account type" <<
                    accountType << "which is not a supported account type";
            }
        }

        if (accountsToQuery.isEmpty()) return;

        QString baseUrl2 = "https://api.public.com";
        // 2. Dispatch concurrent queries
        executeConcurrentQueries(accountsToQuery, baseUrl2, token, accountLoadGeneration);
    });
}

void PublicApiWorker::executePreflight(const QString &symbol, const QString &side)
{
    const QString normalizedSymbol = symbol.trimmed().toUpper();
    beginTradeResults(normalizedSymbol, side, true);

    for(auto &accountData: m_accountList)
    {
        qDebug() << "Executing Preflight for" << accountData.accountId << ":" << normalizedSymbol << side;
        executeTradeOrPreflight(accountData.accountId,
                                normalizedSymbol,
                                side,
                                true);

        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void PublicApiWorker::executeTrade(const QString &symbol, const QString &side)
{
    const QString normalizedSymbol = symbol.trimmed().toUpper();
    beginTradeResults(normalizedSymbol, side, false);

    for(auto &accountData: m_accountList)
    {
        qDebug() << "Executing Trade for" << accountData.accountId << ":" << normalizedSymbol << side;
        executeTradeOrPreflight(accountData.accountId,
                                normalizedSymbol,
                                side,
                                false);

        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void PublicApiWorker::clearSubAccountsList()
{
    ++m_accountLoadGeneration;
    m_accountList.clear();
    m_token = "";
    dismissTradeResults();
}

void PublicApiWorker::executeTradeOrPreflight(const QString &accountId, const QString &symbol, const QString &side, bool isPreflight)
{
    QNetworkAccessManager *manager = new QNetworkAccessManager(this);

    // Construct target URL based on whether we are testing or executing
    QString endpoint = isPreflight ? "preflight/single-leg": "order";
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/" +
                                                        accountId +
                                                        "/" +
                                                        endpoint);

    // Construct Order Payload
    QJsonObject orderObj;
    orderObj["orderId"] = QUuid::createUuid().toString(QUuid::WithoutBraces); // Generate dynamic UUIDv4
    orderObj["orderSide"] = side.toUpper(); // "BUY" or "SELL"
    orderObj["orderType"] = "MARKET";
    orderObj["quantity"] = "1"; // Always trade in single stock amounts
    orderObj["validateOrder"] = "true"; // Validate the order against current account state

    QJsonObject instrumentObj;
    instrumentObj["symbol"] = symbol.toUpper().trimmed();
    instrumentObj["type"] = "EQUITY";
    orderObj["instrument"] = instrumentObj;

    QJsonObject expirationObj;
    expirationObj["timeInForce"] = "DAY";
    orderObj["expiration"] = expirationObj;

    QJsonDocument doc(orderObj);
    QByteArray payload = doc.toJson();

    QNetworkReply *reply = manager->post(request, payload);
    QTimer *timeoutTimer = new QTimer(reply);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(tradeRequestTimeoutMs);

    connect(timeoutTimer, &QTimer::timeout, this, [this, reply, accountId]() {
        if (!reply->isFinished()) {
            updateTradeResult(accountId, false, QStringLiteral("Timed out waiting for Public.com to return a transaction result."));
            emit transactionFailed(QStringLiteral("Transaction timed out for account %1").arg(accountId));
            reply->abort();
        }
    });
    timeoutTimer->start();

    // Connect asynchronous response
    connect(reply, &QNetworkReply::finished, this, [=]() {
        timeoutTimer->stop();
        reply->deleteLater();
        manager->deleteLater();

        const QByteArray responseData = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                return;
            }
            const QString message = apiErrorMessage(reply, responseData);
            updateTradeResult(accountId, false, message);
            emit transactionFailed(QStringLiteral("%1: %2").arg(accountId, message));
            qDebug() << "Transaction failed for" << accountId << message;
            return;
        }

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus >= 400) {
            const QString message = apiErrorMessage(reply, responseData);
            updateTradeResult(accountId, false, message);
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
            updateTradeResult(accountId, true, summary);
            emit preflightPassed(summary);
        } else {
            const QString details = QString("Live Order Placed! Status: %1")
                                        .arg(responseObj["status"].toString());
            updateTradeResult(accountId, true, details);
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
        QFuture<AccountData> future = QtConcurrent::run([this, account, baseUrl, token]() {
            // Set up a local event loop for this specific background thread query
            QEventLoop loop;
            QString requestString = baseUrl + "/userapigateway/trading/" + account.accountId + "/portfolio/v2";
            QNetworkRequest req(requestString);
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            req.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

            QNetworkReply *subReply = manager->get(req);
            connect(subReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec(); // Blocks only this background worker thread, not the GUI/Main thread
            AccountData result = account;
            //AccountData result{id};//, QJsonObject()};
            // if (subReply->error() == QNetworkReply::NoError) {
            //     result.data = QJsonDocument::fromJson(subReply->readAll()).object();
            // } else {
            //     qDebug() << "Error fetching subaccount" << id << ":" << subReply->errorString();
            // }
            subReply->deleteLater();
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

            applyDiscoveredAccounts(discoveredAccounts, accountLoadGeneration, token);

            for(auto &accountData: m_accountList)
            {
                qDebug() << "Account ID: " << accountData.accountId;
                //m_accountList.append(AccountData{accountData.accountId});
                // for (const QString &key : accountData.data.keys()) {
                //     QJsonValue val = accountData.data[key];
                //     qDebug() << key << ":" << val.toVariant(); // .toVariant() converts to a generic QVariant for easy printing
                // }

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
        addAccountToList(account.accountId, account.accountType);
    }
}

QNetworkRequest PublicApiWorker::setPublicBrokerageHeaders(QString requestType)
{
    QNetworkRequest request(QUrl("https://api.public.com/" + requestType));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(m_token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    return request;
}
