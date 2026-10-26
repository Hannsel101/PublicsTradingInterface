#pragma once

#include "securecredentiallocker.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>

class SecureCredentialLocker;
class RHCredentialStore;
class RHNetworkEngine;
class RHBrowserLogin;
class QNetworkReply;

class RHAccountModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { DisplayNameRole = Qt::UserRole + 1, AccountNumberRole, CurrentBalanceRole };
    Q_ENUM(Role)
    explicit RHAccountModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setAccounts(const QJsonArray &results);
    void clear();

private:
    struct Account {
        QString displayName;
        QString accountNumber;
        QString currentBalance;
    };
    QList<Account> m_items;
};

class RHSessionManager final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool browserLoginPending READ browserLoginPending NOTIFY browserLoginPendingChanged FINAL)
    Q_PROPERTY(bool reauthenticating READ reauthenticating NOTIFY reauthenticatingChanged FINAL)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QString activeAccount READ activeAccount NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QAbstractItemModel* accounts READ accounts CONSTANT FINAL)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged FINAL)
    Q_PROPERTY(bool approvalPending READ approvalPending NOTIFY approvalChanged FINAL)
    Q_PROPERTY(int approvalSecondsRemaining READ approvalSecondsRemaining NOTIFY approvalChanged FINAL)
public:
    enum Role { AccountNameRole = Qt::UserRole + 1, SessionIndexRole, ExpiresAtRole, IsExpiredRole, AccountIdRole };
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
    void acceptBrowserToken(QByteArray token, QDateTime expiresAt);
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
    Q_INVOKABLE bool reauthenticate(int index);
    Q_INVOKABLE void prepareReauthentication(const QString &accountId);
    Q_INVOKABLE bool deleteSavedSession(int index);
    Q_INVOKABLE void clearActiveSession();
    Q_INVOKABLE void cancelSignIn();

signals:
    void busyChanged();
    void browserLoginPendingChanged();
    void reauthenticatingChanged();
    void sessionActiveChanged();
    void errorChanged();
    void approvalChanged();
    void sessionSaved(int index);
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
    };
    const Session *find(int index) const;
    bool persist(const QList<Session> &sessions);
    bool ensureDeviceToken();
    bool saveSession(const QString &accountName, const QString &token, qint64 seconds,
                     const QString &tokenType, const QString &accountId = {}, bool continuingBrowser = false);
    void setBrowserLoginPending(bool pending);
    void readBrowserEndpoint(const QString &endpoint, const QByteArray &token,
                             std::function<void(QJsonObject)> done);
    bool removeSession(int index);
    void setBusy(bool value);
    void setError(const QString &message);
    void verifySession(int index, const QString &token);
    void setReauthenticating(bool value);
    void failLogin(const QString &message);
    void endApproval();
    enum class ApprovalStep { Inquiry, Prompt, Continue };

    QPointer<RHBrowserLogin> m_browser;
    bool m_browserLoginPending = false;
    bool m_browserVerifying = false;
    QString m_browserTarget;
    QDateTime m_browserExpiry;
    RHNetworkEngine &m_network;
    QString m_metadataPath;
    RHCredentialStore *m_store;
    std::unique_ptr<RHCredentialStore> m_ownedStore;
    SecureCredentialLocker *m_locker;
    std::unique_ptr<SecureCredentialLocker> m_ownedLocker;
    QList<Session> m_sessions;
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
};
