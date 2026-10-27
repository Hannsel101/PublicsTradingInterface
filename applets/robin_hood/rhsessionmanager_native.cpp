#include "rhsessionmanager.h"
#include "rhnativeauthclient.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhoauthgrant.h"

bool RHSessionManager::nativeCodeRequired() const
{
    return m_nativeLoginPending && !m_oauthPending && m_nativeClient
        && m_nativeClient->state() == RHNativeAuthClient::State::WaitingForCode;
}

void RHSessionManager::setNativeAuthClient(RHNativeAuthClient *client)
{
    if (m_nativeClient == client || m_sessionCommit || (m_busy && !m_nativeLoginPending)) return;
    const QPointer<RHSessionManager> guard(this);
    if (m_nativeLoginPending) cancelSignIn();
    if (!guard || m_busy) return;
    if (m_nativeClient) {
        m_nativeClient->setGrantHandler({});
        disconnect(m_nativeClient, nullptr, this, nullptr);
    }
    m_nativeClient = client;
    if (!client) return;
    connect(client, &RHNativeAuthClient::changed, this, [this] {
        if (!m_nativeLoginPending || m_sessionCommit || m_oauthPending || !m_nativeClient) return;
        const auto state = m_nativeClient->state();
        if (state == RHNativeAuthClient::State::Failed || state == RHNativeAuthClient::State::Cancelled) {
            failLogin(QStringLiteral("Native sign-in failed or was cancelled. Please retry."));
            return;
        }
        m_nativeLoginStatus = m_nativeClient->statusText();
        emit nativeLoginChanged(); // terminal: observers may cancel/delete/replace
    });
    connect(client, &QObject::destroyed, this, [this] {
        if (m_nativeLoginPending && !m_sessionCommit)
            failLogin(QStringLiteral("Native sign-in is unavailable."));
    });
}

bool RHSessionManager::beginNativeLogin(const QString &accountId)
{
    if (m_busy || m_sessionCommit || m_credentialIOPending || m_tradeLocked || !m_loaded
        || !m_nativeClient || !m_network.isConfigured()) return false;
    QString email;
    if (!accountId.isEmpty()) {
        const Session *selected = nullptr;
        for (const auto &session : m_sessions)
            if ((session.accountId.isEmpty() ? session.accountName : session.accountId) == accountId) selected = &session;
        if (!selected) return false;
        if (!selected->nativeLabel || selected->accountName != metadataLabel(*selected)) email = selected->accountName;
    }
    const QPointer<RHSessionManager> guard(this);
    const quint64 generation = ++m_authGeneration;
    const quint64 operationGeneration = m_operationGeneration + 1;
    ++m_labelGeneration;
    // Admission and complete attempt state precede every direct notification.
    m_nativeLoginPending = true; m_nativeRequestStarted = false;
    m_nativeLoginEmail = email; m_nativeLoginStatus = QStringLiteral("Enter email and password");
    m_browserTarget = accountId; m_browserVerifying = false;
    m_nativeCredentials.clear();
    setBusy(true);
    const auto current = [guard,generation,operationGeneration] {
        return guard && guard->m_authGeneration == generation && guard->m_nativeLoginPending
            && guard->m_operationGeneration == operationGeneration;
    };
    if (!current()) return true;
    m_activeIndex = -1; m_accounts.clear();
    if (!current()) return true;
    emit sessionActiveChanged();
    if (!current()) return true;
    setError({});
    if (!current()) return true;
    setReauthenticating(!accountId.isEmpty());
    if (!current()) return true;
    emit nativeLoginChanged();
    return true;
}

