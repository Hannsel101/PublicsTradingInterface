#include "rhsessionmanager.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

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
}

RHSessionManager::RHSessionManager(RHNetworkEngine &network, QString metadataPath,
                                   RHCredentialStore *store, QObject *parent,
                                   int approvalTimeoutMs, int approvalPollMs)
    : QAbstractListModel(parent), m_network(network),
      m_metadataPath(metadataPath.isEmpty() ? defaultPath() : std::move(metadataPath)),
      m_store(store), m_approvalTimeoutMs(approvalTimeoutMs), m_approvalPollMs(approvalPollMs)
{
    if (!m_store) {
        m_ownedStore = std::make_unique<RHQtKeychainStore>();
        m_store = m_ownedStore.get();
    }
    m_expiryTimer.setInterval(30000);
    connect(&m_expiryTimer, &QTimer::timeout, this, [this]() {
        if (!m_sessions.isEmpty())
            emit dataChanged(index(0), index(m_sessions.size() - 1), {IsExpiredRole});
    });
    m_expiryTimer.start();
    m_pollTimer.setSingleShot(true);
    m_pollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pollTimer, &QTimer::timeout, this, &RHSessionManager::pollApproval);
    m_approvalTimeout.setSingleShot(true);
    m_approvalTimeout.setTimerType(Qt::PreciseTimer);
    connect(&m_approvalTimeout, &QTimer::timeout, this, [this]() {
        failLogin(QStringLiteral("Device approval timed out."));
    });
    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, [this]() {
        const int remaining = qMax(0, (m_approvalTimeoutMs - int(m_approvalClock.elapsed()) + 999) / 1000);
        if (remaining != m_approvalSecondsRemaining) {
            m_approvalSecondsRemaining = remaining;
            emit approvalChanged();
        }
    });
    load();
}

RHSessionManager::~RHSessionManager() = default;

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
    case SessionIndexRole: return session.index;
    case ExpiresAtRole: return session.expiresAt.toString(Qt::ISODateWithMs);
    case IsExpiredRole: return QDateTime::currentDateTimeUtc() >= session.expiresAt;
    default: return {};
    }
}

QHash<int, QByteArray> RHSessionManager::roleNames() const
{
    return {{AccountNameRole, "accountName"}, {SessionIndexRole, "sessionIndex"},
            {ExpiresAtRole, "expiresAt"}, {IsExpiredRole, "isExpired"}};
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
    QList<Session> parsed;
    QFile file(m_metadataPath);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) {
            setError(QStringLiteral("Could not read saved session metadata."));
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !document.object().value(QStringLiteral("sessions")).isArray()) {
            setError(QStringLiteral("Session metadata is invalid; it was not overwritten."));
            return false;
        }
        QSet<int> seen;
        for (const QJsonValue &value : document.object().value(QStringLiteral("sessions")).toArray()) {
            if (!value.isObject()) {
                setError(QStringLiteral("Session metadata is invalid; it was not overwritten."));
                return false;
            }
            const QJsonObject item = value.toObject();
            const QJsonValue rawIndex = item.value(QStringLiteral("index"));
            const double numericIndex = rawIndex.toDouble(-1);
            if (!rawIndex.isDouble() || numericIndex < 0
                || numericIndex > std::numeric_limits<int>::max()
                || std::floor(numericIndex) != numericIndex) {
                setError(QStringLiteral("Session metadata is invalid; it was not overwritten."));
                return false;
            }
            Session session{static_cast<int>(numericIndex),
                            item.value(QStringLiteral("account_name")).toString(),
                            QDateTime::fromString(item.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs),
                            QDateTime::fromString(item.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs),
                            item.value(QStringLiteral("token_type")).toString()};
            if (seen.contains(session.index) || session.accountName.trimmed().isEmpty()
                || !session.createdAt.isValid() || !session.expiresAt.isValid()
                || session.expiresAt <= session.createdAt
                || session.createdAt.secsTo(session.expiresAt) > maxLifetime
                || session.tokenType.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0) {
                setError(QStringLiteral("Session metadata is invalid; it was not overwritten."));
                return false;
            }
            seen.insert(session.index);
            parsed.append(session);
        }
    }
    beginResetModel();
    m_sessions = std::move(parsed);
    endResetModel();
    m_loaded = true;
    setError({});
    return true;
}

