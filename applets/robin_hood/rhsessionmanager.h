#pragma once

#include "securecredentiallocker.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QUrl>
#include <QVariantList>
#include <functional>
#include <memory>
#include <utility>

class SecureCredentialLocker;
class RHCredentialStore;
class RHNetworkEngine;
class RHBrowserLogin;
class RHNativeAuthClient;
class QNetworkReply;
class QLockFile;
struct RHOAuthGrant;

class RHAccountModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { DisplayNameRole = Qt::UserRole + 1, AccountNumberRole, CurrentBalanceRole,
                HoldingsRole, HoldingsUpdatedAtRole, HoldingsStatusRole, HoldingsErrorRole };
    Q_ENUM(Role)
    explicit RHAccountModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setAccounts(const QJsonArray &results);
    void setCachedAccounts(const QVariantList &accounts);
    void clear();

private:
    struct Account {
        QString displayName;
        QString accountNumber;
        QString currentBalance;
        QVariantList holdings;
        QString holdingsUpdatedAt;
        QString holdingsStatus = QStringLiteral("unavailable");
        QString holdingsError;
    };
    QList<Account> m_items;
};

class RHSessionManager final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool browserLoginPending READ browserLoginPending NOTIFY browserLoginPendingChanged FINAL)
    Q_PROPERTY(bool nativeLoginPending READ nativeLoginPending NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(bool nativeCodeRequired READ nativeCodeRequired NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(bool nativeLoginWorking READ nativeLoginWorking NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(bool nativeLoginCancellable READ nativeLoginCancellable NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(QString nativeLoginStatus READ nativeLoginStatus NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(QString nativeLoginEmail READ nativeLoginEmail NOTIFY nativeLoginChanged FINAL)
    Q_PROPERTY(bool reauthenticating READ reauthenticating NOTIFY reauthenticatingChanged FINAL)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QString activeAccount READ activeAccount NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QAbstractItemModel* accounts READ accounts CONSTANT FINAL)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged FINAL)
    Q_PROPERTY(bool approvalPending READ approvalPending NOTIFY approvalChanged FINAL)
    Q_PROPERTY(int approvalSecondsRemaining READ approvalSecondsRemaining NOTIFY approvalChanged FINAL)
public:
    enum Role { AccountNameRole = Qt::UserRole + 1, SessionIndexRole, ExpiresAtRole, IsExpiredRole, AccountIdRole, CanRefreshRole };
    Q_ENUM(Role)

    explicit RHSessionManager(RHNetworkEngine &network, QString metadataPath = {},
                              RHCredentialStore *store = nullptr, QObject *parent = nullptr,
                              int approvalTimeoutMs = 180000, int approvalPollMs = 2500,
                              SecureCredentialLocker *locker = nullptr);
    ~RHSessionManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool busy() const { return m_busy; }
    bool browserLoginPending() const { return m_browserLoginPending; }
    void setBrowserLogin(RHBrowserLogin *browser);
    // Nullable, borrowed client; must be used on this QObject thread.
    void setNativeAuthClient(RHNativeAuthClient *client);
    Q_INVOKABLE bool beginNativeLogin(const QString &accountId = {});
    Q_INVOKABLE void submitNativeVerificationCode(const QString &code);
    bool nativeLoginPending() const { return m_nativeLoginPending; }
    bool nativeCodeRequired() const;
    bool nativeLoginWorking() const { return m_nativeLoginPending && m_nativeRequestStarted && !nativeCodeRequired(); }
    bool nativeLoginCancellable() const { return m_nativeLoginPending && !m_sessionCommit; }
    QString nativeLoginStatus() const { return m_nativeLoginStatus; }
    QString nativeLoginEmail() const { return m_nativeLoginEmail; }
    void acceptBrowserToken(QByteArray token, QDateTime expiresAt);
    void acceptPrimaryBrowserToken(QByteArray token);
    bool acceptOAuthGrant(const QJsonObject &response, const QString &clientId, const QString &deviceToken);
    // C++ timer/resume hook; never reads or renews a non-active saved profile.
    void checkSessionRenewal();

    // C++-only lease; no token, callback or session snapshot enters Qt's meta-object API.
    struct TradingSession {
        int sessionIndex = -1;
        quint64 generation = 0;
        QString profileId;
        QByteArray token;
        QDateTime expiresAt;
        QVariantList accounts;
    };
    using TradingCompletion = std::function<void(bool, TradingSession, QString)>;
    QString tradingUnavailableReason() const;
    void requestTradingSession(TradingCompletion done);
    bool isTradingSessionCurrent(const TradingSession &session) const;
    // Disk-free, QObject-thread-only identity/expiry check.
    bool isTradingSessionLocallyCurrent(const TradingSession &session) const;
    void checkTradingOwnershipAsync(TradingSession session, std::function<void(bool)> done);
    // C++-only deterministic worker gate/budget. Hook runs on the worker and must
    // own its captures; it must never access a QObject. Production leaves it empty.
    void setTradingOwnershipReaderForTests(std::function<void()> beforeRead,
                                          qint64 maximumBytes = 64 * 1024 * 1024,
                                          std::function<void()> afterRead = {}) {
        m_ownershipBeforeRead = std::move(beforeRead);
        m_ownershipAfterRead = std::move(afterRead);
        m_ownershipMaximumBytes = qBound(qint64(1), maximumBytes, qint64(64 * 1024 * 1024));
    }
    void releaseTradingSession();
    Q_INVOKABLE bool beginBrowserLogin(const QString &accountId = {});
    bool reauthenticating() const { return m_reauthenticating; }
    bool sessionActive() const { return m_activeIndex >= 0; }
    QString activeAccount() const;
    QAbstractItemModel *accounts() { return &m_accounts; }
    QString error() const { return m_error; }
    bool approvalPending() const { return m_approvalPending; }
    int approvalSecondsRemaining() const { return m_approvalSecondsRemaining; }

    bool load();
    // Called only after a successful login; tokens never cross a QML model role.
    bool saveAuthenticatedSession(const QString &accountName, const QString &token,
                                  qint64 expiresInSeconds, const QString &tokenType = QStringLiteral("Bearer"));
    Q_INVOKABLE bool signIn(const QString &email, const QString &password, const QString &mfaCode = {});
    Q_INVOKABLE bool startSession(int index);
    Q_INVOKABLE bool initializeSession(int index);
    Q_INVOKABLE QVariantList cachedAccounts(int sessionIndex) const;
    Q_INVOKABLE bool reauthenticate(int index);
    Q_INVOKABLE void prepareReauthentication(const QString &accountId);
    Q_INVOKABLE bool deleteSavedSession(int index);
    Q_INVOKABLE void clearActiveSession();
    Q_INVOKABLE void cancelSignIn();

signals:
    void busyChanged();
    void browserLoginPendingChanged();
    void nativeLoginChanged();
    void reauthenticatingChanged();
    void sessionActiveChanged();
    void errorChanged();
    void approvalChanged();
    void sessionSaved(int index);
    void accountCacheChanged();
    void sessionDeleted(int index);
    void reauthenticationRequested(const QString &accountName);

private:
    struct Session {
        int index;
        QString accountName;
        QDateTime createdAt;
        QDateTime expiresAt;
        QString tokenType;
        QString accountId;
        bool canRefresh = false;
        QDateTime renewalAttemptedAt;
        quint64 credentialRevision = 0;
        QString credentialDigest;
        bool renewalPending = false;
        bool nativeLabel = false;
    };
    static QString metadataLabel(const Session &session);
    void hydrateNativeLabels();
    void finishNativeLogin();
    const Session *find(int index) const;
    bool persist(const QList<Session> &sessions, QString *failure = nullptr);
    std::shared_ptr<QLockFile> reserveCredentials(QString *failure);
    bool credentialOwnershipCurrent(int slot, QString *failure) const;
    static bool credentialOwnershipAtPath(const QString &path, const Session *expected,
                                          int slot, QString *failure, qint64 maximumBytes = -1);
    quint64 allocateCredentialRevision(QString *failure);
    static QString credentialDigest(const QString &secret);
    void releaseCredentialReservation();
    bool ensureDeviceToken();
    bool saveSession(const QString &accountName, const QString &token, qint64 seconds,
                     const QString &tokenType, const QString &accountId = {}, bool continuingBrowser = false);
    void setBrowserLoginPending(bool pending);
    void acceptBrowserTokenImpl(QByteArray token, QDateTime expiresAt, const QString &tokenType);
    void readBrowserEndpoint(const QString &endpoint, const QByteArray &token,
                             const QString &tokenType, quint64 generation,
                             std::function<void(QJsonObject)> done);
    void commitBrowserSession(const QString &label, const QString &id, const QByteArray &token,
                              const QString &tokenType, QDateTime expiry, quint64 generation,
                              const QString &secret = {}, bool canRefresh = false, bool activate = false);
    void validateOAuthGrant(RHOAuthGrant grant, quint64 generation, bool activate);
    void renewOAuthGrant(int sessionIndex, RHOAuthGrant grant, const QString &secretDigest);
    bool removeSession(int index);
    void setBusy(bool value);
    void setError(const QString &message);
    void verifySession(int index, const QString &token);
    bool commitAccountCache(int index, const QVariantList &accounts);
    void refreshHoldings(int index, QByteArray token, quint64 generation, int row = 0);
    void readAccountPage(int index, QByteArray token, quint64 generation, int row,
                         QUrl location, QJsonArray positions, QSet<QString> visited);
    void resolveHoldings(int index, QByteArray token, quint64 generation, int row,
                         QJsonArray positions, int cursor, QVariantList holdings, int lookups);
    void holdingsFailed(int index, const QByteArray &token, quint64 generation, int row);
    void setReauthenticating(bool value);
    void failLogin(const QString &message);
    void endApproval();
    enum class ApprovalStep { Inquiry, Prompt, Continue };

    QPointer<RHBrowserLogin> m_browser;
    QPointer<RHNativeAuthClient> m_nativeClient;
    bool m_nativeLoginPending = false;
    bool m_nativeRequestStarted = false;
    QString m_nativeLoginStatus, m_nativeLoginEmail;
    SecureCredentialLocker::PendingCredentials m_nativeCredentials;
    quint64 m_labelGeneration = 0;
    bool m_browserLoginPending = false;
    bool m_browserVerifying = false;
    // Held through credential reconciliation and balanced model publication.
    bool m_sessionCommit = false;
    bool m_oauthPending = false;
    // Callbacks co-own the file reservation while an external store can finish.
    std::shared_ptr<QLockFile> m_credentialReservation;
    bool m_credentialIOPending = false;
    QSet<int> m_unreconciledSessions;
    QString m_browserTarget;
    quint64 m_tradeGeneration = 0;
    bool m_tradeLocked = false;
    bool m_ownershipInFlight = false;
    std::function<void()> m_ownershipBeforeRead;
    std::function<void()> m_ownershipAfterRead;
    qint64 m_ownershipMaximumBytes = 64 * 1024 * 1024;
    RHNetworkEngine &m_network;
    QString m_metadataPath;
    RHCredentialStore *m_store;
    std::unique_ptr<RHCredentialStore> m_ownedStore;
    SecureCredentialLocker *m_locker;
    std::unique_ptr<SecureCredentialLocker> m_ownedLocker;
    QList<Session> m_sessions;
    QHash<int, QVariantList> m_accountCache;
    quint64 m_readGeneration = 0;
    // Advances on every admitted busy operation, including synchronous saves.
    quint64 m_operationGeneration = 0;
    bool m_readingSession = false;
    QTimer m_expiryTimer;
    QTimer m_pollTimer;
    QTimer m_approvalTimeout;
    QTimer m_countdownTimer;
    QElapsedTimer m_approvalClock;
    QPointer<QNetworkReply> m_reply;
    RHAccountModel m_accounts;
    QString m_error;
    QString m_deviceToken;
    QString m_loginName;
    QString m_authDiagnostic;
    QVariantMap m_loginBody;
    SecureCredentialLocker::PendingCredentials m_pendingCredentials;
    QString m_machineId;
    QString m_challengeId;
    ApprovalStep m_nextPoll = ApprovalStep::Inquiry;
    quint64 m_authGeneration = 0;
    int m_approvalTimeoutMs;
    int m_approvalPollMs;
    int m_approvalSecondsRemaining = 0;
    int m_activeIndex = -1;
    bool m_approvalPending = false;
    bool m_activateAfterApproval = false;
    bool m_busy = false;
    bool m_reauthenticating = false;
    bool m_loaded = false;
    bool m_loadingMetadata = false;
};
