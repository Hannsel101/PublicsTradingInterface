#include "stocksearchcontroller.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrlQuery>

StockSearchController::StockSearchController(QObject *parent)
    : QObject(parent),
    m_netManager(new QNetworkAccessManager(this)),
    m_debounceTimer(new QTimer(this))
{
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(300); // 300 ms typing delay

    connect(m_debounceTimer, &QTimer::timeout, this, &StockSearchController::fetchStockSymbols);
    connect(m_netManager, &QNetworkAccessManager::finished, this, &StockSearchController::onNetworkReply);
}

void StockSearchController::updateSearch(const QString &text) {
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        m_debounceTimer->stop();
        m_suggestions.clear();
        emit suggestionsChanged();
        return;
    }

    m_pendingQuery = trimmed;
    m_debounceTimer->start(); // Resets timer on every keystroke
}

void StockSearchController::fetchStockSymbols() {
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    QUrl url("https://api.public.com/userapigateway/trading/instruments");
    QUrlQuery query;
    query.addQueryItem("q", m_pendingQuery);
    url.setQuery(query);

    QNetworkRequest request = createAuthorizedRequest(url);
    m_currentReply = m_netManager->get(request);
}

void StockSearchController::onNetworkReply(QNetworkReply *reply) {
    if (reply != m_currentReply) {
        reply->deleteLater();
        return; // Ignore stale or aborted requests
    }

    m_currentReply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) return;

    if(!dataAlreadyCached)
    {
        // Cache the data
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        // Ensure the directory exists
        QDir().mkpath(cacheDir);
        cacheFilePath = cacheDir + "/instruments_cache.json";
        QByteArray rawResponse = reply->readAll();

        QFile file(cacheFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning() << "Could not save response to cache file:" << file.errorString();
            return;
        }

        file.write(rawResponse);
        file.close();
        qDebug() << "Successfully cached instrument data locally to:" << cacheFilePath;
    }


    // Load Cached Data
    QDir().mkpath(cacheDir);
    cacheFilePath = cacheDir + "/instruments_cache.json";
    QFile file(cacheFilePath);
    qDebug() << cacheFilePath;
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "Failed to open cache file for reading:" << file.errorString();
        return;
    }

    qDebug() << "Opened readonly file";


    // Process the data
    QByteArray data = file.readAll();
    file.close();

    qDebug() << "read everything from file";

    QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
    QJsonArray instrumentsArray;

    qDebug() << "stored into jsonDoc";
    if (jsonDoc.isArray()) {
        qDebug() << "isArray";
        instrumentsArray = jsonDoc.array();
    }
    else if (jsonDoc.isObject())
    {
        qDebug() << "is data value";
        instrumentsArray = jsonDoc.object().value("instruments").toArray();
    }

    QStringList cleanStockTickers;

    for (const QJsonValue& value : instrumentsArray)
    {
        // layered as instruments{ instrument { symbol, type } }
        QJsonObject instrument = value.toObject();
        QJsonObject inst = instrument.value("instrument").toObject();


        QString symbol = inst.value("symbol").toString();
        QString assetClass = inst.value("assetClass").toString().toUpper();
        QString subType = inst.value("type").toString().toUpper();
        QString name = inst.value("name").toString().toUpper();

        if (assetClass == "EQUITY" || subType == "STOCK" || subType == "EQUITY") {
            bool isEtf = (subType == "ETF" || name.contains("ETF") || name.contains("EXCHANGE TRADED FUND"));
            bool isMutualFund = (subType == "MUTUAL_FUND" || subType == "FUND" || name.contains("MUTUAL FUND"));


            if (!isEtf && !isMutualFund) {
                cleanStockTickers.append(symbol);
            }
        }
    }

    qDebug() << "past the instruments array for loop";
    for(const QString &symbol: cleanStockTickers)
    {
        qDebug() << symbol;
    }

    m_suggestions.append("APPL");
    emit suggestionsChanged();
    qDebug() << "done with the function call";
}

void StockSearchController::storeAuthorizationToken(QString newToken)
{
    m_accessToken = newToken;

    if(newToken == "")
        setTokenActive(false);
    else
        setTokenActive(true);
}

QNetworkRequest StockSearchController::createAuthorizedRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,  QByteArray("application/json"));

    // m_access is the active session token you fetched previously
    QByteArray bearerHeader = "Bearer " + m_accessToken.toUtf8();
    request.setRawHeader(QByteArray("Authorization"), bearerHeader);
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));
    return request;
}


bool StockSearchController::tokenActive() const
{
    return m_tokenActive;
}

void StockSearchController::setTokenActive(bool newTokenActive)
{
    if (m_tokenActive == newTokenActive)
        return;
    m_tokenActive = newTokenActive;
    emit tokenActiveChanged();
}
