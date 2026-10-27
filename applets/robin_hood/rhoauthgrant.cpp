#include "rhoauthgrant.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <cmath>

namespace {
constexpr qsizetype maxSecretBytes = 1024 * 1024;
bool safeToken(const QString &text)
{
    if (text.isEmpty() || text.size() > 16384) return false;
    for (const QChar c : text) {
        const ushort v = c.unicode();
        if (!(v >= 'a' && v <= 'z') && !(v >= 'A' && v <= 'Z') && !(v >= '0' && v <= '9')
            && v != '-' && v != '.' && v != '_' && v != '~' && v != '+' && v != '/' && v != '=') return false;
    }
    return true;
}
bool safeBinding(const QString &text)
{
    if (text.isEmpty() || text.size() > 512) return false;
    for (const QChar c : text) if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    return true;
}
bool invalid(QString *error)
{
    if (error) *error = QStringLiteral("OAuth grant is incomplete, invalid, or unsupported; reauthentication is required.");
    return false;
}
}

QByteArray RHOAuthGrant::accessToken() const { return response.value("access_token").toString().toLatin1(); }
QByteArray RHOAuthGrant::refreshToken() const { return m_refreshToken; }
bool RHOAuthGrant::primary() const
{
    return effectiveScope == "internal" && (!response.contains("scope") || response.value("scope") == "internal")
        && response.value("token_type").toString().compare("Bearer", Qt::CaseInsensitive) == 0
        && safeToken(response.value("access_token").toString());
}

bool RHOAuthGrant::parseResponse(const QJsonObject &response, const QString &clientId,
    const QString &deviceToken, QDateTime now, RHOAuthGrant *out, QString *error, const RHOAuthGrant *previous)
{
    if (error) error->clear();
    if (previous && (!previous->primary() || previous->clientId != clientId || previous->deviceToken != deviceToken
        || !safeToken(QString::fromLatin1(previous->refreshToken())))) return invalid(error);
    const QString scope = response.contains("scope") ? response.value("scope").toString()
        : (previous ? previous->effectiveScope : QString());
    const QByteArray refresh = response.contains("refresh_token") ? response.value("refresh_token").toString().toLatin1()
        : (previous ? previous->refreshToken() : QByteArray());
    const auto durationValue = response.value("expires_in");
    const double duration = durationValue.toDouble(-1);
    if (!out || !now.isValid() || !safeBinding(clientId) || !safeBinding(deviceToken)
        || !durationValue.isDouble() || !std::isfinite(duration) || duration <= 0
        || duration > 9007199254740991.0 || std::floor(duration) != duration
        || QJsonDocument(response).toJson(QJsonDocument::Compact).size() > maxSecretBytes / 2
        || !safeToken(response.value("access_token").toString())
        || !safeToken(QString::fromLatin1(refresh))
        || (response.contains("refresh_token") && !safeToken(response.value("refresh_token").toString()))
        || response.value("token_type").toString().compare("Bearer", Qt::CaseInsensitive) != 0
        || scope != "internal") return invalid(error);
    if (response.contains("mfa_required") && response.value("mfa_required") != QJsonValue(false)) return invalid(error);
    for (const auto &key : {"error", "error_description", "mfa_type", "challenge", "challenge_id", "verification_workflow"})
        if (response.contains(QLatin1String(key))) return invalid(error);
    if ((response.contains("client_id") && response.value("client_id") != clientId)
        || (response.contains("device_token") && response.value("device_token") != deviceToken)) return invalid(error);
    RHOAuthGrant grant;
    grant.response = response; grant.clientId = clientId; grant.deviceToken = deviceToken;
    grant.effectiveScope = scope; grant.obtainedAt = now.toUTC();
    if (previous) {
        grant.accountId = previous->accountId;
        grant.nativeEmail = previous->nativeEmail;
        grant.nativePassword = previous->nativePassword;
    }
    grant.expiresAt = grant.obtainedAt.addSecs(static_cast<qint64>(duration));
    if (!grant.expiresAt.isValid() || grant.expiresAt <= grant.obtainedAt) return invalid(error);
    grant.m_refreshToken = refresh;
    *out = grant;
    return true;
}

