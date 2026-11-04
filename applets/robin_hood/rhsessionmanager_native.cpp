#include "rhsessionmanager.h"
#include "rhnativeauthclient.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhoauthgrant.h"
#include <QLockFile>
#include <QRegularExpression>

namespace {
bool storedEmailValid(const QString &email)
{
    static const QRegularExpression pattern(QStringLiteral("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$"));
    return email.size() <= 512 && pattern.match(email).hasMatch();
}
}

struct RHSessionManager::StoredInventoryOperation {
    QList<Session> sessions;
    QList<StoredSessionAccount> rows;
    StoredInventoryCompletion done;
    std::shared_ptr<QLockFile> reservation;
    QMetaObject::Connection destroyed;
    quint64 generation = 0;
    bool finished = false;
    void finish(bool ok, QString reason = {}) {
        if (finished) return;
        finished = true;
        QObject::disconnect(destroyed);
        auto callback = std::move(done);
        if (callback) callback(ok, ok ? rows : QList<StoredSessionAccount>{},
                               ok ? sessions : QList<Session>{}, std::move(reason));
    }
};

void RHSessionManager::requestStoredSessionAccounts(StoredSessionAccountsCompletion done)
{
    if (!done) return;
    if (m_busy || m_sessionCommit || m_credentialIOPending || m_tradeLocked || m_loadingMetadata || !m_loaded) {
        done(false, {}, QStringLiteral("saved_accounts_busy_or_unavailable")); return;
    }
    QString failure;
    if (!reserveCredentials(&failure)) { done(false, {}, QStringLiteral("saved_credentials_busy")); return; }
    const QPointer<RHSessionManager> guard(this);
    const auto epoch = m_operationGeneration + 1;
    setBusy(true);
    if (!guard || m_operationGeneration != epoch) { done(false, {}, QStringLiteral("saved_accounts_cancelled")); return; }
    collectStoredSessionAccounts([guard,done=std::move(done)](bool ok, auto rows, auto, QString reason) mutable {
        if (!guard) { done(false, {}, QStringLiteral("saved_accounts_cancelled")); return; }
        // Cleanup and idle precede the caller; no old state is mutated afterwards.
        guard->setBusy(false);
        done(ok, std::move(rows), std::move(reason));
    });
}

void RHSessionManager::collectStoredSessionAccounts(StoredInventoryCompletion done)
{
    auto operation = std::make_shared<StoredInventoryOperation>();
    operation->done = std::move(done);
    operation->reservation = m_credentialReservation;
    operation->generation = m_operationGeneration;
    operation->destroyed = connect(this, &QObject::destroyed, [operation] {
        operation->finish(false, QStringLiteral("saved_accounts_cancelled"));
    });
    if (!readStoredRoster(&operation->sessions)) {
        operation->finish(false, QStringLiteral("saved_accounts_metadata_invalid")); return;
    }
    readStoredSessionAccount(operation);
}

void RHSessionManager::readStoredSessionAccount(const std::shared_ptr<StoredInventoryOperation> &operation)
{
    if (operation->finished) return;
    if (operation->rows.size() == operation->sessions.size()) {
        // Verify the complete ordered roster again, including additions/deletions.
        QList<Session> current;
        QString failure;
        bool valid = readStoredRoster(&current) && current.size() == operation->sessions.size();
        for (qsizetype row=0; valid && row<current.size(); ++row) {
            const auto &expected = operation->sessions[row];
            valid = current[row].index == expected.index
                && credentialOwnershipAtPath(m_metadataPath, &expected, expected.index, &failure);
        }
        operation->finish(valid, valid ? QString{} : QStringLiteral("saved_accounts_changed")); return;
    }
    const Session snapshot = operation->sessions[operation->rows.size()];
    const QPointer<RHSessionManager> guard(this);
    m_credentialIOPending = true;
    auto consumed = std::make_shared<bool>(false);
    m_store->read(snapshot.index, [guard,operation,snapshot,consumed](bool ok, QString secret, QString) {
        if (*consumed) { SecureCredentialLocker::wipe(secret); return; }
        *consumed = true;
        if (guard) guard->m_credentialIOPending = false;
        QString failure, email;
        RHOAuthGrant grant;
        bool valid = guard && !operation->finished && ok && guard->m_busy
            && operation->generation == guard->m_operationGeneration
            && credentialOwnershipAtPath(guard->m_metadataPath, &snapshot, snapshot.index, &failure);
        if (valid && snapshot.canRefresh) {
            valid = snapshot.credentialRevision != 0 && !snapshot.credentialDigest.isEmpty()
                && credentialDigest(secret) == snapshot.credentialDigest
                && RHOAuthGrant::deserialize(secret, &grant) && grant.accountId == snapshot.accountId
                && grant.expiresAt == snapshot.expiresAt;
            if (valid) email = grant.nativeEmail;
        } else if (valid && !snapshot.credentialDigest.isEmpty()) {
            valid = credentialDigest(secret) == snapshot.credentialDigest;
        }
        if (valid && email.isEmpty() && !snapshot.nativeLabel) email = snapshot.accountName.trimmed();
        SecureCredentialLocker::wipe(secret);
        SecureCredentialLocker::wipe(grant.nativePassword);
        valid = valid && storedEmailValid(email);
        if (!valid) { operation->finish(false, QStringLiteral("saved_accounts_credentials_unavailable")); return; }
        operation->rows.append({snapshot.index, snapshot.accountId, email,
                               snapshot.expiresAt <= QDateTime::currentDateTimeUtc()});
        guard->readStoredSessionAccount(operation);
    });
}

