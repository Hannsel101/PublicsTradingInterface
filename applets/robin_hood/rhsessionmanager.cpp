#include "rhsessionmanager.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhbrowserlogin.h"
#include "securecredentiallocker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qint64 maxLifetime = 30LL * 24 * 60 * 60;
QString defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/robinhood/sessions.json");
}

bool readMetadata(const QString &path, QJsonObject &metadata, QString &error)
{
    metadata = {{QStringLiteral("sessions"), QJsonArray{}}};
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Could not read saved session metadata.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const auto invalid = [&error]() {
        error = QStringLiteral("Session metadata is invalid; it was not overwritten.");
        return false;
    };
    if (file.error() != QFileDevice::NoError || parseError.error != QJsonParseError::NoError
        || !document.isObject() || !document.object().value(QStringLiteral("sessions")).isArray())
        return invalid();
    metadata = document.object();
    const auto token = metadata.value(QStringLiteral("device_token"));
    static const QRegularExpression uuidV4(
        QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\\z"),
        QRegularExpression::CaseInsensitiveOption);
    if (!token.isUndefined() && (!token.isString() || !uuidV4.match(token.toString()).hasMatch())) {
        error = QStringLiteral("Session metadata device token is invalid; it was not overwritten.");
        return false;
    }
    QSet<int> seen;
    for (const auto &value : metadata.value(QStringLiteral("sessions")).toArray()) {
        if (!value.isObject())
            return invalid();
        const QJsonObject item = value.toObject();
        const QJsonValue rawIndex = item.value(QStringLiteral("index"));
        const double numericIndex = rawIndex.toDouble(-1);
        if (!rawIndex.isDouble() || numericIndex < 0
            || numericIndex > std::numeric_limits<int>::max()
            || std::floor(numericIndex) != numericIndex)
            return invalid();
        const int index = static_cast<int>(numericIndex);
        const QDateTime created = QDateTime::fromString(
            item.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        const QDateTime expires = QDateTime::fromString(
            item.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
        if (seen.contains(index) || item.value(QStringLiteral("account_name")).toString().trimmed().isEmpty()
            || !created.isValid() || !expires.isValid() || expires <= created
            || created.secsTo(expires) > maxLifetime
            || (item.value(QStringLiteral("token_type")).toString().compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0
                && item.value(QStringLiteral("token_type")).toString() != QStringLiteral("WebSecondaryBearer"))
            || (item.value("token_type").toString() == QStringLiteral("WebSecondaryBearer")
                && (item.value("account_id").toString().isEmpty() || created.secsTo(expires) > 86400)))
            return invalid();
        seen.insert(index);
    }
    return true;
}
}

RHSessionManager::RHSessionManager(RHNetworkEngine &network, QString metadataPath,
                                   RHCredentialStore *store, QObject *parent,
                                   int approvalTimeoutMs, int approvalPollMs, SecureCredentialLocker *locker)
    : QAbstractListModel(parent), m_network(network),
      m_metadataPath(metadataPath.isEmpty() ? defaultPath() : std::move(metadataPath)),
      m_store(store), m_locker(locker), m_approvalTimeoutMs(approvalTimeoutMs), m_approvalPollMs(approvalPollMs)
{
    if (!m_store) {
        m_ownedStore = std::make_unique<RHQtKeychainStore>();
        m_store = m_ownedStore.get();
    }
    if (!m_locker) {
        m_ownedLocker = std::make_unique<SecureCredentialLocker>();
        m_locker = m_ownedLocker.get();
    }
    m_expiryTimer.setInterval(30000);
    connect(&m_expiryTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessions.isEmpty())
            emit dataChanged(index(0), index(m_sessions.size() - 1), {IsExpiredRole});
    });
    m_expiryTimer.start();
    load();
}

RHSessionManager::~RHSessionManager() { endApproval(); }

int RHSessionManager::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sessions.size();
}

QVariant RHSessionManager::data(const QModelIndex &item, int role) const
{
    if (!item.isValid() || item.row() < 0 || item.row() >= m_sessions.size())
        return {};
    const Session &session = m_sessions.at(item.row());
    switch (role) {
    case AccountNameRole: return session.accountName;
    case AccountIdRole: return session.accountId.isEmpty() ? session.accountName : session.accountId;
    case SessionIndexRole: return session.index;
    case ExpiresAtRole: return session.expiresAt.toString(Qt::ISODateWithMs);
    case IsExpiredRole: return QDateTime::currentDateTimeUtc() >= session.expiresAt;
    default: return {};
    }
}

QHash<int, QByteArray> RHSessionManager::roleNames() const
{
    return {{AccountNameRole, "accountName"}, {SessionIndexRole, "sessionIndex"},
            {ExpiresAtRole, "expiresAt"}, {IsExpiredRole, "isExpired"}, {AccountIdRole, "accountId"}};
}

