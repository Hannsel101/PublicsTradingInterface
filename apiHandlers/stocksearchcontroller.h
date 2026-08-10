#ifndef STOCKSEARCHCONTROLLER_H
#define STOCKSEARCHCONTROLLER_H

#pragma once

#include <QObject>
#include <QStringList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>

class StockSearchController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList suggestions READ suggestions NOTIFY suggestionsChanged)
    Q_PROPERTY(bool tokenActive READ tokenActive WRITE setTokenActive NOTIFY tokenActiveChanged FINAL)

public:
    explicit StockSearchController(QObject *parent = nullptr);

    Q_INVOKABLE void updateSearch(const QString &text);
    QStringList suggestions() const { return m_suggestions; }

    bool tokenActive() const;
    void setTokenActive(bool newTokenActive);

signals:
    void suggestionsChanged();
    void tokenActiveChanged();

public slots:
    void storeAuthorizationToken(QString newToken);

private slots:
    void fetchStockSymbols();
    void onNetworkReply(QNetworkReply *reply);

private:
    QNetworkAccessManager *m_netManager;
    QTimer *m_debounceTimer;
    QNetworkReply *m_currentReply = nullptr;
    QString m_accessToken;
    bool m_tokenActive = false;
    QString m_pendingQuery;
    QStringList m_suggestions;
    QString cacheFilePath;
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    bool dataAlreadyCached = true;

    QNetworkRequest createAuthorizedRequest(const QUrl &url);
};


#endif // STOCKSEARCHCONTROLLER_H
