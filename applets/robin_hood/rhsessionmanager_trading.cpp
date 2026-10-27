#include "rhsessionmanager.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhoauthgrant.h"

#include <utility>
#include <QFutureWatcher>
#include <QtConcurrentRun>

QString RHSessionManager::tradingUnavailableReason() const
{
    if (m_busy)
        return QStringLiteral("Wait for the current operation to finish before trading.");
    const Session *session = find(m_activeIndex);
    if (!m_loaded || !session || !m_network.isConfigured())
        return QStringLiteral("Start a verified Robinhood session before trading.");
    if (session->expiresAt <= QDateTime::currentDateTimeUtc())
        return QStringLiteral("The session expired. Reauthenticate before trading.");
    if (session->tokenType.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("This browser session is read-only. Stock orders require separately verified primary OAuth authorization; this browser flow cannot safely acquire it yet.");
    const auto rows = m_accountCache.value(m_activeIndex);
    if (rows.isEmpty())
        return QStringLiteral("No verified brokerage accounts are available for this login.");
    for (const auto &value : rows) {
        if (value.toMap().value(QStringLiteral("accountNumber")).toString().isEmpty())
            return QStringLiteral("An account has no verified account number. Refresh the session before trading.");
    }
    return {};
}

bool RHSessionManager::isTradingSessionCurrent(const TradingSession &snapshot) const
{
    QString failure;
    return isTradingSessionLocallyCurrent(snapshot)
        && credentialOwnershipCurrent(snapshot.sessionIndex, &failure);
}

bool RHSessionManager::isTradingSessionLocallyCurrent(const TradingSession &snapshot) const
{
    const Session *session = find(snapshot.sessionIndex);
    return m_tradeLocked && m_busy && m_tradeGeneration == snapshot.generation
        && m_activeIndex == snapshot.sessionIndex && session
        && session->tokenType.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) == 0
        && (session->accountId.isEmpty() ? session->accountName : session->accountId) == snapshot.profileId
        && session->expiresAt == snapshot.expiresAt
        && snapshot.expiresAt > QDateTime::currentDateTimeUtc();
}

void RHSessionManager::checkTradingOwnershipAsync(TradingSession snapshot, std::function<void(bool)> done)
{
    if (!done) return;
    if (m_ownershipInFlight || !isTradingSessionLocallyCurrent(snapshot)) {
        done(false); return;
    }
    // Capture all immutable identity on the QObject thread. No manager, QObject,
    // token, secure store or mutable session member is touched by the worker.
    Session expected = *find(snapshot.sessionIndex);
    expected.accountName = metadataLabel(expected);
    expected.nativeLabel = false; // Persisted label is frozen on the QObject thread.
    const auto path = m_metadataPath;
    const auto beforeRead = m_ownershipBeforeRead;
    const auto afterRead = m_ownershipAfterRead;
    const auto maximumBytes = m_ownershipMaximumBytes;
    const auto operation = m_operationGeneration;
    snapshot.token.fill('\0'); snapshot.token.clear(); snapshot.accounts.clear();
    m_ownershipInFlight = true;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
        [this, watcher, snapshot = std::move(snapshot), operation, done = std::move(done)] {
        const bool ok = watcher->result();
        watcher->deleteLater();
        m_ownershipInFlight = false;
        // No release, model mutation or credential write on stale completion.
        done(ok && operation == m_operationGeneration && isTradingSessionLocallyCurrent(snapshot));
    });
    watcher->setFuture(QtConcurrent::run([path, expected, beforeRead, afterRead, maximumBytes] {
        if (beforeRead) beforeRead();
        QString failure;
        const bool ok = credentialOwnershipAtPath(path, &expected, expected.index, &failure, maximumBytes);
        if (afterRead) afterRead();
        return ok;
    }));
}

void RHSessionManager::requestTradingSession(TradingCompletion done)
{
    if (!done)
        return;
    const QString unavailable = tradingUnavailableReason();
    if (!unavailable.isEmpty()) {
        done(false, {}, unavailable);
        return;
    }
    const Session *session = find(m_activeIndex);
    QString failure;
    if (!credentialOwnershipCurrent(m_activeIndex, &failure)) {
        done(false, {}, failure); return;
    }
    TradingSession snapshot;
    snapshot.sessionIndex = m_activeIndex;
    snapshot.generation = ++m_tradeGeneration;
    snapshot.profileId = session->accountId.isEmpty() ? session->accountName : session->accountId;
    snapshot.expiresAt = session->expiresAt;
    snapshot.accounts = m_accountCache.value(m_activeIndex);
    m_tradeLocked = true;
    const QPointer<RHSessionManager> guard(this);
    setBusy(true);
    if (!guard)
        return;
    if (!isTradingSessionCurrent(snapshot)) {
        done(false, {}, QStringLiteral("Trading was cancelled before loading authorization."));
        return;
    }
    m_store->read(snapshot.sessionIndex,
        [this, guard, snapshot = std::move(snapshot), done = std::move(done)]
        (bool ok, QString token, QString) mutable {
        if (!guard) {
            SecureCredentialLocker::wipe(token);
            return;
        }
        if (!isTradingSessionCurrent(snapshot)) {
            SecureCredentialLocker::wipe(token);
            done(false, {}, QStringLiteral("Trading authorization was cancelled or expired."));
            return;
        }
        const bool decoded = RHOAuthGrant::extractAccessToken(token, &snapshot.token);
        RHOAuthGrant grant;
        const bool bundle = RHOAuthGrant::deserialize(token, &grant);
        const bool bound = !bundle || (grant.primary() && grant.accountId == snapshot.profileId
            && grant.expiresAt == snapshot.expiresAt && grant.expiresAt > QDateTime::currentDateTimeUtc());
        const Session *selected = find(snapshot.sessionIndex);
        const bool revisionMatches = selected && (selected->credentialDigest.isEmpty()
            || credentialDigest(token) == selected->credentialDigest);
        SecureCredentialLocker::wipe(token);
        if (!ok || !decoded || !bound || !revisionMatches || !RHNetworkEngine::safeBearer(snapshot.token)) {
            SecureCredentialLocker::wipe(snapshot.token);
            done(false, {}, QStringLiteral("Could not read a usable trading token from the secure store. Reauthenticate and retry."));
            return;
        }
        done(true, std::move(snapshot), {});
    });
}

void RHSessionManager::releaseTradingSession()
{
    if (!m_tradeLocked)
        return;
    m_tradeLocked = false;
    ++m_tradeGeneration;
    // A synchronous idle handler may immediately start another operation.
    setBusy(false);
}