QString RHSessionManager::activeAccount() const
{
    const Session *session = find(m_activeIndex);
    return session ? session->accountName : QString();
}

const RHSessionManager::Session *RHSessionManager::find(int sessionIndex) const
{
    for (const Session &session : m_sessions) {
        if (session.index == sessionIndex)
            return &session;
    }
    return nullptr;
}

void RHSessionManager::setBusy(bool value)
{
    if (m_busy == value)
        return;
    m_busy = value;
    emit busyChanged();
}

void RHSessionManager::setError(const QString &message)
{
    if (m_error == message)
        return;
    m_error = message;
    emit errorChanged();
}

bool RHSessionManager::load()
{
    if (m_busy)
        return false;
    m_loaded = false;
    if (!m_sessions.isEmpty()) {
        beginResetModel();
        m_sessions.clear();
        endResetModel();
    }
    QJsonObject metadata;
    QString error;
    if (!readMetadata(m_metadataPath, metadata, error)) {
        setError(error);
        return false;
    }
    QList<Session> parsed;
    for (const auto &value : metadata.value(QStringLiteral("sessions")).toArray()) {
        const QJsonObject item = value.toObject();
        parsed.append(Session{item.value(QStringLiteral("index")).toInt(),
                              item.value(QStringLiteral("account_name")).toString(),
                              QDateTime::fromString(item.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs),
                              QDateTime::fromString(item.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs),
                              item.value(QStringLiteral("token_type")).toString(),
                              item.value(QStringLiteral("account_id")).toString()});
    }
    const auto token = metadata.value(QStringLiteral("device_token"));
    if (token.isString())
        m_deviceToken = token.toString();
    beginResetModel();
    m_sessions = std::move(parsed);
    endResetModel();
    m_loaded = true;
    setError({});
    return true;
}

bool RHSessionManager::ensureDeviceToken()
{
    if (!m_loaded)
        return false;
    if (!QDir().mkpath(QFileInfo(m_metadataPath).absolutePath())) {
        setError(QStringLiteral("Could not create the session metadata directory."));
        return false;
    }
    QLockFile lock(m_metadataPath + QStringLiteral(".lock"));
    if (!lock.tryLock(1000)) {
        setError(QStringLiteral("Could not lock session metadata."));
        return false;
    }
    // Read again under the shared metadata lock: another model/process may have
    // created the machine identity since this model was constructed.
    QJsonObject metadata;
    QString error;
    if (!readMetadata(m_metadataPath, metadata, error)) {
        setError(error);
        return false;
    }
    const auto saved = metadata.value(QStringLiteral("device_token"));
    if (saved.isString()) {
        m_deviceToken = saved.toString();
        return true;
    }
    const QString token = m_deviceToken.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : m_deviceToken;
    metadata.insert(QStringLiteral("device_token"), token);
    QSaveFile file(m_metadataPath);
    const QByteArray bytes = QJsonDocument(metadata).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setError(QStringLiteral("Could not commit session metadata."));
        return false;
    }
    m_deviceToken = token;
    return true;
}

bool RHSessionManager::persist(const QList<Session> &sessions)
{
    const QFileInfo path(m_metadataPath);
    if (!QDir().mkpath(path.absolutePath())) {
        setError(QStringLiteral("Could not create the session metadata directory."));
        return false;
    }
    QLockFile lock(m_metadataPath + QStringLiteral(".lock"));
    if (!lock.tryLock(1000)) {
        setError(QStringLiteral("Could not lock session metadata."));
        return false;
    }
    QJsonObject metadata;
    QString error;
    if (!readMetadata(m_metadataPath, metadata, error)) {
        setError(error);
        return false;
    }
    if (!metadata.contains(QStringLiteral("device_token")) && !m_deviceToken.isEmpty())
        metadata.insert(QStringLiteral("device_token"), m_deviceToken);
    const auto record = [](const Session &session) {
        return QJsonObject{
            {QStringLiteral("index"), session.index},
            {QStringLiteral("account_name"), session.accountName},
            {QStringLiteral("created_at"), session.createdAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("expires_at"), session.expiresAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("token_type"), session.tokenType},
            {QStringLiteral("account_id"), session.accountId}
        };
    };
    QJsonArray array = metadata.value(QStringLiteral("sessions")).toArray();
    // Apply only this model's changes to the latest metadata. A stale model must
    // not erase profiles (or extension fields) written by another model.
    for (const Session &old : m_sessions) {
        bool retained = false;
        for (const Session &session : sessions)
            retained |= session.index == old.index;
        if (!retained) {
            for (qsizetype i = array.size(); i-- > 0;) {
                const QJsonObject item = array.at(i).toObject();
                if (item.value(QStringLiteral("index")).toInt() == old.index
                    && item.value(QStringLiteral("account_name")).toString() == old.accountName)
                    array.removeAt(i);
            }
        }
    }
    for (const Session &session : sessions) {
        const Session *old = find(session.index);
        const QJsonObject updated = record(session);
        if (old && record(*old) == updated)
            continue;
        qsizetype row = -1;
        QJsonObject item;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (array.at(i).toObject().value(QStringLiteral("index")).toInt() == session.index) {
                row = i;
                item = array.at(i).toObject();
                break;
            }
        }
        const bool migratingLegacyIdentity = old && old->accountId.isEmpty() && !session.accountId.isEmpty()
            && item.value("account_id").toString().isEmpty()
            && item.value("account_name").toString() == old->accountName;
        if (row >= 0 && item.value(QStringLiteral("account_name")).toString() != session.accountName
            && !migratingLegacyIdentity
            && (session.accountId.isEmpty() || item.value("account_id").toString() != session.accountId)) {
            setError(QStringLiteral("Saved sessions changed; reload session metadata before retrying."));
            return false;
        }
        for (auto it = updated.begin(); it != updated.end(); ++it)
            item.insert(it.key(), it.value());
        if (row >= 0)
            array[row] = item;
        else
            array.append(item);
    }
    QSaveFile file(m_metadataPath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Could not write session metadata."));
        return false;
    }
    metadata.insert(QStringLiteral("sessions"), array);
    const QByteArray json = QJsonDocument(metadata).toJson();
    if (file.write(json) != json.size() || !file.commit()) {
        setError(QStringLiteral("Could not commit session metadata."));
        return false;
    }
    return true;
}