bool RHSessionManager::signIn(const QString &email, const QString &password, const QString &mfaCode)
{
    if (!m_nativeClient) {
        if (!m_sessionCommit) setError(QStringLiteral("Password sign-in is disabled. Use browser sign-in."));
        return false;
    }
    if (!m_nativeLoginPending || m_nativeRequestStarted || m_sessionCommit || m_oauthPending) return false;
    if (email.trimmed().isEmpty() || email.size() > 512 || password.isEmpty() || password.size() > 4096) return false;
    const QPointer<RHSessionManager> guard(this);
    const quint64 generation = m_authGeneration;
    const quint64 operationGeneration = m_operationGeneration;
    if (!ensureDeviceToken()) return false;
    if (!guard || generation != m_authGeneration || !m_nativeLoginPending) return false;
    if (!m_nativeCredentials.set(email.trimmed(), password)) { failLogin(QStringLiteral("Could not stage native credentials securely.")); return false; }
    m_nativeRequestStarted = true; m_nativeLoginEmail = email.trimmed();
    m_nativeClient->setGrantHandler([guard,generation,operationGeneration](RHOAuthGrant grant) {
        if (!guard || generation != guard->m_authGeneration || !guard->m_nativeLoginPending
            || operationGeneration != guard->m_operationGeneration || guard->m_oauthPending || guard->m_sessionCommit) return;
        RHOAuthGrant checked;
        if (!RHOAuthGrant::parseResponse(grant.response, grant.clientId, grant.deviceToken,
                grant.obtainedAt, &checked) || checked.expiresAt <= QDateTime::currentDateTimeUtc()) {
            guard->failLogin(QStringLiteral("A complete OAuth grant is required.")); return;
        }
        guard->m_oauthPending = true;
        guard->m_nativeLoginStatus = QStringLiteral("Verifying identity and accounts");
        emit guard->nativeLoginChanged();
        if (!guard || generation != guard->m_authGeneration || !guard->m_nativeLoginPending
            || operationGeneration != guard->m_operationGeneration) return;
        guard->validateOAuthGrant(std::move(checked), generation, false);
    });
    return m_nativeClient->start(email.trimmed(), password, m_deviceToken, mfaCode);
}

void RHSessionManager::submitNativeVerificationCode(const QString &code)
{
    if (nativeCodeRequired() && !m_sessionCommit) m_nativeClient->submitCode(code);
}

void RHSessionManager::finishNativeLogin()
{
    if (!m_nativeLoginPending) return;
    m_nativeLoginPending = false; m_nativeRequestStarted = false;
    m_nativeLoginStatus.clear(); m_nativeLoginEmail.clear(); m_nativeCredentials.clear();
    emit nativeLoginChanged();
}

QString RHSessionManager::metadataLabel(const Session &session)
{
    return session.nativeLabel ? QStringLiteral("Saved Robinhood account %1").arg(session.index + qint64(1)) : session.accountName;
}

void RHSessionManager::hydrateNativeLabels()
{
    const auto labelGeneration = m_labelGeneration;
    const auto authGeneration = m_authGeneration;
    const auto readGeneration = m_readGeneration;
    const auto operationGeneration = m_operationGeneration;
    const QPointer<RHSessionManager> guard(this);
    // These are read-only snapshots, not credential leases. Digest, durable owner
    // and all operation epochs must still match when each completion arrives.
    const auto sessions = m_sessions;
    for (const auto &snapshot : sessions) {
        if (!snapshot.nativeLabel) continue;
        m_store->read(snapshot.index,[guard,snapshot,labelGeneration,authGeneration,readGeneration,operationGeneration](bool ok, QString secret, QString) {
            if (!guard) { SecureCredentialLocker::wipe(secret); return; }
            const auto current = [&] {
                return guard && labelGeneration == guard->m_labelGeneration && authGeneration == guard->m_authGeneration
                    && readGeneration == guard->m_readGeneration && operationGeneration == guard->m_operationGeneration && !guard->m_busy;
            };
            RHOAuthGrant grant;
            QString failure;
            const auto *selected = guard->find(snapshot.index);
            const bool valid = current() && ok && selected && selected->nativeLabel
                && selected->accountId == snapshot.accountId && selected->credentialRevision == snapshot.credentialRevision
                && selected->credentialDigest == snapshot.credentialDigest
                && !snapshot.credentialDigest.isEmpty() && credentialDigest(secret) == snapshot.credentialDigest
                && guard->credentialOwnershipCurrent(snapshot.index,&failure)
                && RHOAuthGrant::deserialize(secret,&grant) && grant.accountId == snapshot.accountId && !grant.nativeEmail.isEmpty();
            SecureCredentialLocker::wipe(secret);
            if (!valid) return;
            for (int row=0;row<guard->m_sessions.size();++row) {
                if (guard->m_sessions[row].index != snapshot.index) continue;
                guard->m_sessions[row].accountName = grant.nativeEmail;
                emit guard->dataChanged(guard->index(row),guard->index(row),{AccountNameRole});
                if (current() && guard->m_activeIndex == snapshot.index) emit guard->sessionActiveChanged();
                return;
            }
        });
        if (!guard || labelGeneration != m_labelGeneration) return;
    }
}