void RHSessionManager::finishStoredAuthentication(StoredAuthenticationResult result, const QString &safeReason)
{
    const auto operation = m_storedAuthentication;
    if (!operation || operation->terminal) return;
    operation->terminal = true;
    operation->timer.stop();
    operation->credentials.clear();
    operation->ready = {};
    auto done = std::move(operation->done);
    // Timers can be queued behind a synchronous secure-store callback. The
    // original monotonic deadline, not event delivery order, owns the outcome.
    const bool expired=operation->clock.elapsed()>=operation->timeoutMs;
    if (done) done(expired ? StoredAuthenticationResult::Failed : result, operation->email,
                   expired ? QStringLiteral("stored_authentication_timeout") : safeReason);
}

void RHSessionManager::storedAuthenticationIdle()
{
    const auto operation = m_storedAuthentication;
    if (!operation) return;
    if (!operation->terminal) {
        operation->terminal=true; operation->timer.stop(); operation->credentials.clear(); operation->ready={};
        auto done=std::move(operation->done);
        m_storedAuthentication.reset();
        const QString reason=operation->clock.elapsed()>=operation->timeoutMs
            ? QStringLiteral("stored_authentication_timeout") : QStringLiteral("stored_authentication_cancelled");
        if (done) done(StoredAuthenticationResult::Failed,operation->email,reason);
        return;
    }
    m_storedAuthentication.reset();
}

void RHSessionManager::storedAuthenticationTimeout()
{
    const QPointer<RHSessionManager> guard(this);
    const auto operation = m_storedAuthentication;
    if (!operation || operation->terminal) return;
    finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("stored_authentication_timeout"));
    if (!guard || m_storedAuthentication != operation) return;
    // A secure write/rollback cannot be interrupted safely. Keep the reservation
    // and busy gate until it reconciles; a held lookup read similarly owns its lease.
    if (m_sessionCommit || m_credentialIOPending) return;
    failLogin(QStringLiteral("Stored authentication timed out."));
}

bool RHSessionManager::storedAuthenticationCanCommit(int slot, const QString &profileId) const
{
    const auto operation = m_storedAuthentication;
    if (!operation) return true;
    QString failure;
    return !operation->terminal && operation->clock.elapsed() < operation->timeoutMs
        && operation->pinned.index == slot && operation->pinned.accountId == profileId
        && credentialOwnershipAtPath(m_metadataPath, &operation->pinned, slot, &failure);
}

bool RHSessionManager::authenticateStoredSession(QString email, StoredAuthenticationReady ready,
                                                 StoredAuthenticationCompletion done)
{
    return authenticateStoredSession(std::move(email), std::move(ready), std::move(done), 480000);
}

