#include "rhsessionmanager.h"
#include "rhnetworkengine.h"
#include "rhbrowserlogin.h"
#include "rhnativeauthclient.h"
#include "rhcredentialstore.h"
#include "rhoauthgrant.h"
#include "securecredentiallocker.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <utility>


void RHSessionManager::setBrowserLoginPending(bool pending)
{
    if (m_browserLoginPending == pending)
        return;
    m_browserLoginPending = pending;
    emit browserLoginPendingChanged();
}

void RHSessionManager::setBrowserLogin(RHBrowserLogin *browser)
{
    if (m_browser == browser || (m_busy && !m_browserLoginPending))
        return;
    cancelSignIn();
    if (m_browser)
        disconnect(m_browser, nullptr, this, nullptr);
    m_browser = browser;
    if (!browser)
        return;
    connect(browser, &RHBrowserLogin::activeChanged, this, [this] {
        if (m_browserLoginPending && m_browser && !m_browser->active())
            failLogin(QStringLiteral("Browser sign-in ended before verification completed."));
    });
    connect(browser, &RHBrowserLogin::errorChanged, this, [this] {
        if (m_browserLoginPending && m_browser && !m_browser->error().isEmpty())
            failLogin(QStringLiteral("Browser sign-in failed. Please retry."));
    });
    connect(browser, &QObject::destroyed, this, [this] {
        if (m_browserLoginPending)
            failLogin(QStringLiteral("Browser sign-in is unavailable."));
    });
}

bool RHSessionManager::beginBrowserLogin(const QString &accountId)
{
    if (m_sessionCommit) return false;
    if (m_busy || !m_loaded || !m_network.isConfigured()) {
        setError(QStringLiteral("Cannot sign in while busy or without network configuration."));
        return false;
    }
    if (!m_browser || !m_browser->available()) {
        setError(QStringLiteral("Secure browser sign-in is unavailable. Desktop requires QtWebEngineQuick. Android QtWebView sign-in is disabled until native pre-navigation enforcement is implemented."));
        return false;
    }
    if (!accountId.isEmpty()) {
        bool found = false;
        for (const Session &session : m_sessions)
            found |= (session.accountId.isEmpty() ? session.accountName : session.accountId) == accountId;
        if (!found) {
            setError(QStringLiteral("The selected saved account is unavailable."));
            return false;
        }
    }
    clearActiveSession();
    ++m_authGeneration;
    m_browserTarget = accountId;
    m_browserVerifying = false;
    setError({});
    setBusy(true);
    setReauthenticating(!accountId.isEmpty());
    setBrowserLoginPending(true);
    if (!m_browser->start()) {
        failLogin(QStringLiteral("Could not open browser sign-in. Check the installed Qt browser module."));
        return false;
    }
    return m_browserLoginPending;
}

void RHSessionManager::readBrowserEndpoint(const QString &endpoint, const QByteArray &token,
                                          const QString &tokenType, quint64 generation,
                                          std::function<void(QJsonObject)> done)
{
    if (generation != m_authGeneration) return;
    QString problem;
    const auto kind = tokenType == QStringLiteral("Bearer")
        ? RHNetworkEngine::AuthKind::OAuthBearer : RHNetworkEngine::AuthKind::WebSecondaryBearer;
    QNetworkReply *reply = m_network.sendAuthenticated(endpoint, token, kind, &problem);
    if (!reply) {
        failLogin(QStringLiteral("Browser token could not be verified using the configured read-only endpoint."));
        return;
    }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation, tokenType, done = std::move(done)] {
        if (generation != m_authGeneration) {
            reply->deleteLater();
            return;
        }
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        QByteArray bytes = reply->readAll();
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        SecureCredentialLocker::wipe(bytes);
        reply->deleteLater();
        if (!ok || !document.isObject()) {
            // Never echo server bodies, token contents, URLs or error strings.
            failLogin(status == 401 || status == 403
                ? (tokenType == QStringLiteral("Bearer")
                    ? QStringLiteral("The primary browser token is not accepted by the API (HTTP %1). The session was not saved.").arg(status)
                    : QStringLiteral("The secondary browser token is not accepted by the read-only API (HTTP %1). The session was not saved.").arg(status))
                : QStringLiteral("Browser token verification failed (HTTP %1). The session was not saved.").arg(status));
            return;
        }
        done(document.object());
    });
}

