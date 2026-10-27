#include "rhsessionmanager.h"
#include "rhcredentialstore.h"
#include "rhnetworkengine.h"
#include "rhbrowserlogin.h"
#include "rhnativeauthclient.h"
#include "rhoauthgrant.h"
#include "securecredentiallocker.h"

#include <QDir>
#include <QCryptographicHash>
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
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qint64 maxLifetime = 30LL * 24 * 60 * 60;
bool validQuantity(const QString &quantity, bool *zero = nullptr)
{
    static const QRegularExpression decimal(QStringLiteral("\\A-?[0-9]+(?:\\.[0-9]+)?\\z"));
    if (quantity.size() > 256 || !decimal.match(quantity).hasMatch()) return false;
    if (zero) {
        *zero = true;
        for (const QChar c : quantity) if (c >= QLatin1Char('1') && c <= QLatin1Char('9')) *zero = false;
    }
    return true;
}

bool safeText(const QString &text, int limit = 256)
{
    if (text.isEmpty() || text.size() > limit) return false;
    for (const QChar c : text) if (c.isNull() || c.category() == QChar::Other_Control) return false;
    return true;
}

QString officialAccountUrl(const QString &text)
{
    const QUrl url(text, QUrl::StrictMode);
    static const QRegularExpression path(QStringLiteral("\\A/accounts/[A-Za-z0-9_-]{1,128}/\\z"));
    if (!url.isValid() || url.scheme() != "https" || url.host() != "api.robinhood.com"
        || url.port(443) != 443 || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || url.path(QUrl::FullyEncoded).contains('%') || !path.match(url.path()).hasMatch()) return {};
    return url.toString();
}

QVariantList sanitizedCache(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > 100) return {};
    QVariantList result;
    QSet<QString> keys;
    for (const auto &raw : value.toArray()) {
        if (!raw.isObject()) return {};
        const auto object = raw.toObject();
        const QString key = object.value("_key").toString();
        const QString label = object.value("displayName").toString();
        if (!safeText(key) || keys.contains(key)
            || !QStringList{"Standard Brokerage", "IRA", "Roth IRA", "Account type unavailable"}.contains(label)
            || !safeText(object.value("currentBalance").toString()) || !object.value("holdings").isArray()) return {};
        keys.insert(key);
        QVariantList holdings;
        if (object.value("holdings").toArray().size() > 10000) return {};
        for (const auto &rawHolding : object.value("holdings").toArray()) {
            const auto holding = rawHolding.toObject();
            const QString symbol = holding.value("symbol").toString();
            const QString quantity = holding.value("quantity").toString();
            bool zero = false;
            if (!safeText(symbol) || !validQuantity(quantity, &zero) || zero) return {};
            holdings.append(QVariantMap{{"symbol", symbol}, {"quantity", quantity}});
        }
        const auto updated = object.value("holdingsUpdatedAt").toString();
        if ((!updated.isEmpty() && !QDateTime::fromString(updated, Qt::ISODateWithMs).isValid())
            || (updated.isEmpty() && !holdings.isEmpty())) return {};
        result.append(QVariantMap{{"_key", key}, {"_url", officialAccountUrl(object.value("_url").toString())},
            {"displayName", label}, {"accountNumber", object.value("accountNumber").toString()},
            {"currentBalance", object.value("currentBalance").toString()}, {"holdings", holdings},
            {"holdingsUpdatedAt", updated}, {"holdingsStatus", updated.isEmpty() ? "unavailable" : "cached"},
            {"holdingsError", QString()}});
    }
    return result;
}
QString defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/robinhood/sessions.json");
}