bool RHSessionManager::authenticateStoredSession(QString email, StoredAuthenticationReady ready,
                                                 StoredAuthenticationCompletion done, int timeoutMs)
{
    if (m_busy || m_sessionCommit || m_credentialIOPending || m_tradeLocked || m_loadingMetadata
        || m_storedAuthentication) return false;
    QString failure;
    if (!reserveCredentials(&failure)) return false;
    auto operation = std::make_shared<StoredAuthenticationOperation>();
    operation->ready = std::move(ready); operation->done = std::move(done);
    operation->timeoutMs = qBound(1, timeoutMs, 480000);
    operation->clock.start();
    operation->timer.setSingleShot(true); operation->timer.setTimerType(Qt::PreciseTimer);
    m_storedAuthentication = operation;
    connect(&operation->timer, &QTimer::timeout, this, &RHSessionManager::storedAuthenticationTimeout);
    operation->timer.start(operation->timeoutMs);
    const QPointer<RHSessionManager> guard(this);
    if (!beginNativeLogin()) {
        finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("native_authentication_unavailable_local_authentication_required"));
        if (guard && m_storedAuthentication==operation) { m_storedAuthentication.reset(); releaseCredentialReservation(); }
        return true;
    }
    if (!guard || m_storedAuthentication != operation || operation->terminal) return true;
    // Never perform provider I/O in the admission stack or before progress.
    QTimer::singleShot(0, this, [guard,operation,email=email.trimmed()] {
        if (!guard || guard->m_storedAuthentication != operation || operation->terminal) return;
        if (!storedEmailValid(email)) {
            guard->finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("invalid_saved_account_email"));
            if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Invalid saved account email."));
            return;
        }
        guard->collectStoredSessionAccounts([guard,operation,email](bool ok, auto rows, auto snapshots, QString reason) {
            if (!guard || guard->m_storedAuthentication != operation) return;
            if (operation->terminal) { guard->failLogin(QStringLiteral("Stored authentication cancelled.")); return; }
            if (!ok) {
                guard->finishStoredAuthentication(StoredAuthenticationResult::Failed, reason);
                if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Saved account inventory is unavailable."));
                return;
            }
            int match=-1;
            for (int i=0;i<rows.size();++i) {
                if (rows[i].email.compare(email,Qt::CaseInsensitive)!=0) continue;
                if (match>=0) {
                    guard->finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("saved_account_email_ambiguous"));
                    if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Saved account email is ambiguous."));
                    return;
                }
                match=i;
            }
            if (match<0) {
                guard->finishStoredAuthentication(StoredAuthenticationResult::NotFound, QStringLiteral("saved_account_not_found"));
                if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Saved account not found."));
                return;
            }
            operation->pinned=snapshots[match]; operation->email=rows[match].email;
            QString failure;
            if (!guard->find(operation->pinned.index)
                || !guard->credentialOwnershipCurrent(operation->pinned.index,&failure)
                || !operation->pinned.canRefresh || operation->pinned.accountId.isEmpty()) {
                guard->finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("stored_credentials_unavailable_local_authentication_required"));
                if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Stored credentials are unavailable."));
                return;
            }
            guard->m_credentialIOPending=true;
            const auto reservation=guard->m_credentialReservation;
            auto consumed=std::make_shared<bool>(false);
            guard->m_store->read(operation->pinned.index,[guard,operation,reservation,consumed](bool loaded,QString secret,QString) {
                if (*consumed) { SecureCredentialLocker::wipe(secret); return; }
                *consumed=true;
                if (guard) guard->m_credentialIOPending=false;
                RHOAuthGrant grant;
                QString failure;
                const bool valid=guard && guard->m_storedAuthentication==operation && !operation->terminal && loaded
                    && operation->clock.elapsed()<operation->timeoutMs
                    && credentialOwnershipAtPath(guard->m_metadataPath,&operation->pinned,operation->pinned.index,&failure)
                    && !operation->pinned.credentialDigest.isEmpty()
                    && credentialDigest(secret)==operation->pinned.credentialDigest
                    && RHOAuthGrant::deserialize(secret,&grant) && grant.accountId==operation->pinned.accountId
                    && grant.expiresAt==operation->pinned.expiresAt
                    && grant.nativeEmail.compare(operation->email,Qt::CaseInsensitive)==0;
                SecureCredentialLocker::wipe(secret);
                if (!guard || guard->m_storedAuthentication!=operation) return;
                if (!valid) {
                    SecureCredentialLocker::wipe(grant.nativePassword);
                    guard->finishStoredAuthentication(StoredAuthenticationResult::Failed, QStringLiteral("stored_credentials_unavailable_local_authentication_required"));
                    if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Stored credentials are unavailable."));
                    return;
                }
                // Only a fresh, owner/digest/expiry-consistent secure snapshot can
                // authorize the no-op. Do not require or stage a password for it.
                const auto clock=guard->m_storedAuthenticationClock;
                const auto now = clock ? clock() : QDateTime::currentDateTimeUtc();
                if (!guard || guard->m_storedAuthentication!=operation || operation->terminal) {
                    SecureCredentialLocker::wipe(grant.nativePassword); return;
                }
                if (now.isValid() && now.msecsTo(grant.expiresAt)>=3600000) {
                    const auto expiry=grant.expiresAt.toUTC().toString(Qt::ISODateWithMs);
                    SecureCredentialLocker::wipe(grant.nativePassword);
                    guard->finishStoredAuthentication(StoredAuthenticationResult::NotNeeded,expiry);
                    if (!guard || guard->m_storedAuthentication!=operation) return;
                    const auto generation=guard->m_authGeneration+1;
                    guard->endApproval();
                    if (!guard || guard->m_storedAuthentication!=operation || guard->m_authGeneration!=generation) return;
                    guard->setReauthenticating(false);
                    if (!guard || guard->m_storedAuthentication!=operation || guard->m_authGeneration!=generation) return;
                    guard->setError({});
                    if (guard && guard->m_storedAuthentication==operation && guard->m_authGeneration==generation) guard->setBusy(false);
                    return;
                }
                const bool staged=now.isValid() && !grant.nativePassword.isEmpty()
                    && operation->credentials.set(grant.nativeEmail,grant.nativePassword);
                SecureCredentialLocker::wipe(grant.nativePassword);
                if (!staged) {
                    guard->finishStoredAuthentication(StoredAuthenticationResult::Failed,QStringLiteral("stored_credentials_unavailable_local_authentication_required"));
                    if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Stored credentials are unavailable."));
                    return;
                }
                guard->m_browserTarget=operation->pinned.accountId;
                auto ready=std::move(operation->ready);
                if (!ready) {
                    guard->finishStoredAuthentication(StoredAuthenticationResult::Failed,QStringLiteral("authentication_progress_callback_required"));
                    if (guard && guard->m_storedAuthentication==operation) guard->failLogin(QStringLiteral("Authentication progress is unavailable."));
                    return;
                }
                ready(operation->email,[guard,operation] {
                    if (guard && guard->m_storedAuthentication==operation) guard->continueStoredAuthentication(operation);
                });
            });
        });
    });
    return true;
}