bool RHSessionManager::saveAuthenticatedSession(const QString &accountName, const QString &token,
                                                 qint64 expiresInSeconds, const QString &tokenType)
{
    if (tokenType.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0)
        return false;
    return saveSession(accountName, token, expiresInSeconds, QStringLiteral("Bearer"));
}

bool RHSessionManager::saveSession(const QString &accountName, const QString &token,
    qint64 expiresInSeconds, const QString &tokenType, const QString &accountId, bool continuingBrowser)
{
    const QString name = accountName.trimmed();
    if ((m_busy && !continuingBrowser) || !m_loaded || name.isEmpty() || !RHNetworkEngine::safeBearer(token.toLatin1())
        || expiresInSeconds <= 0) {
        setError(QStringLiteral("Cannot save an incomplete authenticated session."));
        return false;
    }
    int slot = 0;
    bool replacing = false;
    for (const Session &existing : m_sessions) {
        if ((continuingBrowser && !m_browserTarget.isEmpty()
                && (existing.accountId.isEmpty() ? existing.accountName : existing.accountId) == m_browserTarget)
            || (!accountId.isEmpty() && existing.accountId == accountId)
            || (existing.accountId.isEmpty() && existing.accountName == name)) {
            slot = existing.index;
            replacing = true;
            break;
        }
    }
    if (!replacing) {
        while (find(slot))
            ++slot;
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const Session session{slot, name, now,
        now.addSecs(qMin(expiresInSeconds, tokenType == QStringLiteral("WebSecondaryBearer") ? 86400LL : maxLifetime)),
        tokenType, accountId};
    setError({});
    setBusy(true);
    const QPointer<RHSessionManager> guard(this);
    const auto commit = [this, guard, session, replacing, token](const QString &previousToken) {
        if (!guard) return;
        m_store->write(session.index, token, [this, guard, session, replacing, previousToken](bool ok, const QString &message) {
        if (!guard)
            return;
        if (!ok) {
            m_activateAfterApproval = false;
            setReauthenticating(false);
            setError(message);
            setBusy(false);
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
        if (!replacing)
            updated.append(session);
        if (!persist(updated)) {
            m_activateAfterApproval = false;
            setReauthenticating(false);
            const auto rolledBack = [this, guard](bool restored, const QString &) {
                if (!guard)
                    return;
                if (!restored)
                    setError(QStringLiteral("Metadata save failed; keychain rollback also failed."));
                setBusy(false);
            };
            if (replacing && !previousToken.isEmpty())
                m_store->write(session.index, previousToken, rolledBack);
            else
                m_store->remove(session.index, rolledBack);
            return;
        }
        if (replacing) {
            m_sessions[row] = session;
            emit dataChanged(index(row), index(row));
        } else {
            beginInsertRows({}, m_sessions.size(), m_sessions.size());
            m_sessions.append(session);
            endInsertRows();
        }
        setBusy(false);
        setReauthenticating(false);
        const bool activate = std::exchange(m_activateAfterApproval, false);
        emit sessionSaved(session.index);
        if (guard && activate)
            startSession(session.index);
        });
    };
    if (replacing) {
        m_store->read(slot, [this, guard, commit](bool ok, QString previousToken, QString message) {
            if (!guard)
                return;
            if (!ok) {
                m_activateAfterApproval = false;
                setReauthenticating(false);
                setError(message);
                setBusy(false);
                return;
            }
            commit(previousToken);
        });
    } else {
        commit({});
    }
    return true;
}

bool RHSessionManager::startSession(int sessionIndex)
{
    const Session *session = find(sessionIndex);
    if (!m_loaded || m_busy || !session || QDateTime::currentDateTimeUtc() >= session->expiresAt) {
        setError(QStringLiteral("Select an unexpired saved session."));
        return false;
    }
    clearActiveSession();
    setError({});
    setBusy(true);
    const QPointer<RHSessionManager> guard(this);
    m_store->read(sessionIndex, [this, guard, sessionIndex](bool ok, const QString &token, const QString &message) {
        if (!guard) return;
        if (!ok || token.isEmpty()) {
            setError(message.isEmpty() ? QStringLiteral("Saved session token is unavailable.") : message);
            setBusy(false);
            return;
        }
        if (!find(sessionIndex) || QDateTime::currentDateTimeUtc() >= find(sessionIndex)->expiresAt) {
            setError(QStringLiteral("This session has expired; reauthenticate."));
            setBusy(false);
            return;
        }
        verifySession(sessionIndex, token);
    });
    return true;
}

void RHSessionManager::verifySession(int sessionIndex, const QString &token)
{
    QString problem;
    const Session *session = find(sessionIndex);
    QNetworkReply *reply = m_network.sendAuthenticated(QStringLiteral("Accounts"), token.toLatin1(),
        session->tokenType == QStringLiteral("WebSecondaryBearer")
            ? RHNetworkEngine::AuthKind::WebSecondaryBearer : RHNetworkEngine::AuthKind::OAuthBearer, &problem);
    if (!reply) {
        setError(problem);
        setBusy(false);
        return;
    }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, sessionIndex]() {
        m_reply = nullptr;
        const bool transportOk = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 200
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 300;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QJsonValue results = document.object().value(QStringLiteral("results"));
        if (!transportOk || !results.isArray()) {
            setError(QStringLiteral("Could not verify the saved session. Reauthenticate if it was revoked."));
            setBusy(false);
            return;
        }
        m_accounts.setAccounts(results.toArray());
        m_activeIndex = sessionIndex;
        emit sessionActiveChanged();
        setBusy(false);
    });
}

