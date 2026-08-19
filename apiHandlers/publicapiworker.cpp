#include "publicapiworker.h"

void PublicApiWorker::processToken(QString newToken)
{
    m_token = newToken;
    fetchAllSubaccountsConcurrently();
}

void PublicApiWorker::addAccountToList(QString id)
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
    m_accountList.append(AccountData{id});
}

void PublicApiWorker::fetchAllSubaccountsConcurrently() {
    QString baseUrl = "https://public.com";
    QString token = m_token;
    qDebug() << "token: " << token;
    qDebug() << "baseURL: " << baseUrl;

    // 1. Prepare initial request for all accounts
    QNetworkRequest request = setPublicBrokerageHeaders("userapigateway/trading/account");
    QNetworkReply *reply = manager->get(request);

    // Connect to handling the initial account list
    connect(reply, &QNetworkReply::finished, this, [this, reply, baseUrl, token]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qDebug() << "Failed to fetch accounts:" << reply->errorString();
            return;
        }

        QByteArray responseData = reply->readAll();

        // Parse initial list of account IDs
        QJsonDocument doc = QJsonDocument::fromJson(responseData);
        QJsonArray accounts = doc.object().value("accounts").toArray();

        QStringList accountIds;
        for (const QJsonValue &val : accounts) {
            accountIds.append(val.toObject().value("accountId").toString());
        }

        if (accountIds.isEmpty()) return;

        QString baseUrl2 = "https://api.public.com";
        // 2. Dispatch concurrent queries
        executeConcurrentQueries(accountIds, baseUrl2, token);
    });
}

void PublicApiWorker::executePreflight(const QString &symbol, const QString &side)
{
    // Execute a preflight for all accounts in the account list
    for(auto &accountData: m_accountList)
    {
        qDebug() << "Executing Preflight for" << accountData.accountId << ":" << symbol << side;
        executeTradeOrPreflight(accountData.accountId,
                                symbol,
                                side,
                                true);

        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void PublicApiWorker::executeTrade(const QString &symbol, const QString &side)
{
    // Execute a trade for all accounts in the account list
    for(auto &accountData: m_accountList)
    {
        qDebug() << "Executing Trade for" << accountData.accountId << ":" << symbol << side;
        executeTradeOrPreflight(accountData.accountId,
                                symbol,
                                side,
                                false);

        QEventLoop loop;
        QTimer::singleShot(200, &loop, &QEventLoop::quit);
        loop.exec();
    }
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
    instrumentObj["symbol"] = symbol.toUpper();
    instrumentObj["type"] = "EQUITY";
    orderObj["instrument"] = instrumentObj;

    QJsonObject expirationObj;
    expirationObj["timeInForce"] = "DAY";
    orderObj["expiration"] = expirationObj;

    QJsonDocument doc(orderObj);
    QByteArray payload = doc.toJson();

    QNetworkReply *reply = manager->post(request, payload);

    // Connect asynchronous response
    connect(reply, &QNetworkReply::finished, this, [=]() {
        reply->deleteLater();
        manager->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit transactionFailed(QString("Network Error: %1").arg(reply->errorString()));
            qDebug() << "Transaction failed!\n" << QString("Network Error: %1").arg(reply->errorString());
            return;
        }

        QJsonDocument responseDoc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject responseObj = responseDoc.object();

        if (isPreflight) {
            // Preflight passes back key impact telemetry (fees, margin requirements)
            QString summary = QString("Preflight Success! Est Value: $%1 | Est Fees: $%2")
                                  .arg(responseObj["orderValue"].toString())
                                  .arg(responseObj["estimatedExecutionFee"].toString());
            qDebug() << summary;
            emit preflightPassed(summary);
        } else {
            emit orderExecuted(QString("Live Order Placed! Status: %1")
                                   .arg(responseObj["status"].toString()));
        }
    });
}

void PublicApiWorker::executeConcurrentQueries(const QStringList &accountIds, const QString &baseUrl, const QString &token) {
    // Create an array of futures—one for each account network request
    QList<QFuture<QString>> futures;

    for (const QString &id : accountIds) {
        // QtConcurrent::run handles the parallel wrapper safely
        QFuture<QString> future = QtConcurrent::run([this, id, baseUrl, token]() {
            // Set up a local event loop for this specific background thread query
            QEventLoop loop;
            QString requestString = baseUrl + "/userapigateway/trading/" + id + "/portfolio/v2";
            QNetworkRequest req(requestString);
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            req.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

            QNetworkReply *subReply = manager->get(req);
            connect(subReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec(); // Blocks only this background worker thread, not the GUI/Main thread
            QString result = id;
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
        .then(this, [this](QList<QFuture<QString>> completedFutures) {
            qDebug() << "--- All concurrent subaccount queries finished! ---";
            for (auto &future : completedFutures) {
                addAccountToList(future.result());
            }

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

QNetworkRequest PublicApiWorker::setPublicBrokerageHeaders(QString requestType)
{
    QNetworkRequest request(QUrl("https://api.public.com/" + requestType));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(m_token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    return request;
}
