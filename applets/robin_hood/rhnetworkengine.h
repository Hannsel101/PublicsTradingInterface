#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QNetworkReply;

class RHNetworkEngine final : public QObject
{
public:
    enum class AuthKind { OAuthBearer, WebSecondaryBearer };
    explicit RHNetworkEngine(QObject *parent = nullptr, bool allowLoopbackForTests = false);
    QNetworkReply *sendAuthenticated(const QString &endpoint, const QByteArray &token,
                                    AuthKind kind, QString *error = nullptr);
    static bool safeBearer(const QByteArray &token);

    bool loadConfiguration(const QString &path, QString *error = nullptr);
    bool isConfigured() const;
    // A null reply means nothing was dispatched; callers own successful replies' cleanup.
    QNetworkReply *send(const QString &endpoint, const QVariantMap &values = {},
                        const QVariantMap &query = {}, const QVariantMap &body = {},
                        QString *error = nullptr);

private:
    QNetworkAccessManager m_network;
    QJsonObject m_endpoints;
    QJsonObject m_headerSets;
    bool m_configured = false;
    bool m_allowLoopbackForTests = false;
};