bool RHSessionManager::persist(const QList<Session> &sessions)
{
    const QFileInfo path(m_metadataPath);
    if (!QDir().mkpath(path.absolutePath())) {
        setError(QStringLiteral("Could not create the session metadata directory."));
        return false;
    }
    QJsonArray array;
    for (const Session &session : sessions) {
        array.append(QJsonObject{
            {QStringLiteral("index"), session.index},
            {QStringLiteral("account_name"), session.accountName},
            {QStringLiteral("created_at"), session.createdAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("expires_at"), session.expiresAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("token_type"), session.tokenType}
        });
    }
    QSaveFile file(m_metadataPath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Could not write session metadata."));
        return false;
    }
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("sessions"), array}}).toJson();
    if (file.write(json) != json.size() || !file.commit()) {
        setError(QStringLiteral("Could not commit session metadata."));
        return false;
    }
    return true;
}

bool RHSessionManager::saveAuthenticatedSession(const QString &accountName, const QString &token,
                                                 qint64 expiresInSeconds, const QString &tokenType)
{
    const QString name = accountName.trimmed();
    if (m_busy || !m_loaded || name.isEmpty() || token.isEmpty()
        || tokenType.compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0
        || expiresInSeconds <= 0) {
        setError(QStringLiteral("Cannot save an incomplete authenticated session."));
        return false;
    }
    int slot = 0;
    while (find(slot))
        ++slot;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const Session session{slot, name, now, now.addSecs(qMin(expiresInSeconds, maxLifetime)),
                          QStringLiteral("Bearer")};
    setError({});
    setBusy(true);
    m_store->write(slot, token, [this, session](bool ok, const QString &message) {
        if (!ok) {
            m_activateAfterApproval = false;
            setError(message);
            setBusy(false);
            return;
        }
        QList<Session> updated = m_sessions;
        updated.append(session);
        if (!persist(updated)) {
            m_activateAfterApproval = false;
            m_store->remove(session.index, [this](bool removed, const QString &) {
                if (!removed)
                    setError(QStringLiteral("Metadata save failed; orphaned keychain entry must be removed manually."));
                setBusy(false);
            });
            return;
        }
        beginInsertRows({}, m_sessions.size(), m_sessions.size());
        m_sessions.append(session);
        endInsertRows();
        setBusy(false);
        emit sessionSaved(session.index);
        if (std::exchange(m_activateAfterApproval, false))
            startSession(session.index);
    });
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
    m_store->read(sessionIndex, [this, sessionIndex](bool ok, const QString &token, const QString &message) {
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
    QNetworkReply *reply = m_network.send(QStringLiteral("Accounts"),
        {{QStringLiteral("token_type"), session->tokenType},
         {QStringLiteral("access_token"), token}}, {}, {}, &problem);
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
        m_accounts.clear();
        for (const QJsonValue &value : results.toArray()) {
            if (value.isObject()) {
                const QJsonObject item = value.toObject();
                m_accounts.append(QVariantMap{
                    {QStringLiteral("accountNumber"), item.value(QStringLiteral("account_number")).toString()},
                    {QStringLiteral("type"), item.value(QStringLiteral("type")).toString()}
                });
            }
        }
        emit accountsChanged();
        m_activeIndex = sessionIndex;
        emit sessionActiveChanged();
        setBusy(false);
    });
}

bool RHSessionManager::reauthenticate(int sessionIndex)
{
    const Session *session = find(sessionIndex);
    if (m_busy || !m_loaded || !session) {
        setError(QStringLiteral("Select a saved session to reauthenticate."));
        return false;
    }
    if (sessionIndex == m_activeIndex)
        clearActiveSession();
    const QString name = session->accountName;
    setError({});
    setBusy(true);
    m_store->remove(sessionIndex, [this, sessionIndex, name](bool ok, const QString &message) {
        if (!ok) {
            setError(message);
            setBusy(false);
            return;
        }
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
        setBusy(false);
        emit reauthenticationRequested(name);
    });
    return true;
}

void RHSessionManager::clearActiveSession()
{
    m_deviceToken.clear();
    m_deviceEmail.clear();
    if (m_activeIndex >= 0) {
        m_activeIndex = -1;
        emit sessionActiveChanged();
    }
    if (!m_accounts.isEmpty()) {
        m_accounts.clear();
        emit accountsChanged();
    }
}
