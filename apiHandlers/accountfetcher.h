#ifndef ACCOUNTFETCHER_H
#define ACCOUNTFETCHER_H

#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class AccountFetcher : public QObject {
    Q_OBJECT
public:
    explicit AccountFetcher(const QString &accessToken, QObject *parent = nullptr);
    void fetchAccountBalance();

public slots:
    void processToken(QString newToken);

private slots:
    void onAccountsReceived(QNetworkReply *reply);
    void onPortfolioReceived(QNetworkReply *reply);

private:
    QNetworkAccessManager *manager;
    QString token;
    QString accountId;
    void getPortfolioData();
};


#endif // ACCOUNTFETCHER_H