void RHSessionManager::acceptBrowserToken(QByteArray token, QDateTime expiresAt)
{
    acceptBrowserTokenImpl(std::move(token), expiresAt, QStringLiteral("WebSecondaryBearer"));
}

void RHSessionManager::acceptPrimaryBrowserToken(QByteArray token)
{
    // No lifetime is inferred from a request header. Validate identity/accounts
    // and cache for at most one hour; the server still authorizes every request.
    acceptBrowserTokenImpl(std::move(token), {}, QStringLiteral("Bearer"));
}

void RHSessionManager::acceptBrowserTokenImpl(QByteArray token, QDateTime expiresAt, const QString &tokenType)
{
    if (!m_browserLoginPending || !m_busy || m_browserVerifying || !m_browser || !m_browser->active()) {
        SecureCredentialLocker::wipe(token);
        return;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!RHNetworkEngine::safeBearer(token) || (expiresAt.isValid() && expiresAt <= now)) {
        SecureCredentialLocker::wipe(token);
        failLogin(QStringLiteral("Browser returned an expired or unsafe token."));
        return;
    }
    // Cookie expiry is an upper bound, not an OAuth lifetime. Session cookies
    // get one hour; even persistent cookies are capped to 24 hours.
    const QDateTime expiry = expiresAt.isValid() ? qMin(expiresAt.toUTC(), now.addSecs(86400)) : now.addSecs(3600);
    const quint64 generation = m_authGeneration;
    m_browserVerifying = true;
    readBrowserEndpoint(QStringLiteral("UserProfile"), token, tokenType, generation,
        [this, token, tokenType, expiry, generation](const QJsonObject &profile) {
        const QString id = profile.value("id").toString().trimmed();
        const QString email = profile.value("email").toString().trimmed();
        const QString username = profile.value("username").toString().trimmed();
        const QString label = email.isEmpty() ? username : email;
        if (id.isEmpty() || label.isEmpty()) {
            failLogin(QStringLiteral("Browser token did not return a usable account identity."));
            return;
        }
        if (!m_browserTarget.isEmpty()) {
            const Session *selected = nullptr;
            for (const Session &session : m_sessions)
                if ((session.accountId.isEmpty() ? session.accountName : session.accountId) == m_browserTarget)
                    selected = &session;
            const bool matches = selected && (selected->accountId.isEmpty()
                ? (selected->accountName.compare(email, Qt::CaseInsensitive) == 0
                    || selected->accountName.compare(username, Qt::CaseInsensitive) == 0)
                : selected->accountId == id);
            if (!matches) {
                failLogin(QStringLiteral("Signed-in identity does not match the selected saved account."));
                return;
            }
        }
        readBrowserEndpoint(QStringLiteral("Accounts"), token, tokenType, generation,
            [this, token, tokenType, expiry, generation, id, label](const QJsonObject &response) {
            if (!response.value("results").isArray() || expiry <= QDateTime::currentDateTimeUtc()) {
                failLogin(QStringLiteral("Browser token did not verify usable accounts and expiry."));
                return;
            }
            const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(expiry);
            if (seconds <= 0) {
                failLogin(QStringLiteral("Browser token expired during verification."));
                return;
            }
            commitBrowserSession(label, id, token, tokenType, expiry, generation);
        });
    });
    SecureCredentialLocker::wipe(token);
}

