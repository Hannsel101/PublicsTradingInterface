#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

// Secret-bearing value: C++ only. serialize() is a keychain payload, never metadata.
// Output pointers are borrowed for the call and remain untouched on failure.
struct RHOAuthGrant {
    QJsonObject response;
    QString clientId, deviceToken, accountId, effectiveScope;
    // C++ keychain envelope only, never provider response fields.
    QString nativeEmail, nativePassword;
    QDateTime obtainedAt, expiresAt;
    QByteArray accessToken() const;
    QByteArray refreshToken() const;
    bool primary() const;
    QString serialize() const;
    static bool parseResponse(const QJsonObject &response, const QString &clientId,
        const QString &deviceToken, QDateTime now, RHOAuthGrant *out,
        QString *error = nullptr, const RHOAuthGrant *previous = nullptr);
    static bool deserialize(const QString &secret, RHOAuthGrant *out, QString *error = nullptr);
    static bool extractAccessToken(const QString &secret, QByteArray *out, QString *error = nullptr);
private:
    QByteArray m_refreshToken;
};
