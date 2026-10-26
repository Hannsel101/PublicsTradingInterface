#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>

class RHCredentialStore;
class RHNetworkEngine;
class QNetworkReply;

class RHSessionManager final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)
    Q_PROPERTY(bool sessionActive READ sessionActive NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QString activeAccount READ activeAccount NOTIFY sessionActiveChanged FINAL)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged FINAL)
    Q_PROPERTY(bool approvalPending READ approvalPending NOTIFY approvalChanged FINAL)
    Q_PROPERTY(int approvalSecondsRemaining READ approvalSecondsRemaining NOTIFY approvalChanged FINAL)
public:
    enum Role { AccountNameRole = Qt::UserRole + 1, SessionIndexRole, ExpiresAtRole, IsExpiredRole };
    Q_ENUM(Role)

    explicit RHSessionManager(RHNetworkEngine &network, QString metadataPath = {},
                              RHCredentialStore *store = nullptr, QObject *parent = nullptr,
                              int approvalTimeoutMs = 180000, int approvalPollMs = 2500);
    ~RHSessionManager() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool busy() const { return m_busy; }
    bool sessionActive() const { return m_activeIndex >= 0; }
    QString activeAccount() const;
    QVariantList accounts() const { return m_accounts; }
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
    Q_INVOKABLE void clearActiveSession();
    Q_INVOKABLE void cancelSignIn();

signals:
    void busyChanged();
    void sessionActiveChanged();
    void accountsChanged();
    void errorChanged();
    void approvalChanged();
    void sessionSaved(int index);
    void reauthenticationRequested(const QString &accountName);

private:
    struct Session {
        int index;
        QString accountName;
        QDateTime createdAt;
        QDateTime expiresAt;
        QString tokenType;
    };
    const Session *find(int index) const;
    bool persist(const QList<Session> &sessions);
    void setBusy(bool value);
    void setError(const QString &message);
    void verifySession(int index, const QString &token);
    using AuthResponse = std::function<void(const QJsonObject &, bool, int)>;
    void sendAuthRequest(const QString &endpoint, const QVariantMap &values,
                         const QVariantMap &body, AuthResponse onReply);
    void handleLogin(const QJsonObject &response, bool ok, int status, bool retry);
    void beginApproval(const QString &workflowId, const QString &challengeId);
    void readInquiry();
    void readPrompt();
    void continueVerification();
    void retryLogin();
    void schedulePoll();
    void pollApproval();
    void finishLogin(const QJsonObject &response);
    void failLogin(const QString &message);
    void endApproval();

    enum class ApprovalStep { Inquiry, Prompt, Continue };

    RHNetworkEngine &m_network;
    QString m_metadataPath;
    RHCredentialStore *m_store;
    std::unique_ptr<RHCredentialStore> m_ownedStore;
    QList<Session> m_sessions;
    QTimer m_expiryTimer;
    QTimer m_pollTimer;
    QTimer m_approvalTimeout;
    QTimer m_countdownTimer;
    QElapsedTimer m_approvalClock;
    QPointer<QNetworkReply> m_reply;
    QVariantList m_accounts;
    QString m_error;
    QString m_deviceToken;
    QString m_deviceEmail;
    QString m_loginName;
    QVariantMap m_loginBody;
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
    bool m_loaded = false;
};
