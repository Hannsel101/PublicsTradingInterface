#include "AccountFetcher.h"
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QUrl>

AccountFetcher::AccountFetcher(const QString &accessToken, QObject *parent)
    : QObject(parent), token(accessToken) {
    manager = new QNetworkAccessManager(this);
}

void AccountFetcher::fetchAccountBalance() {
    QNetworkRequest request(QUrl("https://api.public.com/userapigateway/trading/5OH95967/portfolio/v2"));

    // Qt 6 clean raw header formatting via QByteArray literals
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

    // Explicitly capturing context in Qt 6 style connections
    connect(manager, &QNetworkAccessManager::finished, this, &AccountFetcher::onAccountsReceived);
    manager->get(request);
}

void AccountFetcher::processToken(QString newToken)
{
    token = newToken;
    fetchAccountBalance();
}

void AccountFetcher::onAccountsReceived(QNetworkReply *reply) {
    reply->deleteLater();
    disconnect(manager, &QNetworkAccessManager::finished, this, &AccountFetcher::onAccountsReceived);


    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Account lookup failed:" << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    qDebug() << "Raw Response:  " << responseData;

    // UCRT safe JSON decoding via explicit raw data extraction
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "JSON Parse Error:" << parseError.errorString();
        return;
    }

    qDebug() << "onAccountsReceived4";

    QJsonObject obj = doc.object();
    //QJsonArray accounts = obj["accountId"].toArray();
    accountId = obj["accountId"].toString();

    // if (accounts.isEmpty()) {
    //     qWarning() << "No brokerage accounts found.";
    //     return;
    // }

    qDebug() << "onAccountsReceived5";

    // Explicitly grab the first valid trading account
    //accountId = accounts.first().toObject()["accountId"].toString();
    qDebug() << "Found Account ID under LLVM-MinGW:" << accountId;

    qDebug() << "onAccountsReceived6";

    getPortfolioData();
    qDebug() << "onAccountsReceived7";
}

void AccountFetcher::getPortfolioData() {
    QString urlStr = QString("https://api.public.com/userapigateway/trading/%1/portfolio/v2").arg(accountId);
    QNetworkRequest request(urlStr);

    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));
    request.setRawHeader(QByteArray("Authorization"), QString("Bearer %1").arg(token).toUtf8());
    request.setRawHeader(QByteArray("User-Agent"), QByteArray("public-dev-docs"));

    connect(manager, &QNetworkAccessManager::finished, this, &AccountFetcher::onPortfolioReceived);
    manager->get(request);
}

void AccountFetcher::onPortfolioReceived(QNetworkReply *reply) {
    reply->deleteLater();
    disconnect(manager, &QNetworkAccessManager::finished, this, &AccountFetcher::onPortfolioReceived);

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Portfolio fetch failed:" << reply->errorString();
        return;
    }

    // Read the raw network transmission buffer
    QByteArray rawData = reply->readAll();


    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(rawData, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "Sanitized JSON Parse Error:" << parseError.errorString();
        qWarning() << "Error offset location:" << parseError.offset;

        // Print the specific segment of raw string causing the breakdown for visual debugging
        int contextStart = qMax(0, parseError.offset - 30);
        qWarning() << "Faulty Segment Context:" << rawData.mid(contextStart, 60);
        return;
    }

    QJsonObject portfolioObj = doc.object();
    qDebug() << "Portfolio Data Successfully Parsed under LLVM-MinGW!";

    // Public.com balance fields are sent natively as numeric string keys to preserve precision
    // Reference: https://public.com/api/docs/resources/account-details/get-account-portfolio-v2

    if (portfolioObj.contains("buyingPower")) {
        QJsonObject buyingPower = portfolioObj["buyingPower"].toObject();
        QString availableCash = buyingPower["cashOnlyBuyingPower"].toString();
        qDebug() << "Available Cash Balance:" << availableCash;
    }
}