QString RHOAuthGrant::serialize() const
{
    if (!primary()) return {};
    const bool native = !nativeEmail.isEmpty() || !nativePassword.isEmpty();
    QJsonObject envelope{{"format", "RHOAuthGrant"}, {"version", native ? 2 : 1}, {"response", response},
        {"client_id", clientId}, {"device_token", deviceToken}, {"account_id", accountId},
        {"effective_scope", effectiveScope}, {"refresh_token", QString::fromLatin1(m_refreshToken)},
        {"obtained_at", obtainedAt.toUTC().toString(Qt::ISODateWithMs)},
        {"expires_at", expiresAt.toUTC().toString(Qt::ISODateWithMs)}};
    if (native) envelope["native_credentials"] = QJsonObject{{"email", nativeEmail}, {"password", nativePassword}};
    const auto secret = QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    RHOAuthGrant checked;
    return deserialize(secret, &checked) ? secret : QString();
}

bool RHOAuthGrant::deserialize(const QString &secret, RHOAuthGrant *out, QString *error)
{
    if (error) error->clear();
    if (!out || secret.size() > maxSecretBytes) return invalid(error);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(secret.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return invalid(error);
    const auto envelope = document.object();
    QStringList keys{"format", "version", "response", "client_id", "device_token", "account_id",
                           "effective_scope", "refresh_token", "obtained_at", "expires_at"};
    const bool native = envelope.value("version") == 2;
    if (native) keys.append("native_credentials");
    if (envelope.size() != keys.size()) return invalid(error);
    for (auto it = envelope.begin(); it != envelope.end(); ++it)
        if (!keys.contains(it.key())) return invalid(error);
    if (envelope.value("format") != "RHOAuthGrant" || (!native && envelope.value("version") != 1)
        || !envelope.value("response").isObject()) return invalid(error);
    if (!envelope.value("obtained_at").toString().endsWith('Z')
        || !envelope.value("expires_at").toString().endsWith('Z')) return invalid(error);
    const auto obtained = QDateTime::fromString(envelope.value("obtained_at").toString(), Qt::ISODateWithMs);
    // Reconstruct separately stored inheritance without changing the actual response.
    const auto original = envelope.value("response").toObject();
    auto validation = original;
    if (!validation.contains("scope")) validation["scope"] = envelope.value("effective_scope");
    if (!validation.contains("refresh_token")) validation["refresh_token"] = envelope.value("refresh_token");
    RHOAuthGrant grant;
    if (!parseResponse(validation, envelope.value("client_id").toString(),
        envelope.value("device_token").toString(), obtained, &grant, error)) return false;
    grant.response = original;
    const auto expires = QDateTime::fromString(envelope.value("expires_at").toString(), Qt::ISODateWithMs);
    if (expires != grant.expiresAt || envelope.value("effective_scope") != grant.effectiveScope
        || envelope.value("refresh_token").toString().toLatin1() != grant.refreshToken()
        || !envelope.value("account_id").isString()) return invalid(error);
    grant.accountId = envelope.value("account_id").toString();
    if (native) {
        const auto credentials = envelope.value("native_credentials").toObject();
        const auto email = credentials.value("email"); const auto password = credentials.value("password");
        if (credentials.size() != 2 || !credentials.contains("email") || !credentials.contains("password")
            || !email.isString() || !password.isString() || email.toString().trimmed().isEmpty()
            || email.toString().size() > 512 || password.toString().isEmpty() || password.toString().size() > 4096
            || grant.accountId.isEmpty()) return invalid(error);
        for (const QChar c : email.toString())
            if (c.isNull() || c.category() == QChar::Other_Control) return invalid(error);
        grant.nativeEmail = email.toString(); grant.nativePassword = password.toString();
    }
    *out = grant; return true;
}

bool RHOAuthGrant::extractAccessToken(const QString &secret, QByteArray *out, QString *error)
{
    if (error) error->clear();
    if (!out) return invalid(error);
    if (safeToken(secret)) { *out = secret.toLatin1(); return true; }
    RHOAuthGrant grant;
    if (!deserialize(secret, &grant, error)) return false;
    *out = grant.accessToken(); return true;
}
