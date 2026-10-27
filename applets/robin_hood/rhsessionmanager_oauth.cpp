#include "rhsessionmanager.h"
#include "rhoauthgrant.h"
#include "rhnetworkengine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

void RHSessionManager::checkSessionRenewal()
{
    if (m_busy || m_tradeLocked || m_sessionCommit || !m_loaded) return;
    const auto *session = find(m_activeIndex);
    if (!session || !session->canRefresh || session->tokenType != QStringLiteral("Bearer")
        || m_unreconciledSessions.contains(session->index)
        || session->expiresAt > QDateTime::currentDateTimeUtc().addSecs(120)) return;
    initializeSession(session->index);
}

bool RHSessionManager::acceptOAuthGrant(const QJsonObject &response, const QString &clientId,
                                      const QString &deviceToken)
{
    if (m_busy || m_tradeLocked || m_sessionCommit || !m_loaded || !m_network.isConfigured())
        return false;
    RHOAuthGrant grant;
    if (!RHOAuthGrant::parseResponse(response, clientId, deviceToken,
                                   QDateTime::currentDateTimeUtc(), &grant) || !grant.primary()) {
        setError(QStringLiteral("A complete primary OAuth grant is required; sign in again."));
        return false;
    }
    const QPointer<RHSessionManager> guard(this);
    const quint64 generation = ++m_authGeneration;
    m_oauthPending = true;
    m_readingSession = true;
    setBusy(true);
    if (!guard || generation != m_authGeneration) return true;
    setError({});
    if (!guard || generation != m_authGeneration) return true;
    validateOAuthGrant(std::move(grant), generation, false);
    return true;
}

void RHSessionManager::renewOAuthGrant(int sessionIndex, RHOAuthGrant grant, const QString &secretDigest)
{
    const Session *selected = find(sessionIndex);
    if (!selected || m_tradeLocked || m_sessionCommit) return;
    const QPointer<RHSessionManager> guard(this);
    const quint64 readGeneration = m_readGeneration;
    if (selected->renewalPending || (selected->credentialRevision == 0
        && selected->renewalAttemptedAt.isValid() && grant.obtainedAt <= selected->renewalAttemptedAt)) {
        releaseCredentialReservation();
        setError(QStringLiteral("A previous OAuth renewal did not finish securely. Sign in again; the old refresh token will not be retried."));
        if (guard && readGeneration == m_readGeneration) setBusy(false);
        return;
    }
    // Durable fail-closed marker precedes dispatch: cancellation, a crash, or a
    // secure-store failure must never replay a possibly rotated predecessor.
    QString failure;
    if (!credentialOwnershipCurrent(sessionIndex, &failure)) { failLogin(failure); return; }
    const quint64 revision = allocateCredentialRevision(&failure);
    if (!revision) { failLogin(failure); return; }
    QList<Session> attempted = m_sessions;
    for (auto &session : attempted) {
        if (session.index != sessionIndex) continue;
        session.renewalAttemptedAt = QDateTime::currentDateTimeUtc();
        session.credentialRevision = revision;
        session.credentialDigest = secretDigest;
        session.renewalPending = true;
    }
    m_sessionCommit = true;
    const bool persisted = persist(attempted, &failure);
    if (persisted) m_sessions = std::move(attempted);
    m_sessionCommit = false;
    if (!persisted) {
        setError(QStringLiteral("Could not prepare safe OAuth renewal. No renewal was sent; retry after fixing session metadata."));
        if (guard && readGeneration == m_readGeneration) setBusy(false);
        return;
    }
    selected = find(sessionIndex);
    const quint64 generation = ++m_authGeneration;
    m_oauthPending = true;
    m_browserTarget = selected->accountId.isEmpty() ? selected->accountName : selected->accountId;
    QString error;
    auto *reply = m_network.refreshOAuth(grant, &error);
    if (!reply) {
        failLogin(QStringLiteral("OAuth renewal is unavailable; sign in again."));
        return;
    }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, grant = std::move(grant)] {
        if (generation != m_authGeneration) { reply->deleteLater(); return; }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        auto bytes = reply->readAll();
        const auto document = QJsonDocument::fromJson(bytes);
        SecureCredentialLocker::wipe(bytes);
        reply->deleteLater();
        RHOAuthGrant renewed;
        if (!ok || !document.isObject()
            || !RHOAuthGrant::parseResponse(document.object(), grant.clientId, grant.deviceToken,
                    QDateTime::currentDateTimeUtc(), &renewed, nullptr, &grant) || !renewed.primary()) {
            failLogin(QStringLiteral("OAuth renewal failed; sign in again. The saved profile was not activated."));
            return;
        }
        renewed.accountId = grant.accountId;
        validateOAuthGrant(std::move(renewed), generation, true);
    });
}