void RHSessionManager::commitBrowserSession(const QString &label, const QString &id,
    const QByteArray &token, const QString &tokenType, QDateTime expiry, quint64 generation,
    const QString &secret, bool canRefresh, bool activate)
{
    const QPointer<RHSessionManager> guard(this);
    const quint64 readGeneration = m_readGeneration;
    const quint64 operationGeneration = m_operationGeneration;
    const auto current = [guard, generation, readGeneration, operationGeneration] {
        return guard && guard->m_authGeneration == generation && guard->m_readGeneration == readGeneration
            && guard->m_operationGeneration == operationGeneration;
    };
    if (!current()) return;
    bool nativeLabel = false;
    if (canRefresh) {
        RHOAuthGrant checked;
        if (secret.isEmpty() || !RHOAuthGrant::deserialize(secret, &checked)) {
            failLogin(QStringLiteral("OAuth credentials could not be encoded safely; sign in again."));
            return;
        }
        nativeLabel = !checked.nativeEmail.isEmpty();
    }
    int slot = 0;
    bool replacing = false;
    for (const Session &existing : m_sessions) {
        if ((!m_browserTarget.isEmpty()
                && (existing.accountId.isEmpty() ? existing.accountName : existing.accountId) == m_browserTarget)
            || existing.accountId == id || (existing.accountId.isEmpty() && existing.accountName == label)) {
            slot = existing.index;
            replacing = true;
            break;
        }
    }
    if (!replacing) while (find(slot)) ++slot;
    QString failure;
    auto reservation = reserveCredentials(&failure);
    if (!reservation || !credentialOwnershipCurrent(slot, &failure)) {
        releaseCredentialReservation(); failLogin(failure); return;
    }
    const quint64 revision = allocateCredentialRevision(&failure);
    if (!revision) { releaseCredentialReservation(); failLogin(failure); return; }
    const QString payload = canRefresh ? secret : QString::fromLatin1(token);
    const auto previous = find(slot);
    const Session session{slot, label, QDateTime::currentDateTimeUtc(), expiry, tokenType, id, canRefresh,
        previous ? previous->renewalAttemptedAt : QDateTime{}, revision, credentialDigest(payload), false, nativeLabel};

    // Acquire the transaction gate before browser teardown or pending=false can
    // notify direct observers. Keep it through reconciliation and publication.
    m_sessionCommit = true;
    m_nativeCredentials.clear();
    m_oauthPending = false;
    m_browserVerifying = false;
    m_browserTarget.clear();
    m_browserLoginPending = false;
    if (m_nativeLoginPending) {
        m_nativeLoginStatus = QStringLiteral("Saving verified session securely");
        emit nativeLoginChanged();
        if (!current()) return;
    }
    if (m_browser) m_browser->finish();
    if (!current()) return;
    emit browserLoginPendingChanged();
    if (!current()) return;
    const auto finish = [this, current, session, token, activate](bool saved, const QString &failure = QString{}) {
        if (!current()) return;
        m_activateAfterApproval = false;
        setReauthenticating(false);
        if (!current()) return;
        if (!failure.isEmpty()) setError(failure);
        if (!current()) return;
        finishNativeLogin();
        if (!current()) return;
        m_sessionCommit = false;
        releaseCredentialReservation();
        if (saved && activate) {
            emit sessionSaved(session.index);
            if (!current()) return;
            verifySession(session.index, QString::fromLatin1(token));
            return;
        }
        // Last old-attempt action: direct idle observers may start another login.
        setBusy(false);
        if (!current()) return;
        if (saved) emit sessionSaved(session.index);
    };
    const auto commit = [this, guard, reservation, current, finish, session, replacing, payload, activate](QString previousToken) {
        if (!current()) return;
        QString failure;
        if (!credentialOwnershipCurrent(session.index, &failure)) { finish(false, failure); return; }
        m_credentialIOPending = true;
        m_store->write(session.index, payload,
            [this, guard, reservation, current, finish, session, replacing, previousToken, activate](bool ok, const QString &message) {
            if (guard) m_credentialIOPending = false;
            if (!current()) return;
            if (!ok) {
                if (activate) m_unreconciledSessions.insert(session.index);
                finish(false, message);
                return;
            }
            QList<Session> updated = m_sessions;
            int row = -1;
            for (int i = 0; i < updated.size(); ++i) {
                if (updated.at(i).index == session.index) {
                    row = i;
                    updated[i] = session;
                    break;
                }
            }
            if (!replacing) updated.append(session);
            QString failure;
            const bool persisted = persist(updated, &failure);
            if (!current()) return;
            if (!persisted) {
                // Rotation can revoke the predecessor immediately. Keep the
                // newly validated secure bundle as credential truth.
                if (activate) {
                    m_unreconciledSessions.insert(session.index);
                    finish(false, QStringLiteral("OAuth credentials were renewed securely, but session metadata could not be saved. Sign in again before using this profile."));
                    return;
                }
                m_credentialIOPending = true;
                const auto rolledBack = [this, guard, reservation, current, finish, failure, session](bool restored, const QString &) {
                    if (guard) m_credentialIOPending = false;
                    if (!current()) return;
                    if (!restored) m_unreconciledSessions.insert(session.index);
                    finish(false, restored ? failure
                        : QStringLiteral("Metadata save failed; keychain rollback also failed. Reauthenticate before using this session."));
                };
                if (replacing && !previousToken.isEmpty())
                    m_store->write(session.index, previousToken, rolledBack);
                else
                    m_store->remove(session.index, rolledBack);
                return;
            }
            m_unreconciledSessions.remove(session.index);
            if (replacing) {
                m_sessions[row] = session;
                emit dataChanged(index(row), index(row));
            } else {
                beginInsertRows({}, m_sessions.size(), m_sessions.size());
                // The commit gate forbids replacement. A surviving model must
                // always complete the begin/end pair, regardless of generation.
                if (!guard) return;
                m_sessions.append(session);
                endInsertRows();
            }
            if (!current()) return;
            finish(true);
        });
    };
    if (replacing) {
        m_credentialIOPending = true;
        m_store->read(slot, [this, guard, reservation, current, finish, commit](bool ok, QString previousToken, QString message) {
            if (guard) m_credentialIOPending = false;
            if (!current()) return;
            if (!ok) {
                finish(false, message);
                return;
            }
            commit(std::move(previousToken));
        });
    } else {
        commit({});
    }
}