void RHSessionManager::continueStoredAuthentication(const std::shared_ptr<StoredAuthenticationOperation> &operation)
{
    if (operation->continued || operation->terminal || m_storedAuthentication!=operation) return;
    operation->continued=true;
    if (operation->clock.elapsed()>=operation->timeoutMs) { storedAuthenticationTimeout(); return; }
    QString email,password;
    if (!storedAuthenticationCanCommit(operation->pinned.index,operation->pinned.accountId)
        || !operation->credentials.read(email,password)) {
        failLogin(QStringLiteral("Stored profile changed before authentication.")); return;
    }
    operation->credentials.clear();
    operation->dispatching=true;
    const QPointer<RHSessionManager> guard(this);
    const bool started=signIn(email,password);
    SecureCredentialLocker::wipe(email); SecureCredentialLocker::wipe(password);
    operation->dispatching=false;
    if (guard && !started && !operation->terminal) failLogin(QStringLiteral("Stored native authentication could not start."));
}

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
        if (m_storedAuthentication && state == RHNativeAuthClient::State::WaitingForCode) {
            m_storedAuthentication->nativeFailure=QStringLiteral("unsupported_challenge_local_authentication_required");
            failLogin(QStringLiteral("Unsupported challenge; complete authentication locally.")); return;
        }
        if (state == RHNativeAuthClient::State::Failed || state == RHNativeAuthClient::State::Cancelled) {
            // This client status is built solely from finite native vocabulary;
            // raw provider/store detail is never forwarded by the remote API.
            if (m_storedAuthentication) {
                const auto operation=m_storedAuthentication;
                operation->nativeFailure = operation->clock.elapsed() >= operation->timeoutMs
                    ? QStringLiteral("stored_authentication_timeout")
                    : m_nativeClient->statusText() == QStringLiteral("Unsupported challenge; complete authentication locally")
                        ? QStringLiteral("unsupported_challenge_local_authentication_required") : m_nativeClient->statusText();
            }
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
    if (!m_storedAuthentication) {
        m_activeIndex = -1; m_accounts.clear();
        if (!current()) return true;
        emit sessionActiveChanged();
        if (!current()) return true;
    }
    setError({});
    if (!current()) return true;
    setReauthenticating(!accountId.isEmpty());
    if (!current()) return true;
    emit nativeLoginChanged();
    return true;
}

bool RHSessionManager::signIn(const QString &email, const QString &password, const QString &mfaCode)
{
    if (m_storedAuthentication && !m_storedAuthentication->dispatching) return false;
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
    if (m_storedAuthentication) {
        const auto operation = m_storedAuthentication;
        const auto remaining = operation->timeoutMs - operation->clock.elapsed();
        if (remaining <= 0) { storedAuthenticationTimeout(); return false; }
        return m_nativeClient->startRemote(email.trimmed(), password, m_deviceToken, int(remaining));
    }
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