bool RHSessionManager::reauthenticate(int sessionIndex)
{
    const Session *session = find(sessionIndex);
    if (!session || m_busy || !m_loaded || !m_network.isConfigured()) {
        setError(QStringLiteral("Select a saved session after loading network configuration."));
        return false;
    }
    return beginBrowserLogin(session->accountId.isEmpty() ? session->accountName : session->accountId);
}

void RHSessionManager::setReauthenticating(bool value)
{
    if (m_reauthenticating == value)
        return;
    m_reauthenticating = value;
    emit reauthenticatingChanged();
}

void RHSessionManager::prepareReauthentication(const QString &accountId)
{
    beginBrowserLogin(accountId);
}

bool RHSessionManager::deleteSavedSession(int sessionIndex)
{
    return removeSession(sessionIndex);
}

bool RHSessionManager::removeSession(int sessionIndex)
{
    const Session *session = find(sessionIndex);
    if (m_busy || !m_loaded || !session) {
        setError(QStringLiteral("Select a saved session to delete."));
        return false;
    }
    const QString name = session->accountName;
    setError({});
    setBusy(true);
    const QPointer<RHSessionManager> guard(this);
    m_store->remove(sessionIndex, [this, guard, sessionIndex, name](bool ok, const QString &message) {
        if (!guard)
            return;
        if (!ok) {
            setError(message);
            setBusy(false);
            return;
        }
        m_locker->remove(name, [this, guard, sessionIndex](bool removed, QString error) {
        if (!guard)
            return;
        if (!removed) {
            setError(error);
            setBusy(false);
            return;
        }
        if (sessionIndex == m_activeIndex)
            clearActiveSession();
        QList<Session> updated = m_sessions;
        for (auto it = updated.begin(); it != updated.end(); ++it) {
            if (it->index == sessionIndex) {
                updated.erase(it);
                break;
            }
        }
        if (!persist(updated)) {
            setBusy(false);
            return;
        }
        for (int row = 0; row < m_sessions.size(); ++row) {
            if (m_sessions.at(row).index == sessionIndex) {
                beginRemoveRows({}, row, row);
                m_sessions.removeAt(row);
                endRemoveRows();
                break;
            }
        }
        setError({});
        setBusy(false);
        emit sessionDeleted(sessionIndex);
        });
    });
    return true;
}

void RHSessionManager::clearActiveSession()
{
    if (m_activeIndex >= 0) {
        m_activeIndex = -1;
        emit sessionActiveChanged();
    }
    m_accounts.clear();
}