void RHSessionManager::validateOAuthGrant(RHOAuthGrant grant, quint64 generation, bool activate)
{
    const QByteArray token = grant.accessToken();
    readBrowserEndpoint(QStringLiteral("UserProfile"), token, QStringLiteral("Bearer"), generation,
        [this, grant = std::move(grant), token, generation, activate](const QJsonObject &profile) mutable {
        const QString id = profile.value("id").toString().trimmed();
        const QString email = profile.value("email").toString().trimmed();
        const QString username = profile.value("username").toString().trimmed();
        const bool native = m_nativeLoginPending || !grant.nativeEmail.isEmpty();
        const QString label = native ? email : (email.isEmpty() ? username : email);
        if (id.isEmpty() || label.isEmpty() || (!grant.accountId.isEmpty() && grant.accountId != id)) {
            failLogin(QStringLiteral("OAuth identity does not match the saved profile; sign in again."));
            return;
        }
        if (!m_browserTarget.isEmpty()) {
            const Session *selected = nullptr;
            for (const auto &session : m_sessions)
                if ((session.accountId.isEmpty() ? session.accountName : session.accountId) == m_browserTarget)
                    selected = &session;
            if (!selected || (selected->accountId.isEmpty()
                    ? selected->accountName.compare(email, Qt::CaseInsensitive) != 0
                        && selected->accountName.compare(username, Qt::CaseInsensitive) != 0
                    : selected->accountId != id)) {
                failLogin(QStringLiteral("OAuth identity does not match the selected saved profile; sign in again."));
                return;
            }
        }
        grant.accountId = id;
        readBrowserEndpoint(QStringLiteral("Accounts"), token, QStringLiteral("Bearer"), generation,
            [this, grant = std::move(grant), token, generation, activate, label, id, native, email](const QJsonObject &accounts) mutable {
            const auto results = accounts.value("results");
            const auto next = accounts.value("next");
            if (!results.isArray() || results.toArray().size() > 100
                || (!next.isUndefined() && !next.isNull() && next != QJsonValue(QString()))
                || grant.expiresAt <= QDateTime::currentDateTimeUtc()) {
                failLogin(QStringLiteral("OAuth accounts or expiry could not be validated; sign in again."));
                return;
            }
            QSet<QString> identities;
            for (const auto &value : results.toArray()) {
                const auto row = value.toObject();
                const QString number = row.value("account_number").toString();
                const QString identity = number.isEmpty() ? row.value("id").toString() : number;
                if (!value.isObject() || identity.trimmed().isEmpty() || identities.contains(identity)) {
                    failLogin(QStringLiteral("OAuth account identity is unavailable or ambiguous; sign in again."));
                    return;
                }
                identities.insert(identity);
            }
            if (m_nativeLoginPending) {
                QString suppliedEmail, password;
                const bool staged = m_nativeCredentials.read(suppliedEmail, password);
                if (!staged || email.isEmpty() || suppliedEmail.compare(email, Qt::CaseInsensitive) != 0) {
                    SecureCredentialLocker::wipe(suppliedEmail); SecureCredentialLocker::wipe(password);
                    failLogin(QStringLiteral("Verified email does not match the supplied account.")); return;
                }
                grant.nativeEmail = email; grant.nativePassword = password;
                SecureCredentialLocker::wipe(suppliedEmail); SecureCredentialLocker::wipe(password);
            } else if (native) {
                grant.nativeEmail = email;
            }
            commitBrowserSession(label, id, token, QStringLiteral("Bearer"), grant.expiresAt,
                                 generation, grant.serialize(), true, activate);
            SecureCredentialLocker::wipe(grant.nativePassword);
        });
    });
}