bool readMetadata(const QString &path, QJsonObject &metadata, QString &error, qint64 maximumBytes = -1)
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
    // The async trading reader has a hard 64 MiB ceiling, including a sentinel
    // byte for growth after size(). Never allocate or parse an unbounded document.
    if (maximumBytes >= 0 && file.size() > maximumBytes) {
        error = QStringLiteral("Session metadata exceeds the ownership read limit.");
        return false;
    }
    const auto bytes = maximumBytes < 0 ? file.readAll() : file.read(maximumBytes + 1);
    if (maximumBytes >= 0 && bytes.size() > maximumBytes) {
        error = QStringLiteral("Session metadata exceeds the ownership read limit.");
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const auto invalid = [&error]() {
        error = QStringLiteral("Session metadata is invalid; it was not overwritten.");
        return false;
    };
    if (file.error() != QFileDevice::NoError || parseError.error != QJsonParseError::NoError
        || !document.isObject() || !document.object().value(QStringLiteral("sessions")).isArray())
        return invalid();
    metadata = document.object();
    const auto highWater = metadata.value("credential_revision_high_water");
    bool validHighWater = false;
    highWater.toString().toULongLong(&validHighWater);
    if (!highWater.isUndefined() && (!highWater.isString() || !validHighWater)) return invalid();
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
            || (created.secsTo(expires) > maxLifetime && !item.value("can_refresh").toBool(false))
            || (item.value(QStringLiteral("token_type")).toString().compare(QStringLiteral("Bearer"), Qt::CaseInsensitive) != 0
                && item.value(QStringLiteral("token_type")).toString() != QStringLiteral("WebSecondaryBearer"))
            || (item.value("token_type").toString() == QStringLiteral("WebSecondaryBearer")
                && (item.value("account_id").toString().isEmpty() || created.secsTo(expires) > 86400)))
            return invalid();
        const auto refreshHint = item.value("can_refresh");
        const auto renewalAttempt = item.value("renewal_attempted_at");
        const auto revision = item.value("credential_revision");
        const auto digest = item.value("credential_digest");
        bool validRevision = false;
        const quint64 revisionNumber = revision.toString().toULongLong(&validRevision);
        const auto nativeLabel = item.value("native_label");
        if ((!nativeLabel.isUndefined() && !nativeLabel.isBool())
            || (nativeLabel.toBool() && (item.value("account_id").toString().isEmpty()
                || !refreshHint.toBool() || revisionNumber == 0
                || item.value("account_name").toString() != QStringLiteral("Saved Robinhood account %1").arg(qint64(index) + 1))))
            return invalid();
        static const QRegularExpression digestPattern(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        if ((!revision.isUndefined() && (!revision.isString() || !validRevision))
            || (!digest.isUndefined() && (!digest.isString() || (!digest.toString().isEmpty() && !digestPattern.match(digest.toString()).hasMatch())))
            || (revisionNumber == 0 ? !digest.toString().isEmpty() : digest.toString().isEmpty())
            || (!item.value("renewal_pending").isUndefined() && !item.value("renewal_pending").isBool())
            || (item.value("renewal_pending").toBool() && (revisionNumber == 0
                || !QDateTime::fromString(renewalAttempt.toString(), Qt::ISODateWithMs).isValid())))
            return invalid();
        if ((!refreshHint.isUndefined() && !refreshHint.isBool())
            || (!renewalAttempt.isUndefined() && (!renewalAttempt.isString()
                || (!renewalAttempt.toString().isEmpty()
                    && !QDateTime::fromString(renewalAttempt.toString(), Qt::ISODateWithMs).isValid()))))
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
        const QPointer<RHSessionManager> guard(this);
        if (!m_sessions.isEmpty())
            emit dataChanged(index(0), index(m_sessions.size() - 1), {IsExpiredRole});
        if (guard) checkSessionRenewal();
    });
    m_expiryTimer.start();
    load();
}

RHSessionManager::~RHSessionManager()
{
    // Destruction must cancel owned work without publishing partially destroyed
    // model state to direct observers (including the observer deleting us).
    disconnect(this, nullptr, nullptr, nullptr);
    if (m_nativeClient) {
        disconnect(m_nativeClient, nullptr, this, nullptr);
        m_nativeClient->setGrantHandler({});
    }
    ++m_readGeneration;
    endApproval();
}

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
    case CanRefreshRole: return session.canRefresh;
    default: return {};
    }
}

QHash<int, QByteArray> RHSessionManager::roleNames() const
{
    return {{AccountNameRole, "accountName"}, {SessionIndexRole, "sessionIndex"},
            {ExpiresAtRole, "expiresAt"}, {IsExpiredRole, "isExpired"}, {AccountIdRole, "accountId"}, {CanRefreshRole, "canRefresh"}};
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
    if (!value) releaseCredentialReservation();
    if (!value) m_readingSession = false;
    if (m_busy == value)
        return;
    if (value) ++m_operationGeneration;
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
    if (m_busy || m_sessionCommit || m_readingSession || m_credentialIOPending || m_loadingMetadata)
        return false;
    m_loadingMetadata = true;
    m_loaded = false;
    ++m_labelGeneration;
    ++m_readGeneration;
    const QPointer<RHSessionManager> guard(this);
    const auto labelGeneration = m_labelGeneration;
    const auto loading = qScopeGuard([guard] { if (guard) guard->m_loadingMetadata = false; });
    const auto operationGeneration = m_operationGeneration;
    const auto current = [guard,labelGeneration,operationGeneration] {
        return guard && guard->m_labelGeneration == labelGeneration
            && guard->m_operationGeneration == operationGeneration;
    };
    m_accountCache.clear();
    if (!m_sessions.isEmpty()) {
        beginResetModel();
        if (!current()) return true;
        m_sessions.clear();
        endResetModel();
        if (!current()) return true;
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
                              item.value(QStringLiteral("account_id")).toString(),
                              item.value(QStringLiteral("can_refresh")).toBool(false),
                              QDateTime::fromString(item.value("renewal_attempted_at").toString(), Qt::ISODateWithMs),
                              item.value("credential_revision").toString().toULongLong(),
                              item.value("credential_digest").toString(), item.value("renewal_pending").toBool(), item.value("native_label").toBool()});
        if (parsed.last().nativeLabel) parsed.last().accountName = metadataLabel(parsed.last());
        m_accountCache.insert(item.value("index").toInt(), sanitizedCache(item.value("accounts_cache")));
    }
    const auto token = metadata.value(QStringLiteral("device_token"));
    if (token.isString())
        m_deviceToken = token.toString();
    beginResetModel();
    if (!current()) return true;
    m_sessions = std::move(parsed);
    endResetModel();
    if (!current()) return true;
    m_loaded = true;
    emit accountCacheChanged();
    if (!current()) return true;
    setError({});
    if (!current()) return true;
    hydrateNativeLabels();
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