void RHSessionManager::endApproval()
{
    ++m_authGeneration;
    const QPointer<RHSessionManager> guard(this);
    const auto generation = m_authGeneration;
    m_oauthPending = false;
    const bool browserPending = m_browserLoginPending;
    m_browserLoginPending = false;
    m_browserVerifying = false;
    m_browserTarget.clear();
    m_pendingCredentials.clear();
    m_nativeCredentials.clear();
    const bool nativePending = m_nativeLoginPending;
    m_nativeLoginPending = false; m_nativeRequestStarted = false;
    m_nativeLoginStatus.clear(); m_nativeLoginEmail.clear();
    if (m_reply) {
        QPointer<QNetworkReply> reply = m_reply;
        m_reply = nullptr;
        reply->abort();
        if (reply) reply->deleteLater();
        if (!guard || generation != m_authGeneration) return;
    }
    if (m_nativeClient) m_nativeClient->cancel();
    if (!guard || generation != m_authGeneration) return;
    if (m_browser) m_browser->cancel();
    if (!guard || generation != m_authGeneration) return;
    if (nativePending) emit nativeLoginChanged();
    if (!guard || generation != m_authGeneration) return;
    if (browserPending) emit browserLoginPendingChanged();
}

void RHSessionManager::failLogin(const QString &message)
{
    const QPointer<RHSessionManager> guard(this);
    const quint64 generation = m_authGeneration;
    const quint64 readGeneration = m_readGeneration;
    releaseCredentialReservation();
    endApproval();
    if (!guard || m_authGeneration != generation + 1 || m_readGeneration != readGeneration) return;
    setReauthenticating(false);
    if (!guard || m_authGeneration != generation + 1 || m_readGeneration != readGeneration) return;
    setError(message);
    if (!guard || m_authGeneration != generation + 1 || m_readGeneration != readGeneration) return;
    setBusy(false);
}

void RHSessionManager::cancelSignIn()
{
    if (m_sessionCommit) return;
    if (m_readingSession) {
        const QPointer<RHSessionManager> guard(this);
        const quint64 generation = m_readGeneration;
        // Retired error cleanup must precede idle: an idle observer can finish
        // the held read and synchronously fail a replacement save without
        // changing the read generation again.
        setError({});
        if (guard && generation == m_readGeneration) clearActiveSession();
        return;
    }
    if (!m_browserLoginPending && !m_nativeLoginPending)
        return;
    const QPointer<RHSessionManager> guard(this);
    const auto generation = m_authGeneration + 1;
    const auto readGeneration = m_readGeneration;
    endApproval();
    if (!guard || generation != m_authGeneration || readGeneration != m_readGeneration) return;
    setReauthenticating(false);
    if (!guard || generation != m_authGeneration || readGeneration != m_readGeneration) return;
    setError({});
    if (!guard || generation != m_authGeneration || readGeneration != m_readGeneration) return;
    setBusy(false);
}