std::shared_ptr<QLockFile> RHSessionManager::reserveCredentials(QString *failure)
{
    if (m_credentialIOPending) {
        *failure = QStringLiteral("A credential store operation is still pending."); return {};
    }
    if (m_credentialReservation && m_credentialReservation->isLocked())
        return m_credentialReservation;
    if (!QDir().mkpath(QFileInfo(m_metadataPath).absolutePath())) {
        *failure = QStringLiteral("Could not create the session metadata directory."); return {};
    }
    auto reservation = std::make_shared<QLockFile>(m_metadataPath + QStringLiteral(".credentials.lock"));
    reservation->setStaleLockTime(0); // Network/keychain operations have no safe age-based expiry.
    if (!reservation->tryLock(0)) {
        *failure = QStringLiteral("Saved credentials are in use by another operation. Retry after it finishes."); return {};
    }
    m_credentialReservation = reservation;
    return reservation;
}

void RHSessionManager::releaseCredentialReservation()
{
    if (!m_credentialIOPending && m_credentialReservation) {
        m_credentialReservation->unlock();
        m_credentialReservation.reset();
    }
}

QString RHSessionManager::credentialDigest(const QString &secret)
{
    return QString::fromLatin1(QCryptographicHash::hash(secret.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool RHSessionManager::credentialOwnershipCurrent(int slot, QString *failure) const
{
    return credentialOwnershipAtPath(m_metadataPath, find(slot), slot, failure);
}

bool RHSessionManager::credentialOwnershipAtPath(const QString &path, const Session *expected,
                                                int slot, QString *failure, qint64 maximumBytes)
{
    QJsonObject metadata;
    if (!readMetadata(path, metadata, *failure, maximumBytes)) return false;
    for (const auto &value : metadata.value("sessions").toArray()) {
        const auto row = value.toObject();
        if (row.value("index").toInt(-1) != slot) continue;
        if (expected && row.value("account_name").toString() == metadataLabel(*expected)
            && row.value("account_id").toString() == expected->accountId
            && QDateTime::fromString(row.value("created_at").toString(), Qt::ISODateWithMs) == expected->createdAt
            && row.value("credential_revision").toString().toULongLong() == expected->credentialRevision
            && row.value("credential_digest").toString() == expected->credentialDigest
            && QDateTime::fromString(row.value("renewal_attempted_at").toString(), Qt::ISODateWithMs) == expected->renewalAttemptedAt
            && row.value("renewal_pending").toBool() == expected->renewalPending) return true;
        *failure = QStringLiteral("Saved profile changed; reload before accessing credentials."); return false;
    }
    if (!expected) return true;
    *failure = QStringLiteral("Saved profile was deleted; credentials were not accessed or recreated."); return false;
}

quint64 RHSessionManager::allocateCredentialRevision(QString *failure)
{
    // The credential reservation serializes allocation. Persist the high-water
    // with the profile tuple, not before an unrelated keychain read can fail.
    QJsonObject metadata;
    if (!readMetadata(m_metadataPath, metadata, *failure)) return 0;
    bool valid = false;
    const auto raw = metadata.value("credential_revision_high_water");
    quint64 revision = raw.isUndefined() ? 0 : raw.toString().toULongLong(&valid);
    if ((!raw.isUndefined() && !valid) || revision == std::numeric_limits<quint64>::max()) {
        *failure = QStringLiteral("Credential revision is invalid or exhausted."); return 0;
    }
    for (const auto &value : metadata.value("sessions").toArray())
        revision = qMax(revision, value.toObject().value("credential_revision").toString().toULongLong());
    if (revision == std::numeric_limits<quint64>::max()) return 0;
    return revision + 1;
}

bool RHSessionManager::persist(const QList<Session> &sessions, QString *failure)
{
    const auto report = [this, failure](const QString &message) {
        if (failure) *failure = message;
        else setError(message);
    };
    const QFileInfo path(m_metadataPath);
    if (!QDir().mkpath(path.absolutePath())) {
        report(QStringLiteral("Could not create the session metadata directory."));
        return false;
    }
    QLockFile lock(m_metadataPath + QStringLiteral(".lock"));
    if (!lock.tryLock(1000)) {
        report(QStringLiteral("Could not lock session metadata."));
        return false;
    }
    QJsonObject metadata;
    QString error;
    if (!readMetadata(m_metadataPath, metadata, error)) {
        report(error);
        return false;
    }
    if (!metadata.contains(QStringLiteral("device_token")) && !m_deviceToken.isEmpty())
        metadata.insert(QStringLiteral("device_token"), m_deviceToken);
    const auto record = [](const Session &session) {
        return QJsonObject{
            {QStringLiteral("index"), session.index},
            {QStringLiteral("account_name"), metadataLabel(session)},
            {QStringLiteral("native_label"), session.nativeLabel},
            {QStringLiteral("created_at"), session.createdAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("expires_at"), session.expiresAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("token_type"), session.tokenType},
            {QStringLiteral("account_id"), session.accountId},
            {QStringLiteral("can_refresh"), session.canRefresh},
            {QStringLiteral("renewal_attempted_at"), session.renewalAttemptedAt.toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("credential_revision"), QString::number(session.credentialRevision)},
            {QStringLiteral("credential_digest"), session.credentialDigest},
            {QStringLiteral("renewal_pending"), session.renewalPending}
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
                    && item.value(QStringLiteral("account_name")).toString() == metadataLabel(old))
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
        if (session.renewalAttemptedAt.isValid() && old
            && session.renewalAttemptedAt != old->renewalAttemptedAt
            && QDateTime::fromString(item.value("renewal_attempted_at").toString(), Qt::ISODateWithMs) != old->renewalAttemptedAt) {
            report(QStringLiteral("Another OAuth renewal changed this saved profile. Reload before continuing."));
            return false;
        }
        const bool migratingLegacyIdentity = old && old->accountId.isEmpty() && !session.accountId.isEmpty()
            && item.value("account_id").toString().isEmpty()
            && item.value("account_name").toString() == metadataLabel(*old);
        if (row >= 0 && item.value(QStringLiteral("account_name")).toString() != metadataLabel(session)
            && !migratingLegacyIdentity
            && (session.accountId.isEmpty() || item.value("account_id").toString() != session.accountId)) {
            report(QStringLiteral("Saved sessions changed; reload session metadata before retrying."));
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
        report(QStringLiteral("Could not write session metadata."));
        return false;
    }
    metadata.insert(QStringLiteral("sessions"), array);
    quint64 highWater = metadata.value("credential_revision_high_water").toString().toULongLong();
    for (const auto &session : sessions) highWater = qMax(highWater, session.credentialRevision);
    metadata["credential_revision_high_water"] = QString::number(highWater);
    const QByteArray json = QJsonDocument(metadata).toJson();
    if (file.write(json) != json.size() || !file.commit()) {
        report(QStringLiteral("Could not commit session metadata."));
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
    // Admission precedes every notification; a browser continuation is internal,
    // but may never enter an already-owned credential commit.
    if (m_sessionCommit || m_credentialIOPending || (m_readingSession && !continuingBrowser)) return false;
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
    QString reservationError;
    auto reservation = reserveCredentials(&reservationError);
    if (!reservation || !credentialOwnershipCurrent(slot, &reservationError)) {
        releaseCredentialReservation(); setError(reservationError); return false;
    }
    const quint64 revision = allocateCredentialRevision(&reservationError);
    if (!revision) { releaseCredentialReservation(); setError(reservationError); return false; }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const Session session{slot, name, now,
        now.addSecs(qMin(expiresInSeconds, tokenType == QStringLiteral("WebSecondaryBearer") ? 86400LL : maxLifetime)),
        tokenType, accountId, false, {}, revision, credentialDigest(token)};
    const QPointer<RHSessionManager> guard(this);
    m_sessionCommit = true;
    setBusy(true);
    if (!guard) return true;
    setError({});
    if (!guard) return true;
    const auto commit = [this, guard, reservation, session, replacing, token](const QString &previousToken) {
        if (!guard) return;
        QString failure;
        if (!credentialOwnershipCurrent(session.index, &failure)) {
            setError(failure);
            if (guard) { m_sessionCommit = false; setBusy(false); } return;
        }
        m_credentialIOPending = true;
        m_store->write(session.index, token, [this, guard, reservation, session, replacing, previousToken](bool ok, const QString &message) {
        if (!guard)
            return;
        m_credentialIOPending = false;
        if (!ok) {
            m_activateAfterApproval = false;
            setReauthenticating(false);
            if (!guard) return;
            setError(message);
            if (!guard) return;
            m_sessionCommit = false;
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
        QString failure;
        if (!persist(updated, &failure)) {
            m_activateAfterApproval = false;
            setReauthenticating(false);
            if (!guard) return;
            m_credentialIOPending = true;
            const auto rolledBack = [this, guard, reservation, session, failure](bool restored, const QString &) {
                if (!guard)
                    return;
                m_credentialIOPending = false;
                if (!restored) m_unreconciledSessions.insert(session.index);
                setError(restored ? failure : QStringLiteral("Metadata save failed; keychain rollback also failed."));
                if (!guard) return;
                m_sessionCommit = false;
                setBusy(false);
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
            if (!guard) return;
        } else {
            beginInsertRows({}, m_sessions.size(), m_sessions.size());
            if (!guard) return;
            m_sessions.append(session);
            endInsertRows();
            if (!guard) return;
        }
        setReauthenticating(false);
        if (!guard) return;
        const bool activate = std::exchange(m_activateAfterApproval, false);
        emit sessionSaved(session.index);
        if (!guard) return;
        m_sessionCommit = false;
        setBusy(false);
        if (guard && activate && !m_busy) startSession(session.index);
        });
    };
    if (replacing) {
        m_credentialIOPending = true;
        m_store->read(slot, [this, guard, reservation, commit](bool ok, QString previousToken, QString message) {
            if (!guard)
                return;
            m_credentialIOPending = false;
            if (!ok) {
                m_activateAfterApproval = false;
                setReauthenticating(false);
                if (!guard) return;
                setError(message);
                if (!guard) return;
                m_sessionCommit = false;
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
    return initializeSession(sessionIndex);
}

bool RHSessionManager::initializeSession(int sessionIndex)
{
    if (m_sessionCommit || m_credentialIOPending || m_readingSession) return false;
    const Session *session = find(sessionIndex);
    if (m_unreconciledSessions.contains(sessionIndex)) {
        setError(session && session->renewalAttemptedAt.isValid()
            ? QStringLiteral("OAuth renewal needs secure credential/metadata reconciliation. Sign in again; the old refresh token will not be retried.")
            : QStringLiteral("Credential rollback failed. Reauthenticate this saved session before use."));
        return false;
    }
    if (!m_loaded || m_busy || m_tradeLocked || m_sessionCommit || !session
        || (!m_network.isConfigured() && QDateTime::currentDateTimeUtc() >= session->expiresAt)) {
        setError(QStringLiteral("Select an unexpired saved session."));
        return false;
    }
    const QPointer<RHSessionManager> guard(this);
    const quint64 priorGeneration = m_readGeneration;
    clearActiveSession();
    if (!guard || m_readGeneration != priorGeneration + 1 || m_busy) return true;
    const quint64 generation = m_readGeneration;
    QString failure;
    auto reservation = reserveCredentials(&failure);
    if (!reservation || !credentialOwnershipCurrent(sessionIndex, &failure)) {
        releaseCredentialReservation(); setError(failure); return false;
    }
    // Own admission before errorChanged (or any other direct observer) runs.
    m_readingSession = true;
    setBusy(true);
    if (!guard || generation != m_readGeneration) return true;
    setError({});
    if (!guard || generation != m_readGeneration) return true;
    auto cached = m_accountCache.value(sessionIndex);
    for (auto &value : cached) {
        auto row = value.toMap();
        row["holdingsStatus"] = row.value("holdingsUpdatedAt").toString().isEmpty() ? "unavailable" : "cached";
        value = row;
    }
    m_accountCache[sessionIndex] = cached;
    m_accounts.setCachedAccounts(cached);
    if (!guard || generation != m_readGeneration) return true;
    emit accountCacheChanged();
    if (!guard || generation != m_readGeneration) return true;
    m_credentialIOPending = true;
    m_store->read(sessionIndex, [this, guard, reservation, sessionIndex, generation](bool ok, const QString &token, const QString &message) {
        if (!guard) return;
        m_credentialIOPending = false;
        if (generation != m_readGeneration) { releaseCredentialReservation(); return; }
        if (!ok || token.isEmpty()) {
            releaseCredentialReservation();
            setError(message.isEmpty() ? QStringLiteral("Saved session token is unavailable.") : message);
            if (!guard || generation != m_readGeneration) return;
            setBusy(false);
            return;
        }
        const auto *selected = find(sessionIndex);
        if (!selected) { setBusy(false); return; }
        QString failure;
        if (!credentialOwnershipCurrent(sessionIndex, &failure)
            || (!selected->credentialDigest.isEmpty() && credentialDigest(token) != selected->credentialDigest)) {
            releaseCredentialReservation();
            setError(failure.isEmpty() ? QStringLiteral("Saved credential revision does not match; sign in again.") : failure);
            if (guard && generation == m_readGeneration) setBusy(false);
            return;
        }
        RHOAuthGrant grant;
        if (RHOAuthGrant::deserialize(token, &grant)) {
            if (selected->tokenType != QStringLiteral("Bearer") || !grant.primary()
                || grant.accountId.isEmpty() || (!selected->accountId.isEmpty() && grant.accountId != selected->accountId)) {
                setError(QStringLiteral("Saved OAuth identity is inconsistent; sign in again."));
                if (!guard || generation != m_readGeneration) return;
                setBusy(false); return;
            }
            renewOAuthGrant(sessionIndex, std::move(grant), credentialDigest(token));
            return;
        }
        if (QDateTime::currentDateTimeUtc() >= selected->expiresAt) {
            setError(QStringLiteral("This session has expired; reauthenticate."));
            if (!guard || generation != m_readGeneration) return;
            setBusy(false);
            return;
        }
        QByteArray access;
        if (!RHOAuthGrant::extractAccessToken(token, &access)) {
            setError(QStringLiteral("Saved credential is invalid; sign in again."));
            if (!guard || generation != m_readGeneration) return;
            setBusy(false); return;
        }
        verifySession(sessionIndex, QString::fromLatin1(access));
    });
    return true;
}

void RHSessionManager::verifySession(int sessionIndex, const QString &token)
{
    const quint64 generation = m_readGeneration;
    const QPointer<RHSessionManager> guard(this);
    QString problem;
    const Session *session = find(sessionIndex);
    if (!session) return;
    QNetworkReply *reply = m_network.sendAuthenticated(QStringLiteral("Accounts"), token.toLatin1(),
        session->tokenType == QStringLiteral("WebSecondaryBearer")
            ? RHNetworkEngine::AuthKind::WebSecondaryBearer : RHNetworkEngine::AuthKind::OAuthBearer, &problem);
    if (!reply) {
        setError(problem);
        if (!guard || generation != m_readGeneration) return;
        setBusy(false);
        return;
    }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, guard, reply, sessionIndex, token, generation]() {
        if (generation != m_readGeneration) { reply->deleteLater(); return; }
        m_reply = nullptr;
        const bool transportOk = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 200
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() < 300;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        const QJsonValue results = document.object().value(QStringLiteral("results"));
        const auto next = document.object().value("next");
        if (!transportOk || !results.isArray() || results.toArray().size() > 100
            || (!next.isNull() && !next.isUndefined() && next != QJsonValue(QString()))) {
            setError(QStringLiteral("Could not verify the saved session. Reauthenticate if it was revoked."));
            if (!guard || generation != m_readGeneration) return;
            setBusy(false);
            return;
        }
        RHAccountModel parsed;
        parsed.setAccounts(results.toArray());
        QVariantList rows;
        QSet<QString> keys;
        for (int i = 0; i < results.toArray().size(); ++i) {
            const auto raw = results.toArray().at(i).toObject();
            QVariantMap row;
            const auto roles = parsed.roleNames();
            for (auto it = roles.begin(); it != roles.end(); ++it)
                row.insert(QString::fromLatin1(it.value()), parsed.data(parsed.index(i), it.key()));
            QString key = row.value("accountNumber").toString();
            if (key.isEmpty()) key = raw.value("id").toString();
            if (key.isEmpty()) key = officialAccountUrl(raw.value("url").toString());
            if (!safeText(key) || keys.contains(key)) {
                setError(QStringLiteral("Account identity is unavailable or ambiguous."));
                if (!guard || generation != m_readGeneration) return;
                setBusy(false); return;
            }
            keys.insert(key);
            row["_key"] = key; row["_url"] = officialAccountUrl(raw.value("url").toString());
            for (const auto &cached : m_accountCache.value(sessionIndex)) {
                const auto previous = cached.toMap();
                if (previous.value("_key") != key) continue;
                for (const char *field : {"holdings", "holdingsUpdatedAt", "holdingsStatus", "holdingsError"})
                    row[QLatin1String(field)] = previous.value(QLatin1String(field));
                row["holdingsStatus"] = row.value("holdingsUpdatedAt").toString().isEmpty() ? "unavailable" : "cached";
            }
            rows.append(row);
        }
        const bool committed = commitAccountCache(sessionIndex, rows);
        if (!guard || generation != m_readGeneration) return;
        if (!committed) { setBusy(false); return; }
        m_accounts.setCachedAccounts(rows);
        if (!guard || generation != m_readGeneration) return;
        m_activeIndex = sessionIndex;
        emit sessionActiveChanged();
        if (!guard || generation != m_readGeneration) return;
        refreshHoldings(sessionIndex, token.toLatin1(), generation);
    });
}

QVariantList RHSessionManager::cachedAccounts(int sessionIndex) const
{
    if (!find(sessionIndex)) return {};
    QVariantList result;
    for (const auto &value : m_accountCache.value(sessionIndex)) {
        auto row = value.toMap(); row.remove("_key"); row.remove("_url"); result.append(row);
    }
    return result;
}

bool RHSessionManager::commitAccountCache(int sessionIndex, const QVariantList &accounts)
{
    const Session *session = find(sessionIndex);
    if (!session || sanitizedCache(QJsonArray::fromVariantList(accounts)).size() != accounts.size()) return false;
    QLockFile lock(m_metadataPath + QStringLiteral(".lock"));
    if (!lock.tryLock(1000)) { setError(QStringLiteral("Could not lock account cache.")); return false; }
    QJsonObject metadata; QString error;
    if (!readMetadata(m_metadataPath, metadata, error)) { setError(error); return false; }
    auto records = metadata.value("sessions").toArray();
    bool matched = false;
    for (qsizetype i = 0; i < records.size(); ++i) {
        auto record = records[i].toObject();
        if (record.value("index").toInt(-1) != sessionIndex) continue;
        if (record.value("account_name").toString() != metadataLabel(*session)
            || record.value("account_id").toString() != session->accountId
            || QDateTime::fromString(record.value("created_at").toString(), Qt::ISODateWithMs) != session->createdAt) {
            setError(QStringLiteral("Saved profile changed; account cache was not overwritten.")); return false;
        }
        const auto diskCache = sanitizedCache(record.value("accounts_cache"));
        const auto expectedCache = sanitizedCache(QJsonArray::fromVariantList(m_accountCache.value(sessionIndex)));
        if (diskCache != expectedCache) {
            setError(QStringLiteral("Account cache changed; reload before refreshing to avoid overwriting a newer snapshot.")); return false;
        }
        // Persist only the sanitized schema; never tokens or raw brokerage responses.
        auto sanitized = sanitizedCache(QJsonArray::fromVariantList(accounts));
        record["accounts_cache"] = QJsonArray::fromVariantList(sanitized);
        records[i] = record; matched = true; break;
    }
    if (!matched) { setError(QStringLiteral("Saved profile was deleted; account cache was not recreated.")); return false; }
    metadata["sessions"] = records;
    QSaveFile file(m_metadataPath);
    const auto bytes = QJsonDocument(metadata).toJson();
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        setError(QStringLiteral("Could not commit account cache; previous snapshot retained.")); return false;
    }
    const QPointer<RHSessionManager> guard(this);
    m_accountCache[sessionIndex] = accounts;
    if (m_activeIndex == sessionIndex) m_accounts.setCachedAccounts(accounts);
    if (!guard) return true;
    emit accountCacheChanged(); return true;
}

void RHSessionManager::refreshHoldings(int sessionIndex, QByteArray token, quint64 generation, int row)
{
    if (generation != m_readGeneration || !find(sessionIndex)) return;
    if (row >= m_accountCache.value(sessionIndex).size()) { setBusy(false); return; }
    readAccountPage(sessionIndex, token, generation, row, {}, {}, {});
}

void RHSessionManager::holdingsFailed(int sessionIndex, const QByteArray &token, quint64 generation, int row)
{
    if (generation != m_readGeneration || !find(sessionIndex)) return;
    auto rows = m_accountCache.value(sessionIndex);
    auto account = rows[row].toMap();
    account["holdingsStatus"] = account.value("holdingsUpdatedAt").toString().isEmpty() ? "unavailable" : "cached";
    account["holdingsError"] = QStringLiteral("Holdings refresh unavailable; last successful snapshot retained.");
    rows[row] = account;
    // Failure state is transient; the last successful disk snapshot is untouched.
    const QPointer<RHSessionManager> guard(this);
    m_accountCache[sessionIndex] = rows; m_accounts.setCachedAccounts(rows);
    if (!guard || generation != m_readGeneration) return;
    emit accountCacheChanged();
    if (!guard || generation != m_readGeneration) return;
    refreshHoldings(sessionIndex, token, generation, row + 1);
}

void RHSessionManager::readAccountPage(int sessionIndex, QByteArray token, quint64 generation,
    int row, QUrl location, QJsonArray positions, QSet<QString> visited)
{
    if (generation != m_readGeneration || !find(sessionIndex)) return;
    const auto account = m_accountCache.value(sessionIndex).at(row).toMap();
    const QString number = account.value("accountNumber").toString();
    if (visited.size() >= 100 || visited.contains(location.toString())) {
        holdingsFailed(sessionIndex, token, generation, row); return;
    }
    visited.insert(location.toString());
    const auto kind = find(sessionIndex)->tokenType == "WebSecondaryBearer"
        ? RHNetworkEngine::AuthKind::WebSecondaryBearer : RHNetworkEngine::AuthKind::OAuthBearer;
    auto *reply = m_network.sendAuthenticated("CachedPositions", token, kind, nullptr, location, number);
    if (!reply) { holdingsFailed(sessionIndex, token, generation, row); return; }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, sessionIndex, token, generation, row, positions, visited, account]() mutable {
        if (generation != m_readGeneration) { reply->deleteLater(); return; }
        m_reply = nullptr;
        const auto bytes = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && bytes.size() <= 8 * 1024 * 1024;
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(bytes, &error); reply->deleteLater();
        const auto results = doc.object().value("results");
        const auto fail = [&] { holdingsFailed(sessionIndex, token, generation, row); };
        if (!ok || error.error != QJsonParseError::NoError || !results.isArray()
            || positions.size() + results.toArray().size() > 10000) { fail(); return; }
        for (const auto &raw : results.toArray()) {
            if (!raw.isObject()) { fail(); return; }
            const auto position = raw.toObject();
            const QString positionNumber = position.value("account_number").toString();
            const QString positionAccount = position.value("account").toString();
            if ((!positionNumber.isEmpty() && positionNumber != account.value("accountNumber").toString())
                || (!positionAccount.isEmpty() && positionAccount != account.value("_url").toString())
                || (positionNumber.isEmpty() && positionAccount.isEmpty())
                || !position.value("quantity").isString() || !validQuantity(position.value("quantity").toString())) { fail(); return; }
            positions.append(position);
        }
        const auto next = doc.object().value("next");
        // Completion must be explicit; a truncated/envelope-incomplete response cannot clear a snapshot.
        if (next.isUndefined()) { fail(); return; }
        if (next.isString() && !next.toString().isEmpty()) {
            readAccountPage(sessionIndex, token, generation, row, QUrl(next.toString(), QUrl::StrictMode), positions, visited); return;
        }
        if (!next.isNull() && !next.isUndefined() && !next.isString()) { fail(); return; }
        QSet<QString> identities;
        for (const auto &raw : positions) {
            const auto position = raw.toObject(); bool zero = false;
            validQuantity(position.value("quantity").toString(), &zero);
            if (zero) continue;
            QString identity = position.value("instrument").toString();
            if (identity.isEmpty()) identity = position.value("symbol").toString();
            if (identity.isEmpty() || identities.contains(identity)) { fail(); return; }
            identities.insert(identity);
        }
        resolveHoldings(sessionIndex, token, generation, row, positions, 0, {}, 0);
    });
}

void RHSessionManager::resolveHoldings(int sessionIndex, QByteArray token, quint64 generation,
    int row, QJsonArray positions, int cursor, QVariantList holdings, int lookups)
{
    if (generation != m_readGeneration || !find(sessionIndex)) return;
    while (cursor < positions.size()) {
        const auto position = positions.at(cursor).toObject();
        const QString quantity = position.value("quantity").toString(); bool zero = false;
        if (!validQuantity(quantity, &zero)) { holdingsFailed(sessionIndex, token, generation, row); return; }
        ++cursor;
        if (zero) continue;
        const QString symbol = position.value("symbol").toString();
        if (safeText(symbol, 64)) {
            holdings.append(QVariantMap{{"symbol", symbol}, {"quantity", quantity}}); continue;
        }
        const QUrl instrument(position.value("instrument").toString(), QUrl::StrictMode);
        static const QRegularExpression path(QStringLiteral("\\A/instruments/([A-Za-z0-9_-]{1,128})/\\z"));
        const auto match = path.match(instrument.path());
        if (!instrument.isValid() || !match.hasMatch() || instrument.hasQuery() || instrument.hasFragment()
            || (!instrument.isRelative() && (instrument.scheme() != "https" || instrument.host() != "api.robinhood.com"
                || !instrument.userInfo().isEmpty() || instrument.port(443) != 443))) {
            holdingsFailed(sessionIndex, token, generation, row); return;
        }
        const QString fallback = QStringLiteral("Instrument ID: ") + match.captured(1);
        if (lookups >= 200) { holdings.append(QVariantMap{{"symbol", fallback}, {"quantity", quantity}}); continue; }
        const auto kind = find(sessionIndex)->tokenType == "WebSecondaryBearer"
            ? RHNetworkEngine::AuthKind::WebSecondaryBearer : RHNetworkEngine::AuthKind::OAuthBearer;
        auto *reply = m_network.sendAuthenticated("Instruments", token, kind, nullptr, instrument);
        if (!reply) { holdings.append(QVariantMap{{"symbol", fallback}, {"quantity", quantity}}); continue; }
        m_reply = reply;
        connect(reply, &QNetworkReply::finished, this,
            [this, reply, sessionIndex, token, generation, row, positions, cursor, holdings, lookups, quantity, fallback]() mutable {
            if (generation != m_readGeneration) { reply->deleteLater(); return; }
            m_reply = nullptr;
            QString symbol;
            const auto bytes = reply->readAll();
            if (reply->error() == QNetworkReply::NoError
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && bytes.size() <= 1024 * 1024)
                symbol = QJsonDocument::fromJson(bytes).object().value("symbol").toString();
            reply->deleteLater();
            holdings.append(QVariantMap{{"symbol", safeText(symbol, 64) ? symbol : fallback}, {"quantity", quantity}});
            resolveHoldings(sessionIndex, token, generation, row, positions, cursor, holdings, lookups + 1);
        });
        return;
    }
    auto rows = m_accountCache.value(sessionIndex); auto account = rows[row].toMap();
    account["holdings"] = holdings;
    account["holdingsUpdatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    account["holdingsStatus"] = QStringLiteral("fresh"); account["holdingsError"] = QString(); rows[row] = account;
    const QPointer<RHSessionManager> guard(this);
    const bool committed = commitAccountCache(sessionIndex, rows);
    if (!guard || generation != m_readGeneration) return;
    if (!committed) { holdingsFailed(sessionIndex, token, generation, row); return; }
    refreshHoldings(sessionIndex, token, generation, row + 1);
}

bool RHSessionManager::reauthenticate(int sessionIndex)
{
    const Session *session = find(sessionIndex);
    if (!session || m_busy || !m_loaded || !m_network.isConfigured()) {
        setError(QStringLiteral("Select a saved session after loading network configuration."));
        return false;
    }
    const QString target = session->accountId.isEmpty() ? session->accountName : session->accountId;
    return m_nativeClient ? beginNativeLogin(target) : beginBrowserLogin(target);
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
    if (m_nativeClient) beginNativeLogin(accountId);
    else beginBrowserLogin(accountId);
}

bool RHSessionManager::deleteSavedSession(int sessionIndex)
{
    return removeSession(sessionIndex);
}

bool RHSessionManager::removeSession(int sessionIndex)
{
    if (m_sessionCommit || m_readingSession || m_credentialIOPending) return false;
    const Session *session = find(sessionIndex);
    if (m_busy || !m_loaded || !session) {
        setError(QStringLiteral("Select a saved session to delete."));
        return false;
    }
    QString failure;
    auto reservation = reserveCredentials(&failure);
    if (!reservation || !credentialOwnershipCurrent(sessionIndex, &failure)) {
        releaseCredentialReservation(); setError(failure); return false;
    }
    const QString name = session->accountName;
    const QPointer<RHSessionManager> guard(this);
    m_sessionCommit = true;
    setBusy(true);
    if (!guard) return true;
    setError({});
    if (!guard) return true;
    m_credentialIOPending = true;
    m_store->remove(sessionIndex, [this, guard, reservation, sessionIndex, name](bool ok, const QString &message) {
        if (!guard)
            return;
        m_credentialIOPending = false;
        if (!ok) {
            setError(message);
            if (!guard) return;
            m_sessionCommit = false;
            setBusy(false);
            return;
        }
        m_credentialIOPending = true;
        m_locker->remove(name, [this, guard, reservation, sessionIndex](bool removed, QString error) {
        if (!guard)
            return;
        m_credentialIOPending = false;
        if (!removed) {
            setError(error);
            if (!guard) return;
            m_sessionCommit = false;
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
        QString failure;
        if (!persist(updated, &failure)) {
            setError(failure);
            if (!guard) return;
            m_sessionCommit = false;
            setBusy(false);
            return;
        }
        for (int row = 0; row < m_sessions.size(); ++row) {
            if (m_sessions.at(row).index == sessionIndex) {
                beginRemoveRows({}, row, row);
                if (!guard) return;
                m_sessions.removeAt(row);
                endRemoveRows();
                if (!guard) return;
                break;
            }
        }
        setError({});
        if (!guard) return;
        m_accountCache.remove(sessionIndex);
        emit accountCacheChanged();
        if (!guard) return;
        emit sessionDeleted(sessionIndex);
        if (!guard) return;
        m_sessionCommit = false;
        if (sessionIndex == m_activeIndex) clearActiveSession();
        else setBusy(false);
        });
    });
    return true;
}

void RHSessionManager::clearActiveSession()
{
    if (m_tradeLocked || m_sessionCommit)
        return;
    if (m_nativeLoginPending) { cancelSignIn(); return; }
    const QPointer<RHSessionManager> guard(this);
    if (m_oauthPending) {
        endApproval();
        if (!guard) return;
    }
    const quint64 generation = ++m_readGeneration;
    if (m_reply) {
        auto reply = m_reply; m_reply = nullptr;
        reply->abort();
        if (reply) reply->deleteLater();
        if (!guard || generation != m_readGeneration) return;
    }
    const bool wasActive = m_activeIndex >= 0;
    m_activeIndex = -1;
    m_accounts.clear();
    if (!guard || generation != m_readGeneration) return;
    if (wasActive) {
        emit sessionActiveChanged();
        if (!guard || generation != m_readGeneration) return;
    }
    // Direct idle handlers may select another profile. No old-state writes may follow.
    setBusy(false);
}
